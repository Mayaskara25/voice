/* test_config_write: exercises the comment-preserving rewriter -- verbatim copy
 * of everything unmatched, in-place replacement of matched keys, appending of
 * absent keys, idempotency across repeated calls, the seed-once bootstrap, and
 * the local.conf-preferring default path. */
#include "config.h"
#include "config_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void spit(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/* Returns a pointer into one shared static buffer, so only one result is valid
 * at a time -- never compare slurp(a) against slurp(b) in a single expression. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Counts non-overlapping occurrences of `needle` in `hay`. */
static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = strstr(hay, needle); p; p = strstr(p + len, needle))
        n++;
    return n;
}

static const char SEED[] =
    "# Push-to-talk dictation daemon config\n"
    "# key=value, '#' starts a comment.\n"
    "\n"
    "whisper_model_path=./models/old.bin\n"
    "\n"
    "# hand-edit whisper_model_path to point at what you downloaded\n"
    "#whisper_model_path=./models/commented-out.bin\n"
    "   n_threads = 8\n"
    "cleanup_style=dictation\n"
    "whisper_model_path=./models/duplicate-later.bin\n";

/* Every line above is reproduced byte-for-byte except the two real
 * whisper_model_path assignments; llama_model_path was absent so it lands in an
 * appended block. */
static const char EXPECTED[] =
    "# Push-to-talk dictation daemon config\n"
    "# key=value, '#' starts a comment.\n"
    "\n"
    "whisper_model_path=./models/new.bin\n"
    "\n"
    "# hand-edit whisper_model_path to point at what you downloaded\n"
    "#whisper_model_path=./models/commented-out.bin\n"
    "   n_threads = 8\n"
    "cleanup_style=dictation\n"
    "whisper_model_path=./models/new.bin\n"
    "\n"
    "# --- written by dictation-setup ---\n"
    "llama_model_path=./models/q.gguf\n";

int main(void)
{
    char dir[] = "/tmp/dictation_cfgwrite_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    char conf[256], local[256], example[256], cfgdir[256];
    snprintf(conf, sizeof(conf), "%s/test.conf", dir);
    snprintf(cfgdir, sizeof(cfgdir), "%s/configs", dir);
    snprintf(local, sizeof(local), "%s/local.conf", dir);
    snprintf(example, sizeof(example), "%s/example.conf", dir);

    const char *keys[] = { "whisper_model_path", "llama_model_path" };
    const char *vals[] = { "./models/new.bin", "./models/q.gguf" };

    /* --- rewrite: replace present keys, append absent ones, keep the rest --- */
    spit(conf, SEED);
    CHECK(config_write_keys(conf, keys, vals, 2) == 0, "config_write_keys returns 0");
    char *got = slurp(conf);
    CHECK(strcmp(got, EXPECTED) == 0, "comments and unmatched lines survive byte-for-byte");
    CHECK(strstr(got, "#whisper_model_path=./models/commented-out.bin") != NULL,
          "a commented-out assignment is not rewritten");
    CHECK(strstr(got, "   n_threads = 8\n") != NULL,
          "an unrelated key keeps its original spacing verbatim");
    CHECK(strstr(got, "./models/duplicate-later.bin") == NULL,
          "a later duplicate of the key is rewritten too (config_load is last-wins)");

    /* --- idempotency: a second identical call must not duplicate anything --- */
    CHECK(config_write_keys(conf, keys, vals, 2) == 0, "second config_write_keys returns 0");
    got = slurp(conf);
    CHECK(strcmp(got, EXPECTED) == 0, "rewriting twice is a no-op");
    CHECK(count_of(got, "# --- written by dictation-setup ---") == 1,
          "exactly one written-by header after two calls");
    CHECK(count_of(got, "llama_model_path=") == 1,
          "the appended key is rewritten in place, not appended again");

    /* --- the result round-trips through the parser --- */
    struct app_config cfg;
    CHECK(config_load(conf, &cfg) == 0, "config_load reads the rewritten file");
    CHECK(strcmp(cfg.whisper_model_path, "./models/new.bin") == 0, "whisper_model_path written");
    CHECK(strcmp(cfg.llama_model_path, "./models/q.gguf") == 0, "llama_model_path written");
    CHECK(cfg.n_threads == 8, "an untouched setting still parses");
    CHECK(strcmp(cfg.cleanup_style, "dictation") == 0, "another untouched setting still parses");

    /* --- empty value: the setup window's "none (raw whisper)" cleanup option --- */
    const char *nokeys[] = { "llama_model_path" };
    const char *novals[] = { "" };
    CHECK(config_write_keys(conf, nokeys, novals, 1) == 0, "writing an empty value succeeds");
    CHECK(config_load(conf, &cfg) == 0, "config_load reads it back");
    CHECK(cfg.llama_model_path[0] == '\0', "empty llama_model_path round-trips (cleanup disabled)");

    /* --- a missing file is created rather than failing --- */
    char fresh[256];
    snprintf(fresh, sizeof(fresh), "%s/fresh.conf", dir);
    CHECK(config_write_keys(fresh, keys, vals, 2) == 0, "writing a nonexistent file creates it");
    CHECK(config_load(fresh, &cfg) == 0 &&
          strcmp(cfg.whisper_model_path, "./models/new.bin") == 0,
          "the created file parses");
    unlink(fresh);

    /* --- bootstrap: seeds once, then never clobbers --- */
    spit(example, SEED);
    CHECK(config_bootstrap_local(local, example) == 1, "bootstrap creates local.conf");
    CHECK(strcmp(slurp(local), SEED) == 0, "local.conf is a byte-for-byte copy of the template");

    spit(local, "whisper_model_path=./models/hand-edited.bin\n");
    CHECK(config_bootstrap_local(local, example) == 0, "bootstrap reports an existing file");
    CHECK(strcmp(slurp(local), "whisper_model_path=./models/hand-edited.bin\n") == 0,
          "an existing local.conf is never clobbered");

    CHECK(config_bootstrap_local(local, "/nonexistent/example.conf") == 0,
          "an existing local.conf short-circuits before the template is read");

    char nodir[256];
    snprintf(nodir, sizeof(nodir), "%s/nope/local.conf", dir);
    CHECK(config_bootstrap_local(nodir, example) == -1, "bootstrap fails on an unwritable path");

    /* --- default path prefers local.conf, and only when it exists --- */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }
    if (mkdir(cfgdir, 0755) != 0) { perror("mkdir"); return 1; }
    if (chdir(dir) != 0) { perror("chdir"); return 1; }

    CHECK(strcmp(config_default_path(), CONFIG_EXAMPLE_PATH) == 0,
          "default path is example.conf when local.conf is absent");
    spit(CONFIG_LOCAL_PATH, "n_threads=2\n");
    CHECK(strcmp(config_default_path(), CONFIG_LOCAL_PATH) == 0,
          "default path prefers local.conf when it exists");
    unlink(CONFIG_LOCAL_PATH);

    if (chdir(cwd) != 0) { perror("chdir back"); return 1; }

    unlink(conf);
    unlink(local);
    unlink(example);
    rmdir(cfgdir);
    rmdir(dir);

    if (failures) { fprintf(stderr, "test_config_write: %d failure(s)\n", failures); return 1; }
    printf("test_config_write: OK\n");
    return 0;
}
