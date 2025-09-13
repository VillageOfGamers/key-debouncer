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

#define MAX_KEYCODE 256
#define CONTROL_SOCKET_PATH "/run/debounced.sock"
#define MAX_DEBOUNCE_MS 100

// ---------- Globals ----------
typedef struct {
    int pressed;
    int up_pending;
    uint64_t down_time;
    struct timeval up_time;
} KeyState;

static KeyState keys[MAX_KEYCODE];
static int fd_in = -1, fd_out = -1, sock_fd = -1;
static pthread_t sock_thread;
static int running = 1;
static uint8_t debounce_ms = 15;
static char mode = 'd';
static int ft_ad_enabled = 0, ft_arrows_enabled = 0;
static int ft_active = -1;

// ---------- FlashTap pair state ----------
typedef struct {
    int key1, key2;
} FlashPair;

static FlashPair pair_ad = {30, 32};    // A/D
static FlashPair pair_ar = {105,106};   // Left/Right

// ---------- Key map ----------
static const char *key_names[MAX_KEYCODE] = {
    [0] = "KEY_RESERVED",
    [1] = "KEY_ESC",
    [2] = "KEY_1",
    [3] = "KEY_2",
    [4] = "KEY_3",
    [5] = "KEY_4",
    [6] = "KEY_5",
    [7] = "KEY_6",
    [8] = "KEY_7",
    [9] = "KEY_8",
    [10] = "KEY_9",
    [11] = "KEY_0",
    [12] = "KEY_MINUS",
    [13] = "KEY_EQUAL",
    [14] = "KEY_BACKSPACE",
    [15] = "KEY_TAB",
    [16] = "KEY_Q",
    [17] = "KEY_W",
    [18] = "KEY_E",
    [19] = "KEY_R",
    [20] = "KEY_T",
    [21] = "KEY_Y",
    [22] = "KEY_U",
    [23] = "KEY_I",
    [24] = "KEY_O",
    [25] = "KEY_P",
    [26] = "KEY_LEFTBRACE",
    [27] = "KEY_RIGHTBRACE",
    [28] = "KEY_ENTER",
    [29] = "KEY_LEFTCTRL",
    [30] = "KEY_A",
    [31] = "KEY_S",
    [32] = "KEY_D",
    [33] = "KEY_F",
    [34] = "KEY_G",
    [35] = "KEY_H",
    [36] = "KEY_J",
    [37] = "KEY_K",
    [38] = "KEY_L",
    [39] = "KEY_SEMICOLON",
    [40] = "KEY_APOSTROPHE",
    [41] = "KEY_GRAVE",
    [42] = "KEY_LEFTSHIFT",
    [43] = "KEY_BACKSLASH",
    [44] = "KEY_Z",
    [45] = "KEY_X",
    [46] = "KEY_C",
    [47] = "KEY_V",
    [48] = "KEY_B",
    [49] = "KEY_N",
    [50] = "KEY_M",
    [51] = "KEY_COMMA",
    [52] = "KEY_DOT",
    [53] = "KEY_SLASH",
    [54] = "KEY_RIGHTSHIFT",
    [55] = "KEY_KPASTERISK",
    [56] = "KEY_LEFTALT",
    [57] = "KEY_SPACE",
    [58] = "KEY_CAPSLOCK",
    [59] = "KEY_F1",
    [60] = "KEY_F2",
    [61] = "KEY_F3",
    [62] = "KEY_F4",
    [63] = "KEY_F5",
    [64] = "KEY_F6",
    [65] = "KEY_F7",
    [66] = "KEY_F8",
    [67] = "KEY_F9",
    [68] = "KEY_F10",
    [69] = "KEY_NUMLOCK",
    [70] = "KEY_SCROLLLOCK",
    [71] = "KEY_KP7",
    [72] = "KEY_KP8",
    [73] = "KEY_KP9",
    [74] = "KEY_KPMINUS",
    [75] = "KEY_KP4",
    [76] = "KEY_KP5",
    [77] = "KEY_KP6",
    [78] = "KEY_KPPLUS",
    [79] = "KEY_KP1",
    [80] = "KEY_KP2",
    [81] = "KEY_KP3",
    [82] = "KEY_KP0",
    [83] = "KEY_KPDOT",
    [84] = "KEY_ZENKAKUHANKAKU",
    [85] = "KEY_102ND",
    [86] = "KEY_F11",
    [87] = "KEY_F12",
    [88] = "KEY_RO",
    [89] = "KEY_KATAKANA",
    [90] = "KEY_HIRAGANA",
    [91] = "KEY_HENKAN",
    [92] = "KEY_KATAKANAHIRAGANA",
    [93] = "KEY_MUHENKAN",
    [94] = "KEY_KPJPCOMMA",
    [95] = "KEY_KPENTER",
    [96] = "KEY_RIGHTCTRL",
    [97] = "KEY_KPSLASH",
    [98] = "KEY_SYSRQ",
    [99] = "KEY_RIGHTALT",
    [100] = "KEY_LINEFEED",
    [101] = "KEY_HOME",
    [102] = "KEY_UP",
    [103] = "KEY_PAGEUP",
    [104] = "KEY_LEFT",
    [105] = "KEY_RIGHT",
    [106] = "KEY_END",
    [107] = "KEY_DOWN",
    [108] = "KEY_PAGEDOWN",
    [109] = "KEY_INSERT",
    [110] = "KEY_DELETE",
    [111] = "KEY_MACRO",
    [112] = "KEY_MUTE",
    [113] = "KEY_VOLUMEDOWN",
    [114] = "KEY_VOLUMEUP",
    [115] = "KEY_POWER",
    [116] = "KEY_KPEQUAL",
    [117] = "KEY_KPPLUSMINUS",
    [118] = "KEY_PAUSE",
    [119] = "KEY_SCALE",
    [120] = "KEY_KPCOMMA",
    [121] = "KEY_HANGEUL",
    [122] = "KEY_HANJA",
    [123] = "KEY_YEN",
    [124] = "KEY_LEFTMETA",
    [125] = "KEY_RIGHTMETA",
    [126] = "KEY_COMPOSE",
    [127] = "KEY_STOP",
    [128] = "KEY_AGAIN",
    [129] = "KEY_PROPS",
    [130] = "KEY_UNDO",
    [131] = "KEY_FRONT",
    [132] = "KEY_COPY",
    [133] = "KEY_OPEN",
    [134] = "KEY_PASTE",
    [135] = "KEY_FIND",
    [136] = "KEY_CUT",
    [137] = "KEY_HELP",
    [138] = "KEY_MENU",
    [139] = "KEY_CALC",
    [140] = "KEY_SETUP",
    [141] = "KEY_SLEEP",
    [142] = "KEY_WAKEUP",
    [143] = "KEY_FILE",
    [144] = "KEY_SENDFILE",
    [145] = "KEY_DELETEFILE",
    [146] = "KEY_XFER",
    [147] = "KEY_PROG1",
    [148] = "KEY_PROG2",
    [149] = "KEY_WWW",
    [150] = "KEY_MSDOS",
    [151] = "KEY_COFFEE",
    [152] = "KEY_ROTATE_DISPLAY",
    [153] = "KEY_DIRECTION",
    [154] = "KEY_CYCLEWINDOWS",
    [155] = "KEY_MAIL",
    [156] = "KEY_BOOKMARKS",
    [157] = "KEY_COMPUTER",
    [158] = "KEY_BACK",
    [159] = "KEY_FORWARD",
    [160] = "KEY_CLOSECD",
    [161] = "KEY_EJECTCD",
    [162] = "KEY_EJECTCLOSECD",
    [163] = "KEY_NEXTSONG",
    [164] = "KEY_PLAYPAUSE",
    [165] = "KEY_PREVIOUSSONG",
    [166] = "KEY_STOPCD",
    [167] = "KEY_RECORD",
    [168] = "KEY_REWIND",
    [169] = "KEY_PHONE",
    [170] = "KEY_ISO",
    [171] = "KEY_CONFIG",
    [172] = "KEY_HOMEPAGE",
    [173] = "KEY_REFRESH",
    [174] = "KEY_EXIT",
    [175] = "KEY_MOVE",
    [176] = "KEY_EDIT",
    [177] = "KEY_SCROLLUP",
    [178] = "KEY_SCROLLDOWN",
    [179] = "KEY_KPLEFTPAREN",
    [180] = "KEY_KPRIGHTPAREN",
    [181 ... 255] = "UNKNOWN_KEY"
};

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
    static int phys[2] = {0,0};
    phys[idx] = (value!=0);

    if(value==1) { // down
        if(ft_active==other) {
            emit_key(fd_out, other, 0, NULL);
            fprintf(stderr,"[FT] Released %s due to %s press\n", key_names[other], key_names[code]);
        }
        ft_active = code;
        emit_key(fd_out, code, 1, NULL);
        fprintf(stderr,"[FT] DOWN %s\n", key_names[code]);
    } else if(value==0) { // up
        if(ft_active==code) {
            ft_active=-1;
            emit_key(fd_out, code, 0, NULL);
            fprintf(stderr,"[FT] UP %s\n", key_names[code]);
            if(phys[1-idx]) {
                ft_active = other;
                emit_key(fd_out, other, 1, NULL);
                fprintf(stderr,"[FT] %s state restored due to %s release\n", key_names[other], key_names[code]);
            }
        }
    }
}

