#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// ---------- Status ----------
typedef struct {
    uint8_t status_byte;
    uint8_t timeout_ms;
} status_t;
static status_t g_status = {0};

// ---------- Command channel ----------
typedef enum {
    CMD_NONE = 0,
    CMD_START,
    CMD_STOP,
    CMD_STATUS
} cmd_type_t;

#define DEVICE_PATH_MAX 255

typedef struct {
    cmd_type_t type;
    char device[DEVICE_PATH_MAX];
    uint8_t timeout_ms;
    char mode;
    char ftpair[16];
} pending_cmd_t;

static pending_cmd_t g_cmd = {0};
static uint8_t g_cmd_result = 0;
static pthread_mutex_t g_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cmd_pending = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_cmd_done = PTHREAD_COND_INITIALIZER;

// ---------- Bit masks ----------
#define STATUS_RUNNING 0x80
#define STATUS_DEBOUNCE 0x08
#define STATUS_FLASHTAP 0x04
#define STATUS_PAIR_AD 0x02
#define STATUS_PAIR_ARROWS 0x01

#define MAX_KEYCODE 256
#define CONTROL_SOCKET_PATH "/run/debounced.sock"
#define MAX_DEBOUNCE_MS 250

// ---------- Globals ----------
typedef struct {
    int pressed;
    unsigned long long down_time;
    int timerfd;
    unsigned long long last_event_ms;
} KeyState;

static KeyState keys[MAX_KEYCODE];
static int fd_in = -1, fd_out = -1, sock_fd = -1;
static pthread_t sock_thread;
static atomic_int running = 1;
static atomic_int shutdown_requested = 0;
static uint8_t debounce_ms = 50;
static char mode = 'd';
static int ft_ad_enabled = 0, ft_arrows_enabled = 0;
static int ft_active_ad = -1, ft_active_arrows = -1;
static int verbose = 0;
static unsigned long long start_time_ms = 0;

// ---------- FlashTap struct ----------
typedef struct {
    int key1, key2;
    int phys[2];
} FlashPair;
static FlashPair pair_ad = {30, 32, {0, 0}};
static FlashPair pair_ar = {105, 106, {0, 0}};

// ---------- Key map ----------
static const char *key_name(int code) {
    const char *n = libevdev_event_code_get_name(EV_KEY, code);
    return n ? n : "UNKNOWN";
}

// ---------- Mini helpers ----------
static unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((unsigned long long)ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL);
}

static void emit(int fd, int type, int code, int value, const struct timeval *tv) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (tv)
        ev.time = *tv;
    else
        gettimeofday(&ev.time, NULL);
    ssize_t ret = write(fd, &ev, sizeof(ev));
    (void)ret;
}

static void emit_key(int fd, int code, int value, const struct timeval *tv) {
    emit(fd, EV_KEY, code, value, tv);
    emit(fd, EV_SYN, SYN_REPORT, 0, tv);
}

// ---------- State reset function ----------
static void reset_state(void) {
    // Release grabbed input device
    if (fd_in >= 0) {
        if (ioctl(fd_in, EVIOCGRAB, 0) < 0) {
            perror("release grab");
        }
        close(fd_in);
        fd_in = -1;
    }
    // Destroy uinput device
    if (fd_out >= 0) {
        if (ioctl(fd_out, UI_DEV_DESTROY) < 0) {
            perror("destroy uinput device");
        }
        close(fd_out);
        fd_out = -1;
    }
    // Clear key states
    for (int k = 0; k < MAX_KEYCODE; k++) {
        if (keys[k].timerfd >= 0) {
            close(keys[k].timerfd);
            keys[k].timerfd = -1;
        }
        keys[k].pressed = 0;
        keys[k].down_time = 0;
        keys[k].last_event_ms = 0;
    }
    ft_active_ad = ft_active_arrows = -1;
    g_status.status_byte = 0;
    g_status.timeout_ms = 0;
}

// ---------- Virtual keyboard instantiation ----------
static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("uinput open");
        return -1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("uinput ioctl");
        close(fd);
        return -1;
    }
    for (int i = 0; i < MAX_KEYCODE; i++) ioctl(fd, UI_SET_KEYBIT, i);
    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "debounced-virtual-keyboard");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;
    ssize_t ret = write(fd, &uidev, sizeof(uidev));
    (void)ret;
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}

