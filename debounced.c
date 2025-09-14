#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <libevdev/libevdev.h>

// ---------- Status ----------
typedef struct {
    uint8_t status_byte;
    uint8_t timeout_ms;
} status_t;

static status_t g_status = {0};

// Bit masks
#define STATUS_RUNNING      0x80
#define STATUS_DEBOUNCE     0x08
#define STATUS_FLASHTAP     0x04
#define STATUS_PAIR_AD      0x02
#define STATUS_PAIR_ARROWS  0x01

#define MAX_KEYCODE 256
#define CONTROL_SOCKET_PATH "/run/debounced.sock"
#define MAX_DEBOUNCE_MS 100

// ---------- Globals ----------
typedef struct {
    int pressed;
    uint64_t down_time;
    int timerfd;        // -1 if inactive
} KeyState;

static KeyState keys[MAX_KEYCODE];
static int fd_in = -1, fd_out = -1, sock_fd = -1;
static pthread_t sock_thread;
static int running = 1;
static uint8_t debounce_ms = 50;
static char mode = 'd';
static int ft_ad_enabled = 0, ft_arrows_enabled = 0;
static int ft_active = -1;

// ---------- FlashTap ----------
typedef struct {
    int key1, key2;
    int phys[2];
} FlashPair;

static FlashPair pair_ad = {30, 32, {0,0}};      // A/D
static FlashPair pair_ar = {105,106, {0,0}};     // Left/Right

// ---------- Key map ----------
static const char *key_name(int code) {
    const char *n = libevdev_event_code_get_name(EV_KEY, code);
    return n ? n : "UNKNOWN";
}

// ---------- Helpers ----------
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL);
}

static void emit(int fd, int type, int code, int value, const struct timeval *tv) {
    struct input_event ev = { .type=type, .code=code, .value=value };
    if(tv) ev.time = *tv; else gettimeofday(&ev.time,NULL);
    (void)write(fd, &ev, sizeof(ev));
}

static void emit_key(int fd, int code, int value, const struct timeval *tv) {
    emit(fd, EV_KEY, code, value, tv);
    emit(fd, EV_SYN, SYN_REPORT, 0, tv);
}

// ---------- uinput ----------
static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd<0) {
        perror("uinput open");
        return -1;
    }

    if(ioctl(fd, UI_SET_EVBIT, EV_KEY)<0 || ioctl(fd, UI_SET_EVBIT, EV_SYN)<0) {
        perror("uinput ioctl");
        close(fd);
        return -1;
    }

    for(int i=0;i<MAX_KEYCODE;i++) ioctl(fd, UI_SET_KEYBIT, i);

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "debounced-virtual-keyboard");
    uidev.id.bustype = BUS_USB; uidev.id.vendor = 0x1234; uidev.id.product = 0x5678; uidev.id.version=1;
    (void)write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);

    return fd;
}

// ---------- FlashTap ----------
static void handle_flashtap(int code, int value) {
    FlashPair *fp = NULL;
    if(ft_ad_enabled && (code==pair_ad.key1 || code==pair_ad.key2)) fp = &pair_ad;
    else if(ft_arrows_enabled && (code==pair_ar.key1 || code==pair_ar.key2)) fp = &pair_ar;
    if(!fp) return;

    int idx = (code == fp->key1) ? 0 : 1;
    int other = (idx==0)?fp->key2:fp->key1;
    fp->phys[idx] = (value!=0);

    if(value==1) { // down
        if(ft_active==other) {
            emit_key(fd_out, other, 0, NULL);
            printf("[FT] Released %s due to %s press\n", key_name(other), key_name(code));
        }
        ft_active = code;
        emit_key(fd_out, code, 1, NULL);
        fprintf(stderr,"[FT] %s DOWN\n", key_name(code));
    } else if(value==0) { // up
        if(ft_active==code) {
            ft_active=-1;
            emit_key(fd_out, code, 0, NULL);
            printf("[FT] %s UP\n", key_name(code));
            if(fp->phys[1-idx]) {
                ft_active = other;
                emit_key(fd_out, other, 1, NULL);
                printf("[FT] %s state restored due to %s release\n", key_name(other), key_name(code));
            }
        }
    }
}

