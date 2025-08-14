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

#define MAX_KEYCODE 256
#define DEV_BASE "/dev/input/"

const char *key_names[MAX_KEYCODE] = {
    [0 ... MAX_KEYCODE-1] = "UNKNOWN",

    // Alphanumeric block
    [1] = "KEY_ESC",
    [2] = "KEY_1", [3] = "KEY_2", [4] = "KEY_3", [5] = "KEY_4",
    [6] = "KEY_5", [7] = "KEY_6", [8] = "KEY_7", [9] = "KEY_8",
    [10] = "KEY_9", [11] = "KEY_0",
    [12] = "KEY_MINUS", [13] = "KEY_EQUAL",
    [14] = "KEY_BACKSPACE", [15] = "KEY_TAB",
    [16] = "KEY_Q", [17] = "KEY_W", [18] = "KEY_E", [19] = "KEY_R",
    [20] = "KEY_T", [21] = "KEY_Y", [22] = "KEY_U", [23] = "KEY_I",
    [24] = "KEY_O", [25] = "KEY_P",
    [26] = "KEY_LEFTBRACE", [27] = "KEY_RIGHTBRACE",
    [28] = "KEY_ENTER", [29] = "KEY_LEFTCTRL",
    [30] = "KEY_A", [31] = "KEY_S", [32] = "KEY_D", [33] = "KEY_F",
    [34] = "KEY_G", [35] = "KEY_H", [36] = "KEY_J", [37] = "KEY_K",
    [38] = "KEY_L", [39] = "KEY_SEMICOLON", [40] = "KEY_APOSTROPHE",
    [41] = "KEY_GRAVE", [42] = "KEY_LEFTSHIFT", [43] = "KEY_BACKSLASH",
    [44] = "KEY_Z", [45] = "KEY_X", [46] = "KEY_C", [47] = "KEY_V",
    [48] = "KEY_B", [49] = "KEY_N", [50] = "KEY_M",
    [51] = "KEY_COMMA", [52] = "KEY_DOT", [53] = "KEY_SLASH",
    [54] = "KEY_RIGHTSHIFT", [55] = "KEY_KPASTERISK",
    [56] = "KEY_LEFTALT", [57] = "KEY_SPACE", [58] = "KEY_CAPSLOCK",

    // Function keys
    [59] = "KEY_F1", [60] = "KEY_F2", [61] = "KEY_F3", [62] = "KEY_F4",
    [63] = "KEY_F5", [64] = "KEY_F6", [65] = "KEY_F7", [66] = "KEY_F8",
    [67] = "KEY_F9", [68] = "KEY_F10", [87] = "KEY_F11", [88] = "KEY_F12",

    // Navigation / editing
    [99]  = "KEY_SYSRQ", [70] = "KEY_SCROLLLOCK", [119] = "KEY_PAUSE",
    [110] = "KEY_INSERT", [102] = "KEY_HOME", [104] = "KEY_PAGEUP",
    [111] = "KEY_DELETE", [107] = "KEY_END", [109] = "KEY_PAGEDOWN",
    [106] = "KEY_RIGHT", [105] = "KEY_LEFT",
    [108] = "KEY_DOWN", [103] = "KEY_UP",

    // Keypad
    [69]  = "KEY_NUMLOCK", [98]  = "KEY_KPSLASH", [55] = "KEY_KPASTERISK",
    [74]  = "KEY_KPMINUS", [78]  = "KEY_KPPLUS", [96] = "KEY_KPENTER",
    [79]  = "KEY_KP1", [80] = "KEY_KP2", [81] = "KEY_KP3",
    [75]  = "KEY_KP4", [76] = "KEY_KP5", [77] = "KEY_KP6",
    [71]  = "KEY_KP7", [72] = "KEY_KP8", [73] = "KEY_KP9",
    [82]  = "KEY_KP0", [83] = "KEY_KPDOT",

    // Extra modifiers
    [97]  = "KEY_RIGHTCTRL", [100] = "KEY_RIGHTALT",
    [125] = "KEY_LEFTMETA", [126] = "KEY_RIGHTMETA",
    [127] = "KEY_COMPOSE",
    
    // Multimedia / special keys
    [113] = "KEY_MUTE", [114] = "KEY_VOLUMEDOWN", [115] = "KEY_VOLUMEUP",
    [163] = "KEY_NEXTSONG", [164] = "KEY_PLAYPAUSE", [165] = "KEY_STOPCD",
    [166] = "KEY_PREVIOUSSONG",
    [140] = "KEY_CALC", [215] = "KEY_EMAIL", [172] = "KEY_HOMEPAGE",
    [217] = "KEY_SEARCH",
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL);
}

static void emit(int fd, int type, int code, int value, const struct timeval *tv) {
    struct input_event ev = {
        .type = type,
        .code = code,
        .value = value,
        .time = *tv
    };
    (void)write(fd, &ev, sizeof(ev));
}

static int setup_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Cannot open /dev/uinput");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int i = 0; i < MAX_KEYCODE; i++) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "debounced-virtual-keyboard");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    (void)write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}

