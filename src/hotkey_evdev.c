#include "hotkey_evdev.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EVDEV_DIR "/dev/input"
#define MAX_PROBE_DEVICES 64

static volatile sig_atomic_t g_stop = 0;

void hotkey_request_stop(void)
{
    g_stop = 1;
}

/* --- small code -> human name table, covers common PTT candidates --- */
struct code_name { int code; const char *name; };

static const struct code_name KEY_NAMES[] = {
    { 1,   "ESC" },           { 15,  "TAB" },
    { 28,  "ENTER" },         { 29,  "LEFTCTRL" },
    { 42,  "LEFTSHIFT" },     { 54,  "RIGHTSHIFT" },
    { 56,  "LEFTALT" },       { 57,  "SPACE" },
    { 58,  "CAPSLOCK" },      { 70,  "SCROLLLOCK" },
    { 97,  "RIGHTCTRL" },     { 100, "RIGHTALT" },
    { 110, "INSERT" },        { 111, "DELETE" },
    { 119, "PAUSE" },         { 125, "LEFTMETA" },
    { 126, "RIGHTMETA" },
    { 59, "F1" }, { 60, "F2" }, { 61, "F3" }, { 62, "F4" },
    { 63, "F5" }, { 64, "F6" }, { 65, "F7" }, { 66, "F8" },
    { 67, "F9" }, { 68, "F10" }, { 87, "F11" }, { 88, "F12" },
    { 2, "1" }, { 3, "2" }, { 4, "3" }, { 5, "4" }, { 6, "5" },
    { 7, "6" }, { 8, "7" }, { 9, "8" }, { 10, "9" }, { 11, "0" },
    { 30, "A" }, { 48, "B" }, { 46, "C" }, { 32, "D" }, { 18, "E" },
    { 33, "F" }, { 34, "G" }, { 35, "H" }, { 23, "I" }, { 36, "J" },
    { 37, "K" }, { 38, "L" }, { 50, "M" }, { 49, "N" }, { 24, "O" },
    { 25, "P" }, { 16, "Q" }, { 19, "R" }, { 31, "S" }, { 20, "T" },
    { 22, "U" }, { 47, "V" }, { 17, "W" }, { 45, "X" }, { 21, "Y" },
    { 44, "Z" },
};

static const char *key_name(int code)
{
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++)
        if (KEY_NAMES[i].code == code)
            return KEY_NAMES[i].name;
    return NULL;
}

#define NBITS(x) ((((x) - 1) / (sizeof(long) * 8)) + 1)

