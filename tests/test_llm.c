/* test_llm: exercises the LLM cleanup end to end -- loads the GGUF, cleans a
 * messy lowercase transcript, and checks the result is plausible (content
 * preserved, casing/punctuation improved, no runaway). Skips (exit 0) if the
 * model is absent, like test_stt, so the suite runs on a fresh checkout.
 * Greedy decoding is deterministic per model, but assertions stay loose. */
#include "llm_cleanup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MODEL_PATH "models/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    struct stat st;
    if (stat(MODEL_PATH, &st) != 0) {
        printf("test_llm: SKIP (model '%s' not present)\n", MODEL_PATH);
        return 0;
    }

    int ngl = 99;
    const char *env = getenv("DICT_TEST_NGL");
    if (env) ngl = atoi(env);

    struct llm_context llm;
    if (llm_init(&llm, MODEL_PATH, 4, ngl) != 0) {
        fprintf(stderr, "test_llm: FAIL llm_init\n");
        return 1;
    }

    const char *raw = "hello world how are you i am doing well today";
    char *clean = llm_clean(&llm, raw, "dictation");
    llm_free(&llm);

    printf("test_llm: raw   : %s\n", raw);
    printf("test_llm: clean : %s\n", clean ? clean : "(NULL)");

    CHECK(clean != NULL && clean[0] != '\0', "cleanup returns non-empty text");
    if (clean && clean[0]) {
        char low[512];
        snprintf(low, sizeof(low), "%s", clean);
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        CHECK(strstr(low, "hello") && strstr(low, "world"), "content words preserved");
        CHECK(isupper((unsigned char)clean[0]), "first letter capitalized");
        size_t len = strlen(clean);
        char last = clean[len - 1];
        CHECK(last == '.' || last == '!' || last == '?', "ends with terminal punctuation");
        CHECK(len < strlen(raw) * 3, "no runaway output (length sane)");
    }

    if (clean) free(clean);
    if (failures) { fprintf(stderr, "test_llm: %d failure(s)\n", failures); return 1; }
    printf("test_llm: OK\n");
    return 0;
}
