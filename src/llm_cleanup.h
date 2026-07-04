#ifndef DICTATION_LLM_CLEANUP_H
#define DICTATION_LLM_CLEANUP_H

/* Optional LLM cleanup stage (Phase B): fixes casing/punctuation on the raw
 * whisper transcript via llama.cpp, sharing whisper's ggml backend. Runs on the
 * worker thread (like whisper); never touches the GUI/Display. Cleanup is
 * best-effort -- llm_clean() returns NULL on any error and the caller falls
 * back to the raw transcript, so a bad model/prompt never breaks dictation. */

struct llama_model;      /* opaque, like stt_whisper's struct whisper_context */
struct llama_context;
struct llama_sampler;

struct llm_context {
    struct llama_model   *model;
    struct llama_context *ctx;
    struct llama_sampler *sampler;   /* greedy chain, reused across utterances */
    int n_threads;
};

/* Loads the GGUF instruct model. n_gpu_layers > 0 offloads that many layers to
 * the GPU (99 = all); 0 = CPU-only. Returns 0 on success, -1 on error. */
int   llm_init(struct llm_context *llm, const char *model_path, int n_threads, int n_gpu_layers);

/* Cleans `raw` using the named style's system prompt (see llm_cleanup.c). Each
 * call is stateless (KV cache cleared first). Returns a newly malloc'd,
 * NUL-terminated string (caller frees), or NULL on any error/empty output. */
char *llm_clean(struct llm_context *llm, const char *raw, const char *style_name);

/* Returns 1 if `name` is a known cleanup style, else 0. (Unknown styles still
 * work at runtime -- llm_clean falls back to "dictation" -- this just lets the
 * config layer warn.) Safe to call without llm_init. */
int   llm_style_is_known(const char *name);

void  llm_free(struct llm_context *llm);

#endif