static void print_help(const char *progname) {
    fprintf(stdout,
        "Usage: %s <input-device-id> [timeout_ms]\n"
        "\n"
        "Arguments:\n"
        "  <input-device-id>   Device name under %s (e.g. event3)\n"
        "  [timeout_ms]        Optional debounce interval in milliseconds (default: 15)\n"
        "\n"
        "Examples:\n"
        "  %s event3                       Run with 15ms debounce\n"
        "  %s event3 100                   Run with 100ms debounce\n"
        "  %s by-id/usb-*-event-kbd 40     Attach to keyboard by static ID with 40ms debounce\n",
        progname, DEV_BASE, progname, progname, progname
    );
}

static void arm_pending_timer(int tfd,
                              const uint8_t pending_up[MAX_KEYCODE],
                              const uint64_t debounce_until[MAX_KEYCODE]) {
    uint64_t now = now_ms();
    uint64_t min_due = UINT64_MAX;

    for (int k = 0; k < MAX_KEYCODE; k++) {
        if (pending_up[k] && debounce_until[k] < min_due) {
            min_due = debounce_until[k];
        }
    }

    struct itimerspec its = {0};
    if (min_due != UINT64_MAX) {
        int64_t diff_ms = (int64_t)(min_due - now);
        if (diff_ms < 0) diff_ms = 0;
        its.it_value.tv_sec  = (time_t)(diff_ms / 1000);
        its.it_value.tv_nsec = (long)((diff_ms % 1000) * 1000000L);
    } // else disarm (all zeros)

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
    }
}

