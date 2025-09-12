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
#include <limits.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

#define MAX_KEYCODE 256
#define DEV_BASE "/dev/input/"
#define SOCKET_PATH "/run/debounced/debounced.sock"

const char *key_names[MAX_KEYCODE] = {
    [0 ... MAX_KEYCODE-1] = "UNKNOWN",
    [1] = "KEY_ESC", [2] = "KEY_1", [3] = "KEY_2", [4] = "KEY_3", [5] = "KEY_4",
    [6] = "KEY_5", [7] = "KEY_6", [8] = "KEY_7", [9] = "KEY_8",
    [10] = "KEY_9", [11] = "KEY_0", [14] = "KEY_BACKSPACE", [15] = "KEY_TAB",
    [16] = "KEY_Q", [17] = "KEY_W", [18] = "KEY_E", [19] = "KEY_R",
    [20] = "KEY_T", [21] = "KEY_Y", [22] = "KEY_U", [23] = "KEY_I",
    [24] = "KEY_O", [25] = "KEY_P", [28] = "KEY_ENTER", [29] = "KEY_LEFTCTRL",
    [30] = "KEY_A", [31] = "KEY_S", [32] = "KEY_D", [33] = "KEY_F",
    [34] = "KEY_G", [35] = "KEY_H", [36] = "KEY_J", [37] = "KEY_K",
    [38] = "KEY_L", [39] = "KEY_SEMICOLON", [40] = "KEY_APOSTROPHE",
    [41] = "KEY_GRAVE", [42] = "KEY_LEFTSHIFT", [43] = "KEY_BACKSLASH",
    [44] = "KEY_Z", [45] = "KEY_X", [46] = "KEY_C", [47] = "KEY_V",
    [48] = "KEY_B", [49] = "KEY_N", [50] = "KEY_M",
    [51] = "KEY_COMMA", [52] = "KEY_DOT", [53] = "KEY_SLASH",
    [54] = "KEY_RIGHTSHIFT", [55] = "KEY_KPASTERISK",
    [56] = "KEY_LEFTALT", [57] = "KEY_SPACE", [58] = "KEY_CAPSLOCK",
    [59] = "KEY_F1", [60] = "KEY_F2", [61] = "KEY_F3", [62] = "KEY_F4",
    [63] = "KEY_F5", [64] = "KEY_F6", [65] = "KEY_F7", [66] = "KEY_F8",
    [67] = "KEY_F9", [68] = "KEY_F10", [87] = "KEY_F11", [88] = "KEY_F12",
    [97] = "KEY_RIGHTCTRL", [100] = "KEY_RIGHTALT",
    [105] = "KEY_LEFT", [106] = "KEY_RIGHT",
    [125] = "KEY_LEFTMETA", [126] = "KEY_RIGHTMETA",
};

// ---------- FlashTap pair state ----------
typedef struct {
    int key1, key2;
    int active;
    int phys[2];
} FlashPair;

static FlashPair pair_ad = {30, 32, -1, {0,0}};    // A/D
static FlashPair pair_ar = {105,106, -1, {0,0}};   // Left/Right

// ---------- Globals ----------
static int debounce_ms = 15;
static char mode = 'd';
static int use_debounce = 1;
static int use_flashtap = 0;
static int ft_ad_enabled = 0;
static int ft_arrows_enabled = 0;

typedef struct {
    int pressed;
    int up_pending;
    uint64_t down_time;
    struct timeval up_time;
} KeyState;

static KeyState keys[MAX_KEYCODE];

// ---------- Helpers ----------
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL);
}

static void emit(int fd, int type, int code, int value, const struct timeval *tv) {
    struct input_event ev = { .type=type, .code=code, .value=value };
    if (tv) ev.time = *tv;
    else gettimeofday(&ev.time, NULL);
    write(fd, &ev, sizeof(ev));
}

