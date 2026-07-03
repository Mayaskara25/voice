#include "config.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void config_set_defaults(struct app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->whisper_model_path, sizeof(cfg->whisper_model_path), "./models/ggml-base.en.bin");
    cfg->llama_model_path[0] = '\0';
    cfg->ptt_device[0] = '\0';
    cfg->ptt_keycode = 97; /* KEY_RIGHTCTRL */
    cfg->audio_device[0] = '\0';
    cfg->n_threads = 8;
    snprintf(cfg->language, sizeof(cfg->language), "en");
    cfg->test_mode = false;
    cfg->gui_enabled = false;
    cfg->gui_font[0] = '\0';
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static bool parse_bool(const char *v, bool fallback)
{
    if (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0)
        return true;
    if (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0)
        return false;
    return fallback;
}

static void apply_kv(struct app_config *cfg, const char *key, const char *value)
{
    if (strcmp(key, "whisper_model_path") == 0) {
        snprintf(cfg->whisper_model_path, sizeof(cfg->whisper_model_path), "%s", value);
    } else if (strcmp(key, "llama_model_path") == 0) {
        snprintf(cfg->llama_model_path, sizeof(cfg->llama_model_path), "%s", value);
    } else if (strcmp(key, "ptt_device") == 0) {
        snprintf(cfg->ptt_device, sizeof(cfg->ptt_device), "%s", value);
    } else if (strcmp(key, "audio_device") == 0) {
        snprintf(cfg->audio_device, sizeof(cfg->audio_device), "%s", value);
    } else if (strcmp(key, "ptt_keycode") == 0) {
        cfg->ptt_keycode = atoi(value);
    } else if (strcmp(key, "n_threads") == 0) {
        cfg->n_threads = atoi(value);
    } else if (strcmp(key, "language") == 0) {
        snprintf(cfg->language, sizeof(cfg->language), "%s", value);
    } else if (strcmp(key, "test_mode") == 0) {
        cfg->test_mode = parse_bool(value, cfg->test_mode);
    } else if (strcmp(key, "gui_enabled") == 0) {
        cfg->gui_enabled = parse_bool(value, cfg->gui_enabled);
    } else if (strcmp(key, "gui_font") == 0) {
        snprintf(cfg->gui_font, sizeof(cfg->gui_font), "%s", value);
    } else {
        log_warn("config: unknown key '%s' ignored", key);
    }
}

int config_load(const char *path, struct app_config *cfg)
{
    config_set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("config: could not open '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            log_warn("config: %s:%d: missing '=', skipping line", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);

        /* allow trailing inline comments after a value, e.g. "foo=bar   # comment" */
        char *hash = strchr(value, '#');
        if (hash) {
            *hash = '\0';
            value = trim(value);
        }

        apply_kv(cfg, key, value);
    }

    fclose(f);
    return 0;
}

int config_validate(const struct app_config *cfg)
{
    struct stat st;

    if (cfg->whisper_model_path[0] == '\0') {
        log_error("config: whisper_model_path must be set");
        return -1;
    }
    if (stat(cfg->whisper_model_path, &st) != 0) {
        log_error("config: whisper_model_path '%s' does not exist", cfg->whisper_model_path);
        return -1;
    }

    if (cfg->llama_model_path[0] != '\0' && stat(cfg->llama_model_path, &st) != 0) {
        log_error("config: llama_model_path '%s' does not exist", cfg->llama_model_path);
        return -1;
    }

    if (cfg->ptt_device[0] != '\0' && stat(cfg->ptt_device, &st) != 0) {
        log_error("config: ptt_device '%s' does not exist", cfg->ptt_device);
        return -1;
    }

    if (cfg->n_threads <= 0) {
        log_error("config: n_threads must be positive");
        return -1;
    }

    return 0;
}