// ---------- Debounce ----------
static void process_debounce(int fd_in, int code, int value) {
    if(value==2) { // repeat
        emit_key(fd_out, code, 2, NULL);
        return;
    }

    uint64_t now = now_ms();

    if(value==1) { // down
        if(!keys[code].pressed) {
            emit_key(fd_out, code, 1, NULL);
            keys[code].pressed = 1;
            keys[code].down_time = now;
        } else if(keys[code].up_pending) {
            fprintf(stderr,"[DB] %s cancel up, new down %ums before timer expiry\n",
                    key_names[code], (uint8_t)(now - keys[code].down_time));
            keys[code].up_pending = 0;
        }
    } else if(value==0) { // up
        uint8_t elapsed = (uint8_t)(now - keys[code].down_time);
        if(elapsed<debounce_ms) {
            keys[code].up_pending = 1;
            gettimeofday(&keys[code].up_time,NULL);
            uint8_t remaining = debounce_ms - elapsed;

            fprintf(stderr,"[DB] %s pending up, %ums since press, waiting %ums\n",
                    key_names[code], elapsed, remaining);

            struct pollfd pfd = { .fd = fd_in, .events = POLLIN };
            while(remaining--) {
                int ret = poll(&pfd, 1, 1);
                if(ret>0 && (pfd.revents & POLLIN)) {
                    struct input_event ev;
                    ssize_t r = read(fd_in,&ev,sizeof(ev));
                    if(r==sizeof(ev) && ev.type==EV_KEY && ev.code==code) {
                        if(ev.value==1) {
                            fprintf(stderr,"[DB] %s cancel up due to new down\n", key_names[code]);
                            keys[code].up_pending = 0;
                            return;
                        } else if(ev.value==2) {
                            emit_key(fd_out, code, 2, &ev.time);
                        }
                    }
                }
                usleep(1000);
            }

            emit_key(fd_out, code, 0, &keys[code].up_time);
            keys[code].up_pending = 0;
            keys[code].pressed = 0;
            fprintf(stderr,"[DB] %s flush UP after %ums\n", key_names[code], debounce_ms);
        } else {
            emit_key(fd_out, code, 0, &keys[code].up_time);
            keys[code].pressed = 0;
        }
    }
}

