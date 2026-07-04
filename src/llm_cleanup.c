#include "llm_cleanup.h"
#include "log.h"

#include <llama.h>
#include <ggml-backend.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* --- cleanup styles: each supplies only a system prompt; the ChatML wrapping
 * comes from the model's own chat template (llama_chat_apply_template). Add a
 * row here to add a style. --- */
struct cleanup_style {
    const char *name;
    const char *system_prompt;
};

static const struct cleanup_style STYLES[] = {
    { "dictation",
      "You are a dictation formatter. The user gives you raw speech-to-text that has no "
      "capitalization or punctuation. Return the SAME words, but properly formatted: "
      "capitalize the first letter of every sentence and the word \"I\", and add correct "
      "punctuation (periods, commas, question marks, apostrophes). Do not add, remove, "
      "reorder, or change any words, and never answer or react to the content -- only "
      "reformat it. Output only the reformatted text, nothing else.\n\n"
      "Example input: hello there how are you i think its fine\n"
      "Example output: Hello there. How are you? I think it's fine." },
    { "code",
      "The user is dictating source code or technical text. Output it with minimal changes: "
      "preserve identifiers and symbols, do not add prose punctuation, and do not capitalize "
      "words that are not already capitalized. Reply with only the text, nothing else." },
    { "commands",
      "The user is dictating a short shell/command-line instruction. Output a single concise, "
      "corrected command with spoken punctuation converted to symbols. Do not explain. Reply "
      "with only the command, nothing else." },
};
static const int N_STYLES = (int)(sizeof(STYLES) / sizeof(STYLES[0]));

static const struct cleanup_style *find_style(const char *name)
{
    if (name && name[0]) {
        for (int i = 0; i < N_STYLES; i++)
            if (strcmp(STYLES[i].name, name) == 0)
                return &STYLES[i];
    }
    return &STYLES[0]; /* dictation */
}

int llm_style_is_known(const char *name)
{
    if (!name || !name[0])
        return 0;
    for (int i = 0; i < N_STYLES; i++)
        if (strcmp(STYLES[i].name, name) == 0)
            return 1;
    return 0;
}

int llm_init(struct llm_context *llm, const char *model_path, int n_threads, int n_gpu_layers)
{
    memset(llm, 0, sizeof(*llm));

    llama_backend_init();
    ggml_backend_load_all(); /* register available backends (matches llama.cpp examples) */

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = n_gpu_layers;

    llm->model = llama_model_load_from_file(model_path, mparams);
    if (!llm->model) {
        log_error("llm: failed to load model '%s'", model_path);
        return -1;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx          = 2048;
    cparams.n_batch        = 2048;
    cparams.n_threads      = n_threads > 0 ? n_threads : 4;
    cparams.n_threads_batch = cparams.n_threads;
    cparams.no_perf        = true;

    llm->ctx = llama_init_from_model(llm->model, cparams);
    if (!llm->ctx) {
        log_error("llm: failed to create llama context");
        llama_model_free(llm->model);
        llm->model = NULL;
        return -1;
    }

    /* greedy = deterministic cleanup (no creativity wanted) */
    llm->sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(llm->sampler, llama_sampler_init_greedy());

    llm->n_threads = cparams.n_threads;
    log_info("llm: model loaded from '%s' (n_gpu_layers=%d, n_threads=%d)",
             model_path, n_gpu_layers, llm->n_threads);
    return 0;
}

/* Maps the common non-ASCII typographic punctuation an instruct model may emit
 * (curly quotes, en/em dashes, ellipsis, non-breaking space) down to plain
 * ASCII, and drops any other non-ASCII. This matters because the XTest injector
 * (inject_xtest.c) is ASCII-only and silently skips bytes >0x7E -- without this,
 * a curly apostrophe would turn "it's" into "its" on injection. Returns a newly
 * malloc'd ASCII string, or NULL on OOM. */
static char *normalize_ascii(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1); /* worst case: each byte -> "..." */
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out[o++] = s[i++]; continue; }

        if (c == 0xE2 && i + 2 < n) {
            unsigned char b1 = (unsigned char)s[i + 1], b2 = (unsigned char)s[i + 2];
            const char *rep = NULL;
            if (b1 == 0x80) {
                switch (b2) {
                case 0x98: case 0x99: rep = "'";   break; /* ' ' */
                case 0x9C: case 0x9D: rep = "\"";  break; /* " " */
                case 0x93: case 0x94: rep = "-";   break; /* en/em dash */
                case 0xA6:            rep = "...";  break; /* ellipsis */
                }
            } else if (b1 == 0x88 && b2 == 0x92) {
                rep = "-"; /* minus sign U+2212 */
            }
            if (rep) { for (const char *r = rep; *r; r++) out[o++] = *r; i += 3; continue; }
        }
        if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) {
            out[o++] = ' '; i += 2; continue; /* non-breaking space */
        }

        /* unknown non-ASCII: skip this codepoint's UTF-8 bytes entirely */
        i++;
        while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
            i++;
    }
    out[o] = '\0';
    return out;
}