// ---------- Debounce with timerfd ----------
static void start_debounce_timer(int code, uint64_t delay_ms) {
    if(keys[code].timerfd >= 0) {
        close(keys[code].timerfd);
        keys[code].timerfd = -1;
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    if(tfd < 0) {
        perror("timerfd_create");
        return;
    }
    struct itimerspec its = {0};
    its.it_value.tv_sec  = delay_ms / 1000;
    its.it_value.tv_nsec = (delay_ms % 1000) * 1000000ULL;
    if(timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        close(tfd);
        return;
    }
    keys[code].timerfd = tfd;
}

static void process_debounce(int code, int value, const struct timeval *tv) {
    uint64_t now = now_ms();

    if (value == 1) { // down
        if (!keys[code].pressed) {
            emit_key(fd_out, code, 1, tv);
            keys[code].pressed = 1;
            keys[code].down_time = now;
        } else if (keys[code].timerfd >= 0) {
            // cancel pending release
            fprintf(stderr, "[DB] %s cancel pending up\n", key_name(code));
            close(keys[code].timerfd);
            keys[code].timerfd = -1;
        }
    } else if (value == 0) { // up
        uint64_t elapsed = now - keys[code].down_time;
        if (elapsed < debounce_ms) {
            uint64_t delay = debounce_ms - elapsed;
            start_debounce_timer(code, delay);
            fprintf(stderr, "[DB] %s pending up, %ums since press, flush at +%ums\n",
                    key_name(code), (unsigned)elapsed, (unsigned)delay);
        } else {
            emit_key(fd_out, code, 0, tv);
            keys[code].pressed = 0;
            if(keys[code].timerfd>=0) { close(keys[code].timerfd); keys[code].timerfd=-1; }
            fprintf(stderr, "[DB] %s immediate up\n", key_name(code));
        }
    } else if (value == 2) { // repeat
        emit_key(fd_out, code, 2, tv);
    }
}

// ---------- SIGTERM ----------
static void handle_sigterm(int signum) {
    (void)signum;
    for(int k=0;k<MAX_KEYCODE;k++) {
        if(keys[k].pressed) emit_key(fd_out,k,0,NULL);
        if(keys[k].timerfd>=0) close(keys[k].timerfd);
    }
    if(fd_out>=0) { ioctl(fd_out, UI_DEV_DESTROY); close(fd_out); fd_out=-1; }
    if(fd_in>=0) { close(fd_in); fd_in=-1; }
    if(sock_fd>=0) { close(sock_fd); sock_fd=-1; }
    exit(0);
}

// ---------- Socket handling ----------
static void *socket_thread_fn(void *arg) {
    (void)arg;
    struct sockaddr_un addr;
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock_fd<0) { perror("socket"); running=0; return NULL; }

    unlink(CONTROL_SOCKET_PATH);
    memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path, CONTROL_SOCKET_PATH, sizeof(addr.sun_path)-1);

    if(bind(sock_fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
        perror("bind"); close(sock_fd); running=0; return NULL;
    }

    chmod(CONTROL_SOCKET_PATH, 0666);
    if(listen(sock_fd,1)<0) { perror("listen"); close(sock_fd); running=0; return NULL; }

    while(running) {
        int c = accept(sock_fd,NULL,NULL);
        if(c<0) continue;
        
        char buf[128];
        int r = read(c, buf, sizeof(buf)-1);
        if(r <= 0) { close(c); continue; }
        buf[r] = '\0';
        char *cmd = strtok(buf, " \t\n");
        if(!cmd) { close(c); continue; }
        
        if(strcmp(cmd, "STOP") == 0) {
            if(!(g_status.status_byte & STATUS_RUNNING)) {
                fprintf(stderr, "STOP received but daemon not running, ignoring\n");
            } else {
                printf("STOP received\n");
                if(fd_in >= 0) { close(fd_in); fd_in = -1; }
                if(fd_out >= 0) { ioctl(fd_out, UI_DEV_DESTROY); close(fd_out); fd_out = -1; }
                g_status.status_byte = 0;
                g_status.timeout_ms = 0;
            }
        }
        else if(strcmp(cmd, "START") == 0) {
            if(g_status.status_byte & STATUS_RUNNING) {
                fprintf(stderr, "START received but daemon already running, ignoring\n");
                close(c);
                continue;
            }
        
            char *dev   = strtok(NULL, " \t\n");
            char *t     = strtok(NULL, " \t\n");
            char *m     = strtok(NULL, " \t\n");
            char *pairs = strtok(NULL, " \t\n");
            if(!dev || !t || !m || !pairs) { close(c); continue; }
        
            debounce_ms = (uint8_t)atoi(t);
            if(debounce_ms > MAX_DEBOUNCE_MS) debounce_ms = MAX_DEBOUNCE_MS;
            mode = m[0];
        
            ft_ad_enabled = ft_arrows_enabled = 0;
            if(strcmp(pairs,"ad")==0) ft_ad_enabled=1;
            else if(strcmp(pairs,"arrows")==0) ft_arrows_enabled=1;
            else if(strcmp(pairs,"both")==0) ft_ad_enabled=ft_arrows_enabled=1;
        
            printf("START %s mode=%c debounce=%ums FT ad=%d arrows=%d\n",
                    dev, mode, debounce_ms, ft_ad_enabled, ft_arrows_enabled);
            
            fd_in = open(dev,O_RDONLY);
            if(fd_in<0) { perror("input open"); close(c); continue; }
            if(ioctl(fd_in, EVIOCGRAB, 1)<0) { perror("grab"); close(fd_in); fd_in=-1; close(c); continue; }
            
            fd_out = setup_uinput();
            if(fd_out<0) { close(fd_in); fd_in=-1; close(c); continue; }
            
            g_status.status_byte = STATUS_RUNNING;
            if(mode=='d' || mode=='b') g_status.status_byte |= STATUS_DEBOUNCE;
            if(ft_ad_enabled || ft_arrows_enabled) g_status.status_byte |= STATUS_FLASHTAP;
            if(ft_ad_enabled) g_status.status_byte |= STATUS_PAIR_AD;
            if(ft_arrows_enabled) g_status.status_byte |= STATUS_PAIR_ARROWS;
            g_status.timeout_ms = debounce_ms;
        }
        else if(strcmp(cmd, "STATUS") == 0) {
            uint8_t outbuf[2] = { g_status.status_byte, g_status.timeout_ms };
            (void)write(c, outbuf, 2);
        }
    
        close(c);
    }

    return NULL;
}