static bool test_bit(int bit, const unsigned long *array)
{
    return (array[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

/* Representative alpha/space/enter codes; a real keyboard supports all of these,
 * decoys (jack-sense nodes, lid switches, vendor hotkey devices) typically don't. */
static const int PROBE_CODES[] = { 30 /*A*/, 48 /*B*/, 57 /*SPACE*/, 28 /*ENTER*/, 16 /*Q*/ };

static bool looks_like_keyboard(int fd)
{
    unsigned long evbits[NBITS(EV_MAX)];
    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        return false;
    if (!test_bit(EV_KEY, evbits))
        return false;

    unsigned long keybits[NBITS(KEY_MAX)];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return false;

    for (size_t i = 0; i < sizeof(PROBE_CODES) / sizeof(PROBE_CODES[0]); i++)
        if (!test_bit(PROBE_CODES[i], keybits))
            return false;

    return true;
}

static int list_event_devices(char paths[][300], int max)
{
    DIR *d = opendir(EVDEV_DIR);
    if (!d) {
        log_error("hotkey: cannot open %s: %s", EVDEV_DIR, strerror(errno));
        return -1;
    }

    int n = 0;
    struct dirent *ent;
    while (n < max && (ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        snprintf(paths[n], sizeof(paths[0]), "%s/%s", EVDEV_DIR, ent->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int hotkey_find_keyboard_device(char *out_path, size_t out_size)
{
    char paths[MAX_PROBE_DEVICES][300];
    int n = list_event_devices(paths, MAX_PROBE_DEVICES);
    if (n < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES)
                log_warn("hotkey: no permission to read %s (add yourself to the "
                         "'input' group and re-login, see PLAN.md)", paths[i]);
            continue;
        }

        bool ok = looks_like_keyboard(fd);
        char name[256] = "?";
        if (ok)
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        close(fd);

        if (ok) {
            snprintf(out_path, out_size, "%s", paths[i]);
            log_info("hotkey: auto-detected keyboard device %s (\"%s\")", paths[i], name);
            return 0;
        }
    }

    log_error("hotkey: no keyboard-like device found under %s -- set ptt_device explicitly", EVDEV_DIR);
    return -1;
}

int hotkey_list_keys(void)
{
    char paths[MAX_PROBE_DEVICES][300];
    int n = list_event_devices(paths, MAX_PROBE_DEVICES);
    if (n < 0)
        return -1;

    struct pollfd pfds[MAX_PROBE_DEVICES];
    /* Index into paths[] for each pollfd slot, rather than a second copy of the
     * strings: paths[] is still in scope where these are printed below. The
     * copy it replaces was 19 KB of stack, and gcc could not prove the source
     * was NUL-terminated within its own row of the 2D array, so it warned about
     * a truncation that cannot happen. */
    int path_of[MAX_PROBE_DEVICES];
    int nfds = 0;

    for (int i = 0; i < n; i++) {
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES)
                log_warn("hotkey: no permission to read %s", paths[i]);
            continue;
        }
        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLIN;
        path_of[nfds] = i;
        nfds++;
    }

    if (nfds == 0) {
        log_error("hotkey: no readable /dev/input/event* devices -- check 'input' group membership");
        return -1;
    }

    printf("Listening for key presses on %d device(s). Press your desired PTT key "
           "(Ctrl-C to stop)...\n", nfds);
    fflush(stdout);

    struct input_event ev;
    while (!g_stop) {
        int r = poll(pfds, nfds, 500);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_error("hotkey: poll failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nfds && r > 0; i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;
            r--;
            ssize_t nread = read(pfds[i].fd, &ev, sizeof(ev));
            if (nread != (ssize_t)sizeof(ev))
                continue;
            if (ev.type != EV_KEY || ev.value != 1) /* only report the initial press */
                continue;
            const char *name = key_name(ev.code);
            if (name)
                printf("%-24s code=%-4d name=%s\n", paths[path_of[i]], ev.code, name);
            else
                printf("%-24s code=%-4d name=(unlisted, see linux/input-event-codes.h)\n",
                       paths[path_of[i]], ev.code);
            fflush(stdout);
        }
    }

    for (int i = 0; i < nfds; i++)
        close(pfds[i].fd);
    return 0;
}

int hotkey_run(const char *device, int keycode,
               hotkey_callback on_down, hotkey_callback on_up, void *user_data)
{
    char resolved[300];
    const char *use_device = device;

    if (!device || device[0] == '\0') {
        if (hotkey_find_keyboard_device(resolved, sizeof(resolved)) != 0)
            return -1;
        use_device = resolved;
    }

    int fd = open(use_device, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES)
            log_error("hotkey: permission denied opening %s -- add yourself to the "
                      "'input' group (sudo usermod -aG input $USER) and re-login", use_device);
        else
            log_error("hotkey: cannot open %s: %s", use_device, strerror(errno));
        return -1;
    }

    log_info("hotkey: listening on %s for evdev code %d", use_device, keycode);

    struct input_event ev;
    bool held = false;
    while (!g_stop) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, 500);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_error("hotkey: poll failed: %s", strerror(errno));
            break;
        }
        if (r == 0)
            continue;

        ssize_t nread = read(fd, &ev, sizeof(ev));
        if (nread != (ssize_t)sizeof(ev))
            continue;
        if (ev.type != EV_KEY || ev.code != keycode)
            continue;

        if (ev.value == 2) {
            /* auto-repeat: key is still held, not a new press */
            continue;
        } else if (ev.value == 1 && !held) {
            held = true;
            if (on_down)
                on_down(user_data);
        } else if (ev.value == 0 && held) {
            held = false;
            if (on_up)
                on_up(user_data);
        }
    }

    close(fd);
    return 0;
}