static void emit_key(int fd, int code, int value, const struct timeval *tv) {
    emit(fd, EV_KEY, code, value, tv);
    emit(fd, EV_SYN, SYN_REPORT, 0, tv);
}

// ---------- uinput ----------
static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("uinput"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int i = 0; i < MAX_KEYCODE; i++) ioctl(fd, UI_SET_KEYBIT, i);
    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "debounced-virtual-keyboard");
    uidev.id.bustype = BUS_USB; uidev.id.vendor=0x1234; uidev.id.product=0x5678; uidev.id.version=1;
    write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}

// ---------- FlashTap logic ----------
static void handle_flashtap(FlashPair *fp, int code, int value, int out_fd, const struct timeval *tv) {
    if (!fp) return;
    int idx = (code == fp->key1) ? 0 : 1;
    int other = (idx == 0) ? fp->key2 : fp->key1;
    fp->phys[idx] = (value != 0);

    if (value == 1) { // down
        if (fp->active == other) {
            emit_key(out_fd, other, 0, tv);
            printf("[FT] override UP %-15s\n", key_names[other]);
        }
        fp->active = code;
        emit_key(out_fd, code, 1, tv);
        printf("[FT] DOWN %-15s\n", key_names[code]);
    } else if (value == 0) { // up
        if (fp->active == code) {
            fp->active = -1;
            emit_key(out_fd, code, 0, tv);
            printf("[FT] UP %-15s\n", key_names[code]);
            int other_idx = (idx == 0) ? 1 : 0;
            if (fp->phys[other_idx]) {
                fp->active = other;
                emit_key(out_fd, other, 1, tv);
                printf("[FT] restore DOWN %-15s\n", key_names[other]);
            }
        } else {
            printf("[FT] ignore UP %-15s\n", key_names[code]);
        }
    }
}

static FlashPair* ft_lookup(int code) {
    if (ft_ad_enabled && (code==pair_ad.key1 || code==pair_ad.key2)) return &pair_ad;
    if (ft_arrows_enabled && (code==pair_ar.key1 || code==pair_ar.key2)) return &pair_ar;
    return NULL;
}

