#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define STATUS_RUNNING 0x80
#define STATUS_DEBOUNCE 0x08
#define STATUS_FLASHTAP 0x04
#define STATUS_PAIR_AD 0x02
#define STATUS_PAIR_ARROWS 0x01

#define SOCKET_PATH "/run/debounced.sock"
#define MAX_DEVICES 64

typedef struct {
    char path[PATH_MAX];
    char name[256];
    int event_num;
} device_info;

// ---------- Show status ----------
static int show_status(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("connect"); close(sock); return -1; }
    if (write(sock, "STATUS", 6) != 6) { perror("write"); close(sock); return -1; }
    uint8_t buf[2] = {0};
    ssize_t r = read(sock, buf, 2);
    close(sock);
    if (r != 2) { fprintf(stderr, "Failed to read status from daemon\n"); return -1; }
    uint8_t status_byte = buf[0];
    uint8_t timeout = buf[1];
    char running = (status_byte & STATUS_RUNNING) ? 'Y' : 'N';
    printf("Debounce daemon status\n================================\nRunning: %c\n", running);
    if (running == 'Y') {
        char mode_str[32] = "", ft_str[16] = "";
        if ((status_byte & STATUS_DEBOUNCE) && (status_byte & STATUS_FLASHTAP)) strcpy(mode_str, "Debounce & FlashTap");
        else if (status_byte & STATUS_DEBOUNCE) strcpy(mode_str, "Debounce");
        else if (status_byte & STATUS_FLASHTAP) strcpy(mode_str, "FlashTap");
        if (status_byte & STATUS_FLASHTAP) {
            if ((status_byte & STATUS_PAIR_AD) && (status_byte & STATUS_PAIR_ARROWS)) strcpy(ft_str, "A/D & Arrows");
            else if (status_byte & STATUS_PAIR_AD) strcpy(ft_str, "A/D");
            else if (status_byte & STATUS_PAIR_ARROWS) strcpy(ft_str, "Arrows");
        }
        printf("Mode: %s\n", mode_str);
        if (status_byte & STATUS_FLASHTAP) {
            const char *plural = ((status_byte & STATUS_PAIR_AD) && (status_byte & STATUS_PAIR_ARROWS)) ? "s" : "";
            printf("FlashTap Pair%s: %s\n", plural, ft_str);
        }
        if (status_byte & STATUS_DEBOUNCE) printf("Timeout: %dms\n", timeout);
    }
    return 0;
}

// ---------- Print usage ----------
static void print_usage(const char *prog) {
    printf("Usage: %s stop|show|status\n", prog);
    printf("       %s start <device> [timeout] [mode] [pair]\n\n", prog);
    printf("Commands:\n");
    printf("    stop: stops all debounce and FlashTap activity until next start\n");
    printf("    show: lists potential keyboard device nodes in a human-readable format\n");
    printf("    status: shows current status of the daemon process\n");
    printf("    start: starts the daemon with the arguments provided (list below)\n\n");
    printf("Arguments (only 'start' command uses arguments):\n");
    printf("    device: path to keyboard event node to start with [REQUIRED, NO DEFAULT]\n");
    printf("    timeout: length of time in ms to debounce each input for [default: 50]\n");
    printf("    mode: b (both), d (debounce only), f (FlashTap only) [default: d]\n");
    printf("    pair: ad, arrows, both, none [default: none for d, ad for f/b]\n");
}

// ---------- Comparison for numeric sort ----------
static int cmp_event(const void *a, const void *b) {
    return ((device_info*)a)->event_num - ((device_info*)b)->event_num;
}

