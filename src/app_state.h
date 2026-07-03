#ifndef DICTATION_APP_STATE_H
#define DICTATION_APP_STATE_H

enum app_state {
    APP_STATE_IDLE,
    APP_STATE_RECORDING,
    APP_STATE_TRANSCRIBING,
    APP_STATE_CLEANING,
    APP_STATE_INJECTING,
    APP_STATE_ERROR,
};

static inline const char *app_state_name(enum app_state s)
{
    switch (s) {
    case APP_STATE_IDLE:          return "idle";
    case APP_STATE_RECORDING:     return "recording";
    case APP_STATE_TRANSCRIBING:  return "transcribing";
    case APP_STATE_CLEANING:      return "cleaning";
    case APP_STATE_INJECTING:     return "injecting";
    case APP_STATE_ERROR:         return "error";
    }
    return "?";
}

#endif
