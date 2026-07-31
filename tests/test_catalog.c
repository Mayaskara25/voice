/* test_catalog: exercises the models.conf parser -- field splitting, comment
 * and blank-line skipping, malformed-line rejection, and presence detection. */
#include "model_catalog.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    char dir[] = "/tmp/dictation_catalog_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    char conf[256], models[256], present[512];
    snprintf(conf, sizeof(conf), "%s/models.conf", dir);
    snprintf(models, sizeof(models), "%s/models", dir);
    snprintf(present, sizeof(present), "%s/ggml-base.en.bin", models);
    if (mkdir(models, 0755) != 0) { perror("mkdir"); return 1; }

    FILE *f = fopen(conf, "w");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f,
        "# a comment line\n"
        "\n"
        "   # indented comment\n"
        "whisper|base.en|base.en (faster)|ggml-base.en.bin|https://example.invalid/base.en.bin|147951465\n"
        "  whisper | tiny.en | tiny.en # small | ggml-tiny.en.bin | https://example.invalid/t.bin?a=1#f | 0 \n"
        "llama|qwen|Qwen2.5-1.5B|q.gguf|https://example.invalid/q.gguf|1117320736\n"
        "whisper|missing-fields|only three\n"                        /* too few fields */
        "whisper|too|many|fields|https://example.invalid/x|1|extra\n" /* too many fields */
        "bogus|k|Bogus kind|k.bin|https://example.invalid/k.bin|1\n"  /* unknown kind */
        "whisper|nosize|No size|n.bin|https://example.invalid/n.bin|not-a-number\n");
    fclose(f);

    struct model_catalog c;
    CHECK(catalog_load(&c, conf) == 0, "catalog_load returns 0 on a readable file");
    CHECK(c.n == 4, "4 valid rows kept, 3 malformed rows skipped");

    if (c.n < 4) {
        fprintf(stderr, "test_catalog: %d failure(s)\n", failures ? failures : 1);
        return 1;
    }

    CHECK(strcmp(c.e[0].kind, "whisper") == 0, "kind parsed");
    CHECK(strcmp(c.e[0].id, "base.en") == 0, "id parsed");
    CHECK(strcmp(c.e[0].display, "base.en (faster)") == 0, "display parsed");
    CHECK(strcmp(c.e[0].filename, "ggml-base.en.bin") == 0, "filename parsed");
    CHECK(strcmp(c.e[0].url, "https://example.invalid/base.en.bin") == 0, "url parsed");
    CHECK(c.e[0].size == 147951465L, "size_bytes parsed");

    /* Unlike config.c, '#' inside a field is data, not a comment: URLs and
     * display names may legitimately contain one. Surrounding spaces trimmed. */
    CHECK(strcmp(c.e[1].id, "tiny.en") == 0, "surrounding whitespace trimmed from fields");
    CHECK(strcmp(c.e[1].display, "tiny.en # small") == 0, "'#' inside a display name kept");
    CHECK(strcmp(c.e[1].url, "https://example.invalid/t.bin?a=1#f") == 0, "'#' inside a url kept");
    CHECK(c.e[1].size == 0, "size 0 parsed as unknown");

    CHECK(strcmp(c.e[2].kind, "llama") == 0, "llama kind accepted");
    CHECK(c.e[3].size == 0, "unparsable size degrades to 0 rather than dropping the row");

    /* presence: only ggml-base.en.bin exists under <dir>/models */
    FILE *m = fopen(present, "w");
    if (!m) { perror("fopen model"); return 1; }
    fputs("not really a model\n", m);
    fclose(m);

    catalog_refresh_presence(&c, models);
    char want[512];
    snprintf(want, sizeof(want), "%s/ggml-base.en.bin", models);
    CHECK(strcmp(c.e[0].path, want) == 0, "path is <models_dir>/<filename>");
    CHECK(c.e[0].present, "existing file detected as present");
    CHECK(!c.e[1].present, "absent file detected as missing");
    CHECK(!c.e[2].present, "absent gguf detected as missing");

    const struct model_entry *found = catalog_find(&c, "qwen");
    CHECK(found != NULL && strcmp(found->filename, "q.gguf") == 0, "catalog_find by id");
    CHECK(catalog_find(&c, "nope") == NULL, "catalog_find returns NULL for an unknown id");

    catalog_free(&c);
    CHECK(c.n == 0 && c.e == NULL, "catalog_free resets the struct");

    struct model_catalog missing;
    CHECK(catalog_load(&missing, "/nonexistent/models.conf") == -1,
          "catalog_load returns -1 for a missing file");
    catalog_free(&missing);

    /* The shipped catalog must actually parse -- nothing else validates those
     * seed rows. Relative path: make runs the tests from the repo root. */
    struct model_catalog shipped;
    if (catalog_load(&shipped, "configs/models.conf") == 0) {
        CHECK(shipped.n >= 3, "shipped configs/models.conf has its seed rows");
        catalog_free(&shipped);
    } else {
        fprintf(stderr, "FAIL: shipped configs/models.conf did not load"
                        " (run make test from the repo root)\n");
        failures++;
    }

    unlink(present);
    unlink(conf);
    rmdir(models);
    rmdir(dir);

    if (failures) { fprintf(stderr, "test_catalog: %d failure(s)\n", failures); return 1; }
    printf("test_catalog: OK\n");
    return 0;
}