// ---------- Mode constraints ----------
static void apply_mode_constraints(void) {
    if (mode == 'd') {
        use_debounce = 1;
        use_flashtap = 0;
        ft_ad_enabled = 0;
        ft_arrows_enabled = 0;
    } else if (mode == 'f') {
        use_debounce = 0;
        use_flashtap = 1;
        if (!ft_ad_enabled && !ft_arrows_enabled) {
            fprintf(stderr, "ERROR: FlashTap mode requires non-none FT pair\n");
            exit(1);
        }
    } else if (mode == 'b') {
        use_debounce = 1;
        use_flashtap = 1;
        if (!ft_ad_enabled && !ft_arrows_enabled) {
            fprintf(stderr, "ERROR: Both mode requires non-none FT pair\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "ERROR: invalid mode '%c'\n", mode);
        exit(1);
    }
}

// ---------- IPC parsing ----------
static int parse_start_cmd(char *line, char **dev) {
    char *tok = strtok(line, " \t\n");
    if (!tok || strcmp(tok, "START") != 0) return -1;
    *dev = strtok(NULL, " \t\n");
    char *t = strtok(NULL, " \t\n");
    char *m = strtok(NULL, " \t\n");
    char *pairs = strtok(NULL, " \t\n");
    if (!*dev || !t || !m || !pairs) {
        fprintf(stderr, "ERROR: START requires 4 args\n");
        return -1;
    }
    debounce_ms = atoi(t);
    mode = m[0];
    if (strcmp(pairs,"ad")==0) { ft_ad_enabled=1; ft_arrows_enabled=0; }
    else if (strcmp(pairs,"arrows")==0) { ft_ad_enabled=0; ft_arrows_enabled=1; }
    else if (strcmp(pairs,"both")==0) { ft_ad_enabled=1; ft_arrows_enabled=1; }
    else if (strcmp(pairs,"none")==0) { ft_ad_enabled=0; ft_arrows_enabled=0; }
    else { fprintf(stderr,"ERROR: invalid FT pair arg '%s'\n",pairs); return -1; }
    apply_mode_constraints();
    return 0;
}

// ---------- Processing ----------
static void process_key(int out_fd, int code, int value, const struct timeval *tv) {
    if (use_flashtap) {
        FlashPair *fp = ft_lookup(code);
        if (fp) { handle_flashtap(fp, code, value, out_fd, tv); return; }
    }
    if (use_debounce) {
        if (value == 1) { // down
            if (!keys[code].pressed) {
                emit_key(out_fd, code, 1, tv);
                keys[code].pressed = 1;
                keys[code].down_time = now_ms();
            }
        } else if (value == 0) { // up
            long long elapsed = now_ms() - keys[code].down_time;
            if (elapsed < debounce_ms) {
                keys[code].up_pending = 1;
                keys[code].up_time = *tv;
                printf("[DEBOUNCE] buffer UP %-15s\n", key_names[code]);
            } else {
                emit_key(out_fd, code, 0, tv);
                keys[code].pressed = 0;
            }
        }
    } else {
        emit_key(out_fd, code, value, tv);
    }
}

// ---------- main ----------
int main(void) {
    unlink(SOCKET_PATH);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock<0) { perror("socket"); return 1; }
    struct sockaddr_un addr={0};
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (bind(sock,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("bind"); return 1; }
    if (listen(sock,1)<0) { perror("listen"); return 1; }

    printf("Daemon ready, waiting for START...\n");

    for (;;) {
        int c = accept(sock,NULL,NULL);
        if (c<0) continue;
        char buf[512]; int r=read(c,buf,sizeof(buf)-1);
        if (r<=0) { close(c); continue; }
        buf[r]=0;
        char *dev=NULL;
        if (parse_start_cmd(buf,&dev)<0) { close(c); continue; }

        printf("Opening device %s, mode=%c, debounce=%dms, FT ad=%d, arrows=%d\n",
               dev, mode, debounce_ms, ft_ad_enabled, ft_arrows_enabled);

        int fd_in=open(dev,O_RDONLY);
        if(fd_in<0){ perror("input open"); close(c); continue; }
        if(ioctl(fd_in,EVIOCGRAB,1)<0){ perror("grab"); close(fd_in); close(c); continue; }
        int out_fd=setup_uinput();
        if(out_fd<0){ close(fd_in); close(c); continue; }

        struct pollfd fds[2]={{fd_in,POLLIN},{c,POLLIN}};
        for(;;){
            if(poll(fds,2,-1)<0) break;
            if(fds[1].revents&POLLIN){
                char cmd[64]; int rr=read(c,cmd,sizeof(cmd));
                if(rr>0 && strncmp(cmd,"STOP",4)==0){ printf("STOP received\n"); break; }
            }
            if(fds[0].revents&POLLIN){
                struct input_event ev; ssize_t rr=read(fd_in,&ev,sizeof(ev));
                if(rr<=0) continue;
                if(ev.type==EV_KEY && ev.code<MAX_KEYCODE) {
                    process_key(out_fd,ev.code,ev.value,&ev.time);
                }
            }
            // flush debounce ups
            for(int k=0;k<MAX_KEYCODE;k++) {
                if(keys[k].up_pending) {
                    long long elapsed=now_ms()-keys[k].down_time;
                    if(elapsed>=debounce_ms) {
                        emit_key(out_fd,k,0,&keys[k].up_time);
                        keys[k].up_pending=0;
                        keys[k].pressed=0;
                        printf("[DEBOUNCE] flush UP %-15s\n",key_names[k]);
                    }
                }
            }
        }

        close(fd_in); close(out_fd); close(c);
    }

    close(sock); unlink(SOCKET_PATH);
    return 0;
}
