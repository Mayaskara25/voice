/* test_config: exercises the INI parser -- key parsing, defaults, inline
 * comments, unknown-key tolerance, and the missing-file error path. */
#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    char path[] = "/tmp/dictation_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    FILE *f = fdopen(fd, "w");
    fprintf(f,
        "# a comment line\n"
        "\n"
        "whisper_model_path=/tmp/model.bin\n"
        "ptt_keycode=42\n"
        "n_threads=3\n"
        "test_mode=true\n"
        "gui_enabled=true\n"
        "gui_font=9x15\n"
        "audio_device=plughw:2,0   # inline comment after value\n"
        "bogus_key=whatever\n");   /* unknown key must be tolerated */
    fclose(f);

    struct app_config cfg;
    int rc = config_load(path, &cfg);
    unlink(path);

    CHECK(rc == 0, "config_load returns 0 on a readable file");
    CHECK(strcmp(cfg.whisper_model_path, "/tmp/model.bin") == 0, "whisper_model_path parsed");
    CHECK(cfg.ptt_keycode == 42, "ptt_keycode parsed");
    CHECK(cfg.n_threads == 3, "n_threads parsed");
    CHECK(cfg.test_mode == true, "test_mode=true parsed");
    CHECK(cfg.gui_enabled == true, "gui_enabled=true parsed");
    CHECK(strcmp(cfg.gui_font, "9x15") == 0, "gui_font parsed");
    CHECK(strcmp(cfg.audio_device, "plughw:2,0") == 0, "inline comment stripped from value");
    CHECK(strcmp(cfg.language, "en") == 0, "language default retained when unset");
    CHECK(cfg.llama_model_path[0] == '\0', "llama_model_path default is empty");

    struct app_config cfg2;
    CHECK(config_load("/nonexistent/does-not-exist.conf", &cfg2) == -1,
          "config_load returns -1 for a missing file");

    if (failures) { fprintf(stderr, "test_config: %d failure(s)\n", failures); return 1; }
    printf("test_config: OK\n");
    return 0;
}
