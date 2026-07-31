/* test_setup: the parts of the setup window that don't need an X display --
 * the log ring's whole-line eviction, the tail view's line alignment, and the
 * selection -> configs/local.conf write. Opens no Display, so `make test` stays
 * headless-safe. */
#include "config.h"
#include "config_write.h"
#include "setup_gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_log_ring(void)
{
    enum { CAP = 16 };
    char buf[CAP + 1] = "";
    size_t len = 0;

    len = setup_log_append(buf, CAP, len, "line1\n", 6);
    CHECK(len == 6 && strcmp(buf, "line1\n") == 0, "first append");

    len = setup_log_append(buf, CAP, len, "line2\n", 6);
    CHECK(len == 12 && strcmp(buf, "line1\nline2\n") == 0, "second append fits");

    /* Overflow: the oldest WHOLE line goes, never half of one -- a partial
     * first line renders as garbage in the pane. */
    len = setup_log_append(buf, CAP, len, "line3\n", 6);
    CHECK(len == 12 && strcmp(buf, "line2\nline3\n") == 0,
          "overflow evicts a whole line, not just the overflowing bytes");
    CHECK(buf[len] == '\0', "the ring stays NUL-terminated for mu_text");

    /* A single write larger than the whole ring keeps its newest tail. */
    len = setup_log_append(buf, CAP, len, "0123456789abcdefghij", 20);
    CHECK(len == CAP, "an oversized append is capped at the ring size");
    CHECK(strcmp(buf, "456789abcdefghij") == 0, "an oversized append keeps its newest bytes");

    /* No newline anywhere: eviction still has to make room. */
    char nb[CAP + 1] = "";
    size_t nl = setup_log_append(nb, CAP, 0, "aaaaaaaaaa", 10);
    nl = setup_log_append(nb, CAP, nl, "bbbbbbbbbb", 10);
    CHECK(nl <= CAP, "a newline-free ring never exceeds its capacity");
    CHECK(memcmp(nb + nl - 4, "bbbb", 4) == 0, "newest bytes survive with no newline to cut at");
}

static void test_log_view(void)
{
    const char *s = "line1\nline2\nline3\n";
    size_t len = strlen(s);

    CHECK(setup_log_view(s, len, 100) == s, "a short log is shown whole");

    const char *v = setup_log_view(s, len, 8);
    CHECK(strcmp(v, "line3\n") == 0,
          "the tail view starts at a line boundary, not mid-line");
}

static void test_write_selection(void)
{
    char dir[] = "/tmp/dictation_setup_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }

    char example[256], local[256];
    snprintf(example, sizeof(example), "%s/example.conf", dir);
    snprintf(local, sizeof(local), "%s/local.conf", dir);

    FILE *f = fopen(example, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs("# a documented template\n"
          "whisper_model_path=./models/placeholder.bin\n"
          "llama_model_path=./models/placeholder.gguf\n"
          "n_threads=8\n", f);
    fclose(f);

    CHECK(setup_write_selection(local, example,
                                "./models/ggml-base.en.bin",
                                "./models/q.gguf") == 0, "selection is written");

    struct app_config cfg;
    CHECK(config_load(local, &cfg) == 0, "local.conf loads");
    CHECK(strcmp(cfg.whisper_model_path, "./models/ggml-base.en.bin") == 0,
          "whisper_model_path written");
    CHECK(strcmp(cfg.llama_model_path, "./models/q.gguf") == 0, "llama_model_path written");
    CHECK(cfg.n_threads == 8, "an unrelated setting survived");

    FILE *r = fopen(local, "r");
    char text[4096] = "";
    size_t n = fread(text, 1, sizeof(text) - 1, r);
    text[n] = '\0';
    fclose(r);
    CHECK(strstr(text, "# a documented template") != NULL,
          "the template's comments survived into local.conf");

    /* "none (raw whisper)" writes an empty path, which disables cleanup. */
    CHECK(setup_write_selection(local, example, "./models/ggml-base.en.bin", "") == 0,
          "the 'none' cleanup choice is written");
    CHECK(config_load(local, &cfg) == 0, "local.conf still loads");
    CHECK(cfg.llama_model_path[0] == '\0', "empty llama_model_path disables cleanup");
    CHECK(strcmp(cfg.whisper_model_path, "./models/ggml-base.en.bin") == 0,
          "the whisper choice is unchanged by a cleanup change");

    unlink(local);
    unlink(example);
    rmdir(dir);
}

int main(void)
{
    test_log_ring();
    test_log_view();
    test_write_selection();

    if (failures) { fprintf(stderr, "test_setup: %d failure(s)\n", failures); return 1; }
    printf("test_setup: OK\n");
    return 0;
}
