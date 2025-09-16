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
    unsigned short bustype;
    unsigned short vendor;
    unsigned short product;
    unsigned short version;
    char serial[256];
} device_info;

static int show_status(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    struct timeval tv = {1, 0};  // 1s timeout
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    // Send STATUS command
    if (write(sock, "STATUS", 6) != 6) {
        perror("write");
        close(sock);
        return -1;
    }
    uint8_t buf[2] = {0};
    ssize_t r = read(sock, buf, 2);
    close(sock);
    if (r != 2) {
        fprintf(stderr, "Failed to read status from daemon\n");
        return -1;
    }
    uint8_t status_byte = buf[0];
    uint8_t timeout = buf[1];
    char running = (status_byte & STATUS_RUNNING) ? 'Y' : 'N';
    printf("Debounce daemon status report\n");
    printf("Running: %c\n", running);
    if (running == 'Y') {
        char mode_str[16] = "";
        char ft_str[16] = "";
        if ((status_byte & STATUS_DEBOUNCE) && (status_byte & STATUS_FLASHTAP))
            strcpy(mode_str, "DB+FT");
        else if (status_byte & STATUS_DEBOUNCE)
            strcpy(mode_str, "DB");
        else if (status_byte & STATUS_FLASHTAP)
            strcpy(mode_str, "FT");
        if (status_byte & STATUS_FLASHTAP) {
            if ((status_byte & STATUS_PAIR_AD) && (status_byte & STATUS_PAIR_ARROWS))
                strcpy(ft_str, "Both");
            else if (status_byte & STATUS_PAIR_AD)
                strcpy(ft_str, "AD");
            else if (status_byte & STATUS_PAIR_ARROWS)
                strcpy(ft_str, "Arrows");
        }
        printf("Mode: %s\n", mode_str);
        if (status_byte & STATUS_FLASHTAP) printf("FT Pairs: %s\n", ft_str);
        if (status_byte & STATUS_DEBOUNCE) printf("Timeout: %dms\n", timeout);
    }
    return 0;
}

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

// Check if symlink points to a path containing "mouse" or "wmi-event"
static int symlink_is_excluded(const char *symlink_path) {
    char name[PATH_MAX];
    strncpy(name, symlink_path, PATH_MAX - 1);
    char *base = basename(name);
    if (strcasestr(base, "mouse") || strcasestr(base, "wmi-event")) return 1;
    return 0;
}

// Build a list of event* nodes to exclude by scanning by-id and by-path
static void build_exclusion_list(char excluded[][PATH_MAX], int *exclude_count) {
    *exclude_count = 0;
    const char *dirs[] = {"/dev/input/by-id", "/dev/input/by-path"};
    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dirs[d], entry->d_name);
            char target[PATH_MAX];
            ssize_t len = readlink(full, target, sizeof(target) - 1);
            if (len < 0) continue;
            target[len] = 0;
            if (symlink_is_excluded(entry->d_name)) {
                char devnode[PATH_MAX];
                snprintf(devnode, sizeof(devnode), "/dev/input/%s", basename(target));
                snprintf(excluded[(*exclude_count)++], PATH_MAX, "%s", devnode);
            }
        }
        closedir(dir);
    }
}

// Check if a device is in the exclusion list
static int is_excluded(const char *path, char excluded[][PATH_MAX], int exclude_count) {
    for (int i = 0; i < exclude_count; i++) {
        if (strcmp(path, excluded[i]) == 0) return 1;
    }
    return 0;
}

// Simple keyboard filter: ensure EV_KEY present
static int is_keyboard(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long evbit[(EV_MAX + 7) / 8] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
    close(fd);
    return evbit[EV_KEY / 8] & (1 << (EV_KEY % 8));
}