// ---------- FlashTap logic ----------
static void handle_flashtap(int code, int value) {
    FlashPair *fp = NULL;
    int *ft_active_ptr = NULL;
    if (ft_ad_enabled && (code == pair_ad.key1 || code == pair_ad.key2)) {
        fp = &pair_ad;
        ft_active_ptr = &ft_active_ad;
    } else if (ft_arrows_enabled && (code == pair_ar.key1 || code == pair_ar.key2)) {
        fp = &pair_ar;
        ft_active_ptr = &ft_active_arrows;
    }
    if (!fp) return;
    int idx = (code == fp->key1) ? 0 : 1;
    int other = (idx == 0) ? fp->key2 : fp->key1;
    fp->phys[idx] = (value != 0);
    if (value == 1) {  // down
        if (*ft_active_ptr == other) {
            emit_key(fd_out, other, 0, NULL);
            printf("[FT] Released %s due to %s press\n", key_name(other), key_name(code));
        }
        *ft_active_ptr = code;
        emit_key(fd_out, code, 1, NULL);
        if (verbose) fprintf(stderr, "[FT] %s DOWN\n", key_name(code));
    } else if (value == 0) {  // up
        if (*ft_active_ptr == code) {
            *ft_active_ptr = -1;
            emit_key(fd_out, code, 0, NULL);
            if (verbose) printf("[FT] %s UP\n", key_name(code));
            if (fp->phys[1 - idx]) {
                *ft_active_ptr = other;
                emit_key(fd_out, other, 1, NULL);
                printf("[FT] %s state restored to %s due to %s release\n", key_name(other),
                       fp->phys[1 - idx] ? "DOWN" : "UP", key_name(code));
            }
        }
    }
}

// ---------- Post-debounce event router ----------
static void post_debounce_event(int code, int value, const struct timeval *tv) {
    if ((ft_ad_enabled && (code == pair_ad.key1 || code == pair_ad.key2)) ||
        (ft_arrows_enabled && (code == pair_ar.key1 || code == pair_ar.key2))) {
        handle_flashtap(code, value);
    } else {
        emit_key(fd_out, code, value, tv);
    }
}

// ---------- Debounce timers ----------
static void start_debounce_timer(int code, unsigned long long delay_ms) {
    if (keys[code].timerfd >= 0) {
        close(keys[code].timerfd);
        keys[code].timerfd = -1;
    }
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        perror("timerfd_create");
        return;
    }
    struct itimerspec its = {0};
    its.it_value.tv_sec = delay_ms / 1000;
    its.it_value.tv_nsec = (delay_ms % 1000) * 1000000ULL;
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        close(tfd);
        return;
    }
    keys[code].timerfd = tfd;
}

// ---------- Debounce processing ----------
static void process_debounce(int code, int value, const struct timeval *tv) {
    unsigned long long now = now_ms();
    unsigned long long delta = now - keys[code].last_event_ms;  // compute delta before updating
    keys[code].last_event_ms = now;
    if (value == 1) {  // DOWN
        if (now - start_time_ms < debounce_ms) {
            printf("[DB] Ignored %s DOWN during startup delay\n", key_name(code));
            return;  // drop the event
        }
        if (!keys[code].pressed) {
            post_debounce_event(code, 1, tv);
            keys[code].pressed = 1;
            keys[code].down_time = now;
            keys[code].timerfd = -1;
            if (verbose) printf("[DB] %s DOWN, %llu ms since last event\n", key_name(code), delta);
        } else if (keys[code].timerfd > 0) {
            close(keys[code].timerfd);
            keys[code].timerfd = -1;
            if (!verbose)
                printf("[DB] %s DOWN canceled pending UP\n", key_name(code));  // quiet mode
            else
                printf("[DB] %s DOWN canceled pending UP, %llu ms since last event\n", key_name(code), delta);
        }
    } else if (value == 0) {  // UP
        unsigned long long elapsed = now - keys[code].down_time;
        if (elapsed < debounce_ms) {
            // arm timer using helper
            start_debounce_timer(code, debounce_ms - elapsed);
            if (verbose)
                printf("[DB] %s UP pending, %llu ms since press, flush after %ums, %llu ms since last event\n",
                       key_name(code), (unsigned long long)elapsed, (unsigned)(debounce_ms - elapsed), delta);
        } else {
            post_debounce_event(code, 0, tv);
            keys[code].pressed = 0;
            keys[code].timerfd = -1;
            if (verbose) printf("[DB] %s UP immediate, %llu ms since last event\n", key_name(code), delta);
        }
    } else if (value == 2) {  // REPEAT
        if (keys[code].pressed) {
            emit_key(fd_out, code, 2, tv);
            if (verbose) printf("[DB] %s REPEAT\n", key_name(code));
        } else {
            if (verbose) printf("[DB] Ignored %s REPEAT (key not pressed)\n", key_name(code));
        }
    }
}

// ---------- SIGTERM handler ----------
static void handle_sigterm(int signum) {
    (void)signum;
    atomic_store(&shutdown_requested, 1);
}

