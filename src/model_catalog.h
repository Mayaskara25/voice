#ifndef DICTATION_MODEL_CATALOG_H
#define DICTATION_MODEL_CATALOG_H

#include "config.h"   /* CONFIG_PATH_MAX */

#include <stdbool.h>
#include <stddef.h>

/* The downloadable-model catalog (Phase D), parsed from configs/models.conf.
 *
 * Kept as an external data file rather than a hardcoded C table so that a moved
 * HuggingFace URL is a one-line edit instead of a recompile. Format is
 * pipe-separated (a record has 6 fields, which key=value handles badly):
 *
 *   kind|id|display name|filename|url|size_bytes
 *
 * Malformed lines are warned about and skipped, mirroring how config.c treats
 * unknown keys -- a bad catalog row must never be fatal. */

struct model_entry {
    char kind[8];                    /* "whisper" | "llama" */
    char id[64];                     /* stable short name (--fetch-model, config) */
    char display[96];                /* label for the setup window */
    char filename[128];              /* basename under models/ */
    char url[512];
    long size;                       /* advisory, progress bar only; 0 = unknown */
    bool present;                    /* filled by catalog_refresh_presence() */
    char path[CONFIG_PATH_MAX];      /* "<models_dir>/<filename>", e.g. "./models/x.bin" */
};

struct model_catalog {
    struct model_entry *e;
    size_t n;
};

/* Parses the catalog at `path` into *c (which is overwritten, not appended to).
 * Returns 0 on success, -1 if the file can't be opened. An empty or
 * all-malformed file is still a success with n == 0. Call catalog_free() when
 * done, including after a successful load with n == 0. */
int catalog_load(struct model_catalog *c, const char *path);

/* Recomputes entry->path from `models_dir` and entry->present from stat(2).
 * `models_dir` is used verbatim (no ~ or $HOME expansion -- config.c does none
 * either, and the daemon is always started after a cd into the project dir). */
void catalog_refresh_presence(struct model_catalog *c, const char *models_dir);

/* Returns the entry with this id, or NULL. */
const struct model_entry *catalog_find(const struct model_catalog *c, const char *id);

void catalog_free(struct model_catalog *c);

#endif
