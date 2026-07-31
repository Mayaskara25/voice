#ifndef DICTATION_LLM_STYLES_H
#define DICTATION_LLM_STYLES_H

/* The cleanup-style table (Phase B), split out of llm_cleanup.c so that code
 * which only needs to *name* a style doesn't have to link llama.cpp.
 *
 * config.c validates `cleanup_style` and previously pulled this in via
 * llm_cleanup.h -- that single symbol reference dragged llm_cleanup.o ->
 * libllama.a -> libggml*.a -> the CUDA runtime into every binary linking
 * config.o (which is why the tests/ binaries are ~73 MB). Keeping the table
 * here, free of any llama dependency, lets dictation-setup (Phase D) link
 * config.c with nothing heavier than -lX11. */

struct cleanup_style {
    const char *name;
    const char *system_prompt;
};

/* Returns the named style, or the default ("dictation") if `name` is NULL,
 * empty, or unknown. Never returns NULL. */
const struct cleanup_style *llm_style_find(const char *name);

/* Returns 1 if `name` is a known cleanup style, else 0. (Unknown styles still
 * work at runtime -- llm_style_find falls back to "dictation" -- this just lets
 * the config layer warn.) */
int llm_style_is_known(const char *name);

#endif