int main(int argc, char *argv[]) {
    if (argc == 1 || (argc > 1 && strcmp(argv[1], "--help") == 0)) {
        print_help(argv[0]);
        return 0;
    }

    if (argc != 2 && argc != 3) {
        print_help(argv[0]);
        return 1;
    }

    int timeout_ms = 15;
    if (argc == 3) {
        timeout_ms = atoi(argv[2]);
        if (timeout_ms <= 0 || timeout_ms > 10000) {
            fprintf(stderr, "Invalid timeout: %d ms (must be between 1–10000)\n", timeout_ms);
            return 1;
        }
    }

    char device_path[PATH_MAX];
    snprintf(device_path, sizeof(device_path), DEV_BASE "%s", argv[1]);

    int in_fd = open(device_path, O_RDONLY);
    if (in_fd < 0) {
        perror("Failed to open input device");
        return 255;
    }

    if (ioctl(in_fd, EVIOCGRAB, 1) < 0) {
        perror("Failed to grab input device");
        close(in_fd);
        return 255;
    }

    int out_fd = setup_uinput();
    if (out_fd < 0) {
        ioctl(in_fd, EVIOCGRAB, 0);
        close(in_fd);
        return 255;
    }

    // Timer for flushing delayed UPs exactly when their debounce expires
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        perror("timerfd_create");
        ioctl(out_fd, UI_DEV_DESTROY);
        close(out_fd);
        ioctl(in_fd, EVIOCGRAB, 0);
        close(in_fd);
        return 255;
    }

    // State
    struct input_event ev;
    uint64_t debounce_until[MAX_KEYCODE]     = {0}; // last-down time + timeout
    uint8_t  key_state[MAX_KEYCODE]          = {0};
    uint8_t  seen_second_down[MAX_KEYCODE]   = {0};
    uint64_t last_event_time[MAX_KEYCODE]    = {0};

    // Delayed-UP tracking
    uint8_t        pending_up[MAX_KEYCODE]   = {0};
    struct timeval pending_up_time[MAX_KEYCODE];

    struct pollfd fds[2] = {
        { .fd = in_fd, .events = POLLIN },
        { .fd = tfd,   .events = POLLIN }
    };

    // Disarm timer initially
    arm_pending_timer(tfd, pending_up, debounce_until);

    for (;;) {
        int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // Timer fired: flush any pending UPs whose debounce expired
        if (fds[1].revents & POLLIN) {
            uint64_t expirations = 0;
            (void)read(tfd, &expirations, sizeof(expirations)); // drain

            uint64_t t_flush = now_ms();
            for (int k = 0; k < MAX_KEYCODE; k++) {
                if (pending_up[k] && t_flush >= debounce_until[k]) {
                    uint64_t time_since_press = t_flush + (uint64_t)timeout_ms - debounce_until[k];
                    uint64_t time_since_last  = (last_event_time[k] > 0) ? (t_flush - last_event_time[k]) : 0;

                    key_state[k]       = 0;
                    pending_up[k]      = 0;
                    seen_second_down[k]= 0; // reset bounce state after release
                    last_event_time[k] = t_flush;

                    printf("FLUSH UP: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                           key_names[k], k, time_since_press, time_since_last);
                    fflush(stdout);

                    emit(out_fd, EV_KEY, k, 0, &pending_up_time[k]);
                    emit(out_fd, EV_SYN, SYN_REPORT, 0, &pending_up_time[k]);
                }
            }
            // Re-arm timer to next pending
            arm_pending_timer(tfd, pending_up, debounce_until);
        }

        // Input device has data
        if (fds[0].revents & POLLIN) {
            ssize_t r = read(in_fd, &ev, sizeof(ev));
            if (r == 0) {
                // EOF on device
                break;
            }
            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                perror("Read failed");
                break;
            }
            if (r != sizeof(ev)) continue;

            if (ev.type != EV_KEY)
                continue;

            int code  = ev.code;
            int value = ev.value; // 0=up, 1=down, 2=autorepeat
            if (code < 0 || code >= MAX_KEYCODE)
                continue;

            // Allow volume knob events through unfiltered
            if (code == 113 || code == 114 || code == 115) {
                emit(out_fd, EV_KEY, code, value, &ev.time);
                emit(out_fd, EV_SYN, SYN_REPORT, 0, &ev.time);
                continue;
            }

            uint64_t t_now = now_ms();

            // If there is a pending UP for this key and its debounce has already expired,
            // flush it immediately before processing the new event for consistency.
            if (pending_up[code] && t_now >= debounce_until[code]) {
                uint64_t time_since_press = t_now + (uint64_t)timeout_ms - debounce_until[code];
                uint64_t time_since_last  = (last_event_time[code] > 0) ? (t_now - last_event_time[code]) : 0;

                key_state[code]        = 0;
                pending_up[code]       = 0;
                seen_second_down[code] = 0;
                last_event_time[code]  = t_now;

                printf("FLUSH UP (inline): %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                       key_names[code], code, time_since_press, time_since_last);
                fflush(stdout);

                emit(out_fd, EV_KEY, code, 0, &pending_up_time[code]);
                emit(out_fd, EV_SYN, SYN_REPORT, 0, &pending_up_time[code]);
                arm_pending_timer(tfd, pending_up, debounce_until);
            }

            uint64_t time_since_press = (debounce_until[code] > 0)
                ? (t_now + (uint64_t)timeout_ms - debounce_until[code])
                : 0;
            uint64_t time_since_last_event = (last_event_time[code] > 0)
                ? (t_now - last_event_time[code])
                : 0;
            last_event_time[code] = t_now;

            if (value == 1) { // Key down
                if (t_now < debounce_until[code]) {
                    // Second DOWN within debounce → treat as bounce, cancel any pending UP
                    seen_second_down[code] = 1;
                    if (pending_up[code]) {
                        pending_up[code] = 0; // continuous press, drop pending UP
                        printf("CANCEL PENDING UP due to second DOWN: %s (code %d)\n",
                               key_names[code], code);
                        fflush(stdout);
                        arm_pending_timer(tfd, pending_up, debounce_until);
                    }
                    printf("Debounced DOWN: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                           key_names[code], code, time_since_press, time_since_last_event);
                    fflush(stdout);
                    continue;
                }

                // New press
                debounce_until[code]   = t_now + (uint64_t)timeout_ms;
                seen_second_down[code] = 0;
                key_state[code]        = 1;

                printf("PASS DOWN: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                       key_names[code], code, time_since_press, time_since_last_event);
                fflush(stdout);

                emit(out_fd, EV_KEY, code, 1, &ev.time);
                emit(out_fd, EV_SYN, SYN_REPORT, 0, &ev.time);

            } else if (value == 0) { // Key up
                if (t_now < debounce_until[code]) {
                    if (!seen_second_down[code]) {
                        // Delay the UP until debounce expiry
                        pending_up[code]     = 1;
                        pending_up_time[code]= ev.time;

                        uint64_t wait_ms = debounce_until[code] - t_now;
                        printf("PENDING EARLY UP: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event, waiting %" PRIu64 "ms\n",
                               key_names[code], code, time_since_press, time_since_last_event, wait_ms);
                        fflush(stdout);

                        arm_pending_timer(tfd, pending_up, debounce_until);
                    } else {
                        // In a bounce sequence → ignore UP
                        printf("Debounced UP: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                               key_names[code], code, time_since_press, time_since_last_event);
                        fflush(stdout);
                    }
                } else {
                    // Normal UP after window
                    key_state[code] = 0;
                    printf("PASS UP: %s (code %d) at %" PRIu64 "ms after press, %" PRIu64 "ms since last event\n",
                           key_names[code], code, time_since_press, time_since_last_event);
                    fflush(stdout);

                    emit(out_fd, EV_KEY, code, 0, &ev.time);
                    emit(out_fd, EV_SYN, SYN_REPORT, 0, &ev.time);
                }

            } else if (value == 2) { // Auto-repeat
                emit(out_fd, EV_KEY, code, 2, &ev.time);
                emit(out_fd, EV_SYN, SYN_REPORT, 0, &ev.time);
            }
        }
    }

    perror("Read failed or terminated");

    // Cleanup
    close(tfd);
    ioctl(out_fd, UI_DEV_DESTROY);
    close(out_fd);
    ioctl(in_fd, EVIOCGRAB, 0);
    close(in_fd);
    return 255;
}