// ---------- Socket handler thread ----------
static void *socket_thread_fn(void *arg) {
    (void)arg;
    struct sockaddr_un addr;
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        atomic_store(&running, 0);
        return NULL;
    }

    unlink(CONTROL_SOCKET_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_fd);
        atomic_store(&running, 0);
        return NULL;
    }

    chmod(CONTROL_SOCKET_PATH, 0666);

    if (listen(sock_fd, 1) < 0) {
        perror("listen");
        close(sock_fd);
        atomic_store(&running, 0);
        return NULL;
    }

    while (atomic_load(&running)) {
        int c = accept(sock_fd, NULL, NULL);
        if (c < 0) continue;

        char buf[512];
        int r = read(c, buf, sizeof(buf) - 1);
        if (r <= 0) { close(c); continue; }
        buf[r] = '\0';

        char *cmd = strtok(buf, " \t\n");
        if (!cmd) { close(c); continue; }

        pthread_mutex_lock(&g_cmd_mutex);

        if (strncasecmp(cmd, "STOP", 4) == 0) {
            g_cmd.type = CMD_STOP;
        } else if (strncasecmp(cmd, "START", 5) == 0) {
            char *dev  = strtok(NULL, " \t\n");
            char *t    = strtok(NULL, " \t\n");
            char *m    = strtok(NULL, " \t\n");
            char *pair = strtok(NULL, " \t\n");
            if (!dev || !t || !m || !pair) {
                pthread_mutex_unlock(&g_cmd_mutex);
                close(c);
                continue;
            }
            g_cmd.type = CMD_START;
            strncpy(g_cmd.device, dev, DEVICE_PATH_MAX - 1);
            g_cmd.device[DEVICE_PATH_MAX - 1] = '\0';
            g_cmd.timeout_ms = (uint8_t)atoi(t);
            g_cmd.mode = m[0];
            strncpy(g_cmd.ftpair, pair, sizeof(g_cmd.ftpair) - 1);
            g_cmd.ftpair[sizeof(g_cmd.ftpair) - 1] = '\0';
        } else if (strncasecmp(cmd, "STATUS", 6) == 0) {
            g_cmd.type = CMD_STATUS;
        } else {
            pthread_mutex_unlock(&g_cmd_mutex);
            close(c);
            continue;
        }

            cmd_type_t saved_type = g_cmd.type;

            // Signal main() and wait for result
            pthread_cond_signal(&g_cmd_pending);
            pthread_cond_wait(&g_cmd_done, &g_cmd_mutex);

            uint8_t result = g_cmd_result;
            pthread_mutex_unlock(&g_cmd_mutex);

            // Send response
            if (saved_type == CMD_STATUS) {
            uint8_t outbuf[2] = {g_status.status_byte, g_status.timeout_ms};
            ssize_t ret = write(c, outbuf, 2);
            (void)ret;
        } else {
            ssize_t ret = write(c, &result, 1);
            (void)ret;
        }

        close(c);
    }

    return NULL;
}