// Enumerate usable devices
static int enumerate_devices(device_info *devs, int max) {
    DIR *dir = opendir("/dev/input/");
    if (!dir) return 0;
    struct dirent *entry;
    int count = 0;
    char excluded[64][PATH_MAX];
    int exclude_count;
    build_exclusion_list(excluded, &exclude_count);
    while ((entry = readdir(dir)) && count < max) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (is_excluded(path, excluded, exclude_count)) continue;
        if (!is_keyboard(path)) continue;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct input_id id;
        char name[256] = "UNKNOWN";
        char serial[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        ioctl(fd, EVIOCGID, &id);
        ioctl(fd, EVIOCGUNIQ(sizeof(serial)), serial);
        close(fd);
        snprintf(devs[count].path, PATH_MAX, "%s", path);
        snprintf(devs[count].name, sizeof(devs[count].name), "%s", name);
        devs[count].bustype = id.bustype;
        devs[count].vendor = id.vendor;
        devs[count].product = id.product;
        devs[count].version = id.version;
        snprintf(devs[count].serial, sizeof(devs[count].serial), "%s", serial);
        count++;
    }
    closedir(dir);
    return count;
}

// Show available devices
static int show_devices(void) {
    device_info devs[MAX_DEVICES];
    int count = enumerate_devices(devs, MAX_DEVICES);
    if (count == 0) {
        printf("No keyboards found.\n");
        return 0;
    }
    printf("Available keyboards:\n");
    for (int i = 0; i < count; i++) {
        printf("%d: %-30s %-20s [Bus:%04x Vendor:%04x Product:%04x Version:%04x] %s\n", i, devs[i].path, devs[i].name,
               devs[i].bustype, devs[i].vendor, devs[i].product, devs[i].version, devs[i].serial);
    }
    return 0;
}

// Send IPC command
static int send_cmd(const char *cmd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    struct timeval tv = {1, 0};  // 1s timeout
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    if (write(sock, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
        perror("write");
        close(sock);
        return 1;
    }
    uint8_t status_code = 1;
    ssize_t ret = read(sock, &status_code, 1);
    (void)ret;
    close(sock);
    if (status_code == 1) {
        if (strncasecmp(cmd, "STOP", 4) == 0) {
            fprintf(stderr, "Daemon is already idle; stop command was ignored.\n");
        }
        if (strncasecmp(cmd, "START", 5) == 0) {
            fprintf(stderr, "Daemon is already running; start command was ignored.\n");
        }
    }
    return status_code;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }
    int dev_idx = 1;  // default device arg position
    int timeout = 50;
    char mode = 'd';
    char ftpair[16] = "none";
    char device[PATH_MAX] = {0};
    if (strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "show") == 0 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "--help") == 0) {
        if (argc != 2) {
            fprintf(stderr, "[%s] does not take extra arguments\n", argv[1]);
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[1], "stop") == 0) return send_cmd("STOP");
        if (strcmp(argv[1], "show") == 0) return show_devices();
        if (strcmp(argv[1], "status") == 0) return show_status();
        if (strcmp(argv[1], "--help") == 0) { print_usage(argv[0]); return 0; }
    } else if (strcmp(argv[1], "start") == 0) {
        if (argc < 3 || argc > 6) {
            fprintf(stderr, "'start' requires 1-4 additional args\n");
            print_usage(argv[0]);
            return 1;
        }
        dev_idx = 2;
    } else if (strchr(argv[1], '/')) {
        // device path given directly
        if (argc > 5) {
            fprintf(stderr, "Too many arguments with device path\n");
            print_usage(argv[0]);
            return 1;
        }
        dev_idx = 1;
    } else {
        fprintf(stderr, "First argument must be 'stop', 'show', 'start', or a device path\n");
        print_usage(argv[0]);
        return 1;
    }
    strncpy(device, argv[dev_idx], PATH_MAX - 1);
    if (argc >= dev_idx + 2) timeout = atoi(argv[dev_idx + 1]);
    if (argc >= dev_idx + 3) mode = argv[dev_idx + 2][0];
    if (argc >= dev_idx + 4) strncpy(ftpair, argv[dev_idx + 3], sizeof(ftpair) - 1);
    // If FT pair not supplied, set defaults depending on mode
    if (mode == 'd')
        strncpy(ftpair, "none", sizeof(ftpair) - 1);
    else if (ftpair[0] == 0)
        strncpy(ftpair, "ad", sizeof(ftpair) - 1);
    // Compose START command
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "START %s %d %c %s", device, timeout, mode, ftpair);
    return send_cmd(cmd);
}