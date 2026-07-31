#ifndef DICTATION_CONFIG_WRITE_H
#define DICTATION_CONFIG_WRITE_H

#include <stddef.h>

/* Writing the config back out (Phase D).
 *
 * config_load() is lossy on round-trip: it discards every comment, and
 * configs/example.conf is ~55 lines of documentation against 14 settings. So
 * the setup GUI must not read-modify-write through struct app_config -- it
 * would silently delete the documentation. This module rewrites the file
 * line-preservingly instead: matching lines are replaced, everything else is
 * copied verbatim. */

/* The file the setup GUI writes, and the read-only template it is seeded from.
 * local.conf is gitignored; example.conf is the tracked, commented original. */
#define CONFIG_LOCAL_PATH   "configs/local.conf"
#define CONFIG_EXAMPLE_PATH "configs/example.conf"

/* Rewrites `path`, setting keys[0..n-1] to vals[0..n-1].
 *
 * Every line matching `^[ \t]*<key>[ \t]*=` is replaced *entirely* with
 * `key=value` -- including any trailing inline comment it carried (config.c
 * allows `audio_device=plughw:2,0  # note`). That is the one kind of comment
 * this module does not preserve; it applies only to keys actually being
 * written, and the keys the setup GUI writes carry none. Every other line --
 * comments, blanks, commented-out settings, other keys --
 * is copied byte-for-byte. Keys not present in the file are appended at the end
 * under a `# --- written by dictation-setup ---` header (emitted only if there
 * is something to append, so repeated calls neither duplicate the header nor
 * the keys). A missing file is treated as empty and created.
 *
 * Writes to `<path>.tmp` and rename(2)s it into place, so a crash mid-write
 * leaves the previous config intact rather than a half-written one.
 *
 * Returns 0 on success, -1 on error with a message logged. */
int config_write_keys(const char *path, const char *const *keys,
                      const char *const *vals, size_t n);

/* Creates `local_path` as a byte-for-byte copy of `example_path` if it does not
 * already exist, so local.conf inherits all of example.conf's comments and
 * tuned settings rather than starting as a bare two-line file.
 *
 * Seed-once semantics: an existing local_path is never touched (the create is
 * O_EXCL, so this is safe even against a concurrent setup process).
 *
 * Returns 1 if it created the file, 0 if it already existed, -1 on error. */
int config_bootstrap_local(const char *local_path, const char *example_path);

/* The config file to read when the user didn't pass --config: CONFIG_LOCAL_PATH
 * when it exists, else CONFIG_EXAMPLE_PATH. Both are relative, matching the
 * project convention (nothing expands ~ or $HOME; launchers cd into the project
 * directory first). The returned string is static and must not be freed. */
const char *config_default_path(void);

#endif