// ---------- Main program loop ----------
int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
            verbose = 1;
        }
    }
    if (access("/dev/uinput", F_OK) != 0) {
        fprintf(stderr, "You need uinput support for this program to function.\n");
        return 2;
    }
    for (int i = 0; i < MAX_KEYCODE; i++) keys[i].timerfd = -1;
    signal(SIGTERM, handle_sigterm);
    pthread_create(&sock_thread, NULL, socket_thread_fn, NULL);
    printf("Debounced daemon ready%s.\n", verbose ? " (verbose)" : "");
    while (atomic_load(&running)) {
        if (atomic_load(&shutdown_requested)) {
            reset_state();
            break;
        }

        if (fd_in < 0) {
            // Idle — wait for a command
            pthread_mutex_lock(&g_cmd_mutex);
            while (g_cmd.type == CMD_NONE && atomic_load(&running))
                pthread_cond_wait(&g_cmd_pending, &g_cmd_mutex);
            pthread_mutex_unlock(&g_cmd_mutex);
            // fall through to command processing below
        }

        // Check for pending command
        pthread_mutex_lock(&g_cmd_mutex);
        if (g_cmd.type != CMD_NONE) {
            switch (g_cmd.type) {
                case CMD_START:
                    if (g_status.status_byte & STATUS_RUNNING) {
                        fprintf(stderr, "START received but daemon already running, ignoring.\n");
                        g_cmd_result = 1;
                        break;
                    }

                    debounce_ms = g_cmd.timeout_ms;
                    if (debounce_ms > MAX_DEBOUNCE_MS) debounce_ms = MAX_DEBOUNCE_MS;
                    mode = g_cmd.mode;
                
                    ft_ad_enabled = ft_arrows_enabled = 0;
                    if (strcasecmp(g_cmd.ftpair, "ad") == 0)
                        ft_ad_enabled = 1;
                    else if (strcasecmp(g_cmd.ftpair, "arrows") == 0)
                        ft_arrows_enabled = 1;
                    else if (strcasecmp(g_cmd.ftpair, "both") == 0)
                        ft_ad_enabled = ft_arrows_enabled = 1;

                    printf("START %s mode=%c debounce=%ums FT ad=%d arrows=%d\n",
                           g_cmd.device, mode, debounce_ms, ft_ad_enabled, ft_arrows_enabled);

                    fd_in = open(g_cmd.device, O_RDONLY);
                    if (fd_in < 0) {
                        perror("input open");
                        g_cmd_result = 1;
                        break;
                    }

                    if (ioctl(fd_in, EVIOCGRAB, 1) < 0) {
                        perror("grab");
                        close(fd_in);
                        fd_in = -1;
                        g_cmd_result = 1;
                        break;
                    }

                    fd_out = setup_uinput();
                    if (fd_out < 0) {
                        close(fd_in);
                        fd_in = -1;
                        g_cmd_result = 1;
                        break;
                    }

                    // Reset key state
                    ft_active_ad = ft_active_arrows = -1;
                    start_time_ms = now_ms();
                    for (int k = 0; k < MAX_KEYCODE; k++) {
                        keys[k].pressed = 0;
                        keys[k].down_time = 0;
                        if (keys[k].timerfd >= 0) {
                            close(keys[k].timerfd);
                            keys[k].timerfd = -1;
                        }
                        keys[k].last_event_ms = 0;
                    }

                    g_status.status_byte = STATUS_RUNNING;
                    if (mode == 'd' || mode == 'b') g_status.status_byte |= STATUS_DEBOUNCE;
                    if (ft_ad_enabled || ft_arrows_enabled) g_status.status_byte |= STATUS_FLASHTAP;
                    if (ft_ad_enabled) g_status.status_byte |= STATUS_PAIR_AD;
                    if (ft_arrows_enabled) g_status.status_byte |= STATUS_PAIR_ARROWS;
                    g_status.timeout_ms = debounce_ms;

                    g_cmd_result = 0;
                    break;
                case CMD_STOP:
                    if (!(g_status.status_byte & STATUS_RUNNING)) {
                        fprintf(stderr, "STOP received but daemon not running, ignoring.\n");
                        g_cmd_result = 1;
                    } else {
                        printf("STOP received, cleaning up and releasing device nodes.\n");
                        reset_state();
                        g_cmd_result = 0;
                    }
                    break;
                case CMD_STATUS:
                    g_cmd_result = 0; // result unused for STATUS, socket thread reads g_status directly
                    break;
                default:
                    break;
            }
            g_cmd.type = CMD_NONE;
            pthread_cond_signal(&g_cmd_done);
        }
        pthread_mutex_unlock(&g_cmd_mutex);
        if (fd_in < 0) continue;
        int version;
        // Check if device disappeared
        if (ioctl(fd_in, EVIOCGVERSION, &version) < 0) {
            fprintf(stderr, "Keyboard device disappeared. Stopping and resetting state.\n");
            reset_state();
            continue;
        }
        struct pollfd pfds[MAX_KEYCODE + 1];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){fd_in, POLLIN, 0};
        for (int k = 0; k < MAX_KEYCODE; k++)
            if (keys[k].timerfd >= 0) pfds[nfds++] = (struct pollfd){keys[k].timerfd, POLLIN, 0};
        int ret = poll(pfds, nfds, -1);
        if (ret <= 0) continue;
        // check input device
        if (pfds[0].revents & POLLIN) {
            struct input_event ev;
            ssize_t r = read(fd_in, &ev, sizeof(ev));
            if (r == sizeof(ev) && ev.type == EV_KEY && ev.code < MAX_KEYCODE) {
                process_debounce(ev.code, ev.value, &ev.time);
            }
        }
        // check timers
        for (int i = 1; i < nfds; i++) {
            if (pfds[i].revents & POLLIN) {
                unsigned long long expir;
                ssize_t ret = read(pfds[i].fd, &expir, sizeof(expir));
                (void)ret;
                for (int k = 0; k < MAX_KEYCODE; k++) {
                    if (keys[k].timerfd == pfds[i].fd) {
                        unsigned long long now = now_ms();
                        unsigned long long delta = now - keys[k].last_event_ms;
                        post_debounce_event(k, 0, NULL);
                        keys[k].pressed = 0;
                        close(keys[k].timerfd);
                        keys[k].timerfd = -1;
                        if (verbose)
                            printf("[DB] %s flush UP after %u ms, %llu ms since last event\n", key_name(k), debounce_ms,
                                   delta);
                        keys[k].last_event_ms = now;
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