// ---------- SIGTERM ----------
static void handle_sigterm(int signum) {
    (void)signum;
    for(int k=0;k<MAX_KEYCODE;k++) if(keys[k].pressed) emit_key(fd_out,k,0,NULL);
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
        char buf[128]; int r=read(c,buf,sizeof(buf)-1);
        if(r>0) {
            buf[r]=0;
            char *cmd=strtok(buf," \t\n");
            if(!cmd) { close(c); continue; }

            if(strcmp(cmd,"STOP")==0) {
                fprintf(stderr,"STOP received\n");
                if(fd_in>=0) { close(fd_in); fd_in=-1; }
                if(fd_out>=0) { ioctl(fd_out, UI_DEV_DESTROY); close(fd_out); fd_out=-1; }
            } else if(strcmp(cmd,"START")==0) {
                char *dev=strtok(NULL," \t\n");
                char *t = strtok(NULL," \t\n");
                char *m = strtok(NULL," \t\n");
                char *pairs = strtok(NULL," \t\n");
                if(!dev || !t || !m || !pairs) { close(c); continue; }

                debounce_ms=(uint8_t)atoi(t); if(debounce_ms>MAX_DEBOUNCE_MS) debounce_ms=MAX_DEBOUNCE_MS;
                mode = m[0];
                ft_ad_enabled=0; ft_arrows_enabled=0;
                if(strcmp(pairs,"ad")==0) ft_ad_enabled=1;
                else if(strcmp(pairs,"arrows")==0) ft_arrows_enabled=1;
                else if(strcmp(pairs,"both")==0) ft_ad_enabled=ft_arrows_enabled=1;

                fprintf(stderr,"START %s mode=%c debounce=%ums FT ad=%d arrows=%d\n",
                        dev, mode, debounce_ms, ft_ad_enabled, ft_arrows_enabled);

                fd_in = open(dev,O_RDONLY);
                if(fd_in<0) { perror("input open"); close(c); continue; }
                if(ioctl(fd_in,EVIOCGRAB,1)<0) { perror("grab"); close(fd_in); fd_in=-1; close(c); continue; }

                fd_out = setup_uinput();
                if(fd_out<0) { close(fd_in); fd_in=-1; close(c); continue; }
            }
        }
        close(c);
    }
    return NULL;
}

// ---------- Main ----------
int main(void) {
    if(access("/dev/uinput",F_OK)!=0) {
        fprintf(stderr,"You need uinput support for this program to function. What input stack are you on?\n");
        return 2;
    }

    signal(SIGTERM, handle_sigterm);

    pthread_create(&sock_thread,NULL,socket_thread_fn,NULL);

    fprintf(stderr,"Debounced daemon ready, waiting for commands...\n");

    while(running) {
        if(fd_in<0) { usleep(10000); continue; }

        struct input_event ev;
        ssize_t r = read(fd_in,&ev,sizeof(ev));
        if(r!=sizeof(ev)) continue;
        if(ev.type!=EV_KEY || ev.code>=MAX_KEYCODE) continue;

        if(ft_ad_enabled || ft_arrows_enabled) handle_flashtap(ev.code,ev.value);
        process_debounce(fd_in, ev.code, ev.value);
    }

    return 0;
}