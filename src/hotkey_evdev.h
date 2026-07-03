#ifndef DICTATION_HOTKEY_EVDEV_H
#define DICTATION_HOTKEY_EVDEV_H

#include <stddef.h>

typedef void (*hotkey_callback)(void *user_data);

/* Scans /dev/input/event* for a device that looks like a real keyboard
 * (supports EV_KEY with a broad alphabetic key range), skipping decoys
 * such as HDMI audio jack-sense nodes or lid switches that also expose
 * a handful of EV_KEY codes. Writes the device path into out_path.
 * Returns 0 on success, -1 if no suitable device was found. */
int hotkey_find_keyboard_device(char *out_path, size_t out_size);

/* Prints "<device> <evdev code> <key name>" to stdout for every keypress
 * observed across all readable /dev/input/event* devices, until interrupted
 * (Ctrl-C). Used to discover the ptt_keycode value for the config file --
 * this is an evdev code (linux/input-event-codes.h), NOT an X keysym/keycode. */
int hotkey_list_keys(void);

/* Opens `device` (or auto-detects one if device is empty) and blocks,
 * calling on_down/on_up (with user_data) whenever the given evdev keycode
 * transitions to pressed (value==1) or released (value==0). Auto-repeat
 * events (value==2) are discarded. Returns -1 on error (e.g. permission
 * denied -- the caller should already have set up `input` group access),
 * otherwise does not return until the process is signaled to stop. */
int hotkey_run(const char *device, int keycode,
               hotkey_callback on_down, hotkey_callback on_up, void *user_data);

/* Requests hotkey_run() to stop at the next event-loop iteration. Safe to
 * call from a signal handler. */
void hotkey_request_stop(void);

#endif