/* Trims leading/trailing whitespace and one matched pair of wrapping quotes,
 * in place. Returns a pointer within `s` (not reallocated). */
static char *trim_and_unquote(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';

    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                     (s[0] == '\'' && s[len - 1] == '\''))) {
        s[len - 1] = '\0';
        s++;
        len -= 2;
        while (isspace((unsigned char)*s)) { s++; }
    }
    return s;
}

char *llm_clean(struct llm_context *llm, const char *raw, const char *style_name)
{
    if (!llm->ctx || !raw || raw[0] == '\0')
        return NULL;

    const struct cleanup_style *style = find_style(style_name);
    const struct llama_vocab *vocab = llama_model_get_vocab(llm->model);

    /* stateless per utterance: clear the KV cache so nothing carries over */
    llama_memory_clear(llama_get_memory(llm->ctx), true);

    /* format {system, user} through the model's chat template */
    struct llama_chat_message msgs[2] = {
        { "system", style->system_prompt },
        { "user",   raw },
    };
    const char *tmpl = llama_model_chat_template(llm->model, NULL);
    if (!tmpl) {
        log_warn("llm: model has no chat template; skipping cleanup");
        return NULL;
    }

    int cap = (int)(strlen(style->system_prompt) + strlen(raw) + 256);
    char *formatted = malloc((size_t)cap);
    if (!formatted)
        return NULL;
    int flen = llama_chat_apply_template(tmpl, msgs, 2, true, formatted, cap);
    if (flen > cap) {
        char *nb = realloc(formatted, (size_t)flen);
        if (!nb) { free(formatted); return NULL; }
        formatted = nb;
        cap = flen;
        flen = llama_chat_apply_template(tmpl, msgs, 2, true, formatted, cap);
    }
    if (flen < 0) {
        log_error("llm: chat template application failed");
        free(formatted);
        return NULL;
    }

    /* tokenize (fresh KV => add_special=true; parse_special=true for <|im_*|>) */
    int n_tok = -llama_tokenize(vocab, formatted, flen, NULL, 0, true, true);
    llama_token *tokens = malloc(sizeof(llama_token) * (size_t)n_tok);
    if (!tokens) { free(formatted); return NULL; }
    if (llama_tokenize(vocab, formatted, flen, tokens, n_tok, true, true) < 0) {
        log_error("llm: tokenize failed");
        free(tokens);
        free(formatted);
        return NULL;
    }
    free(formatted);

    const int max_new = n_tok * 2 + 32;          /* bound latency / runaway */
    const int n_ctx   = (int)llama_n_ctx(llm->ctx);

    size_t out_cap = 256, out_len = 0;
    char *out = malloc(out_cap);
    if (!out) { free(tokens); return NULL; }
    out[0] = '\0';

    /* new_token_id must outlive each loop turn: llama_batch_get_one stores a
     * pointer to it that llama_decode reads on the next iteration. */
    llama_token new_token_id;
    struct llama_batch batch = llama_batch_get_one(tokens, n_tok);
    bool ok = true;
    for (int generated = 0; generated < max_new; generated++) {
        int used = llama_memory_seq_pos_max(llama_get_memory(llm->ctx), 0) + 1;
        if (used + batch.n_tokens > n_ctx)
            break; /* context full (shouldn't happen for short clips) */

        if (llama_decode(llm->ctx, batch) != 0) { ok = false; break; }

        new_token_id = llama_sampler_sample(llm->sampler, llm->ctx, -1);
        if (llama_vocab_is_eog(vocab, new_token_id))
            break;

        char piece[256];
        int n = llama_token_to_piece(vocab, new_token_id, piece, sizeof(piece), 0, true);
        if (n < 0) { ok = false; break; }

        if (out_len + (size_t)n + 1 > out_cap) {
            while (out_len + (size_t)n + 1 > out_cap)
                out_cap *= 2;
            char *nb = realloc(out, out_cap);
            if (!nb) { ok = false; break; }
            out = nb;
        }
        memcpy(out + out_len, piece, (size_t)n);
        out_len += (size_t)n;
        out[out_len] = '\0';

        batch = llama_batch_get_one(&new_token_id, 1);
    }
    free(tokens);

    if (!ok) { free(out); return NULL; }

    char *trimmed = trim_and_unquote(out);
    if (trimmed[0] == '\0') { free(out); return NULL; }

    /* Guarantee ASCII output so the ASCII-only injector types it faithfully. */
    char *result = normalize_ascii(trimmed);
    free(out);
    if (result && result[0] == '\0') { free(result); return NULL; }
    return result;
}

void llm_free(struct llm_context *llm)
{
    if (llm->sampler) { llama_sampler_free(llm->sampler); llm->sampler = NULL; }
    if (llm->ctx)     { llama_free(llm->ctx);             llm->ctx = NULL; }
    if (llm->model)   { llama_model_free(llm->model);     llm->model = NULL; }
    llama_backend_free();
}