// ---------- Main ----------
int main(void) {
    if(access("/dev/uinput",F_OK)!=0) {
        fprintf(stderr,"You need uinput support for this program to function.\n");
        return 2;
    }

    signal(SIGTERM, handle_sigterm);
    pthread_create(&sock_thread,NULL,socket_thread_fn,NULL);

    for(int i=0;i<MAX_KEYCODE;i++) keys[i].timerfd=-1;

    printf("Debounced daemon ready, waiting for commands...\n");

    while(running) {
        if(fd_in<0) { usleep(10000); continue; }

        struct pollfd pfds[MAX_KEYCODE+1];
        int nfds=0;
        pfds[nfds++] = (struct pollfd){ fd_in, POLLIN, 0 };
        for(int k=0;k<MAX_KEYCODE;k++)
            if(keys[k].timerfd>=0)
                pfds[nfds++] = (struct pollfd){ keys[k].timerfd, POLLIN, 0 };

        int ret = poll(pfds,nfds,-1);
        if(ret<=0) continue;

        // check input device
        if(pfds[0].revents & POLLIN) {
            struct input_event ev;
            ssize_t r = read(fd_in,&ev,sizeof(ev));
            if(r==sizeof(ev) && ev.type==EV_KEY && ev.code<MAX_KEYCODE) {
                if(ft_ad_enabled || ft_arrows_enabled) handle_flashtap(ev.code,ev.value);
                process_debounce(ev.code, ev.value, &ev.time);
            }
        }

        // check timers
        for(int i=1;i<nfds;i++) {
            if(pfds[i].revents & POLLIN) {
                uint64_t expir;
                (void)read(pfds[i].fd,&expir,sizeof(expir));
                for(int k=0;k<MAX_KEYCODE;k++) {
                    if(keys[k].timerfd==pfds[i].fd) {
                        emit_key(fd_out,k,0,NULL);
                        keys[k].pressed=0;
                        close(keys[k].timerfd);
                        keys[k].timerfd=-1;
                        fprintf(stderr,"[DB] %s flush UP after %ums\n", key_name(k), debounce_ms);
                        break;
                    }
                }
            }
        }
    }
    return 0;
}