// ---------- Show keyboards ----------
static int show_devices(void) {
    DIR *dir = opendir("/dev/input/");
    if (!dir) return 0;
    struct dirent *entry;
    device_info devs[MAX_DEVICES];
    int n_events = 0;
    while ((entry = readdir(dir)) && n_events < MAX_DEVICES) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        snprintf(devs[n_events].path, PATH_MAX, "/dev/input/%s", entry->d_name);
        devs[n_events].event_num = atoi(entry->d_name + 5);
        n_events++;
    }
    closedir(dir);
    const char *symlink_dirs[] = {"/dev/input/by-id", "/dev/input/by-path"};
    int valid[MAX_DEVICES] = {0};
    for (int i = 0; i < n_events; i++) {
        int count = 0;
        int exclude_node = 0;
        for (int d = 0; d < 2; d++) {
            DIR *sdir = opendir(symlink_dirs[d]);
            if (!sdir) continue;
            struct dirent *sentry;
            while ((sentry = readdir(sdir))) {
                if (sentry->d_name[0] == '.') continue;
                char full_symlink[PATH_MAX];
                snprintf(full_symlink, PATH_MAX, "%s/%s", symlink_dirs[d], sentry->d_name);
                char target[PATH_MAX];
                ssize_t len = readlink(full_symlink, target, sizeof(target) - 1);
                if (len < 0) continue;
                target[len] = '\0';
                if (strcmp(basename(target), basename(devs[i].path)) != 0)
                    continue;
                if (strcasestr(sentry->d_name, "mouse") || strcasestr(sentry->d_name, "wmi-event")) {
                    exclude_node = 1;
                    break;
                }
                count++;
            }
            closedir(sdir);
            if (exclude_node) break;
        }
        valid[i] = (!exclude_node && count > 0);
    }
    device_info filtered[MAX_DEVICES];
    int fcount = 0;
    for (int i = 0; i < n_events; i++) {
        if (!valid[i]) continue;
        int fd = open(devs[i].path, O_RDONLY);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof(devs[i].name)), devs[i].name);
        close(fd);
        filtered[fcount++] = devs[i];
    }
    qsort(filtered, fcount, sizeof(device_info), cmp_event);
    if (fcount == 0) { printf("No keyboards found.\n"); return 0; }
    for (int i = 0; i < fcount; i++)
        printf("%d: %-20s %s\n", i + 1, filtered[i].path, filtered[i].name);
    return 0;
}

// ---------- Send command ----------
static int send_cmd(const char *cmd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("connect"); close(sock); return -1; }
    if (write(sock, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) { perror("write"); close(sock); return 1; }
    uint8_t status_code = 1;
    ssize_t ret = read(sock, &status_code, 1);
    (void)ret;
    close(sock);
    if (status_code == 1) {
        if (strncasecmp(cmd, "STOP", 4) == 0)
            fprintf(stderr, "Daemon is already idle; stop command was ignored.\n");
        if (strncasecmp(cmd, "START", 5) == 0)
            fprintf(stderr, "Daemon is already running; start command was ignored.\n");
    }
    return status_code;
}

// ---------- Main ----------
int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 0; }
    int timeout = 50;
    char mode = 'd';
    char ftpair[16] = "none";
    char device[PATH_MAX] = {0};
    if (strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "show") == 0 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "--help") == 0) {
        if (argc != 2) { fprintf(stderr, "%s does not take extra arguments\n", argv[1]); print_usage(argv[0]); return 1; }
        if (strcmp(argv[1], "stop") == 0) return send_cmd("STOP");
        if (strcmp(argv[1], "show") == 0) return show_devices();
        if (strcmp(argv[1], "status") == 0) return show_status();
        if (strcmp(argv[1], "--help") == 0) { print_usage(argv[0]); return 0; }
    } else if (strcmp(argv[1], "start") == 0) {
        if (argc < 3 || argc > 6) { fprintf(stderr, "Too many arguments; maximum 4 for start command.\n"); print_usage(argv[0]); return 1; }
    } else { fprintf(stderr, "Command must be one of: stop show status start\n"); print_usage(argv[0]); return 1; }
    strncpy(device, argv[2], PATH_MAX - 1);
    if (argc >= 4) timeout = atoi(argv[3]);
    if (argc >= 5) mode = argv[4][0];
    if (argc == 6) strncpy(ftpair, argv[5], sizeof(ftpair) - 1);
    if (mode == 'd') strncpy(ftpair, "none", sizeof(ftpair) - 1);
    else if (ftpair[0] == 0) strncpy(ftpair, "ad", sizeof(ftpair) - 1);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "START %s %d %c %s", device, timeout, mode, ftpair);
    return send_cmd(cmd);
}