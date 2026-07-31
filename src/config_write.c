#include "config_write.h"
#include "config.h"   /* CONFIG_PATH_MAX */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WRITTEN_HEADER "# --- written by dictation-setup ---\n"

/* True if `line` assigns `key`, i.e. matches ^[ \t]*<key>[ \t]*=.
 *
 * The anchor matters: example.conf's prose mentions both key names inside
 * comments ("point whisper_model_path at ..."), and a commented-out
 * `#whisper_model_path=old` must survive untouched. Only a line whose first
 * non-blank run is exactly the key, followed by '=', is an assignment. */
static int line_assigns(const char *line, const char *key)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0)
        return 0;
    p += klen;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '=';
}

int config_write_keys(const char *path, const char *const *keys,
                      const char *const *vals, size_t n)
{
    char tmp[CONFIG_PATH_MAX];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp)) {
        log_error("config_write: path '%s' too long for a .tmp sibling", path);
        return -1;
    }

    char *handled = calloc(n ? n : 1, 1);
    if (!handled) {
        log_error("config_write: out of memory");
        return -1;
    }

    FILE *in = fopen(path, "r");
    if (!in && errno != ENOENT) {
        log_error("config_write: could not read '%s': %s", path, strerror(errno));
        free(handled);
        return -1;
    }

    FILE *out = fopen(tmp, "w");
    if (!out) {
        log_error("config_write: could not create '%s': %s", tmp, strerror(errno));
        if (in)
            fclose(in);
        free(handled);
        return -1;
    }

    int rc = 0;
    int last_had_newline = 1;   /* an empty input file needs no separator */

    if (in) {
        /* getline, not a fixed buffer: this loop copies unmatched lines
         * verbatim, and a fixed buffer would split a long comment into two
         * lines that no longer round-trip byte-for-byte. */
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, in)) != -1) {
            size_t match = n;
            for (size_t i = 0; i < n; i++) {
                if (line_assigns(line, keys[i])) {
                    match = i;
                    break;
                }
            }
            if (match < n) {
                /* Rewrite every occurrence, not just the first: config_load
                 * takes last-wins, so leaving a later duplicate in place would
                 * silently override the value we just wrote. */
                if (fprintf(out, "%s=%s\n", keys[match], vals[match]) < 0)
                    rc = -1;
                handled[match] = 1;
                last_had_newline = 1;
            } else {
                if (fwrite(line, 1, (size_t)len, out) != (size_t)len)
                    rc = -1;
                last_had_newline = (len > 0 && line[len - 1] == '\n');
            }
            if (rc != 0)
                break;
        }
        free(line);
        fclose(in);
    }

    if (rc == 0) {
        int wrote_header = 0;
        for (size_t i = 0; i < n; i++) {
            if (handled[i])
                continue;
            if (!wrote_header) {
                /* A file not ending in a newline would otherwise glue its last
                 * line onto our header. */
                if (!last_had_newline && fputc('\n', out) == EOF)
                    rc = -1;
                if (rc == 0 && fputs("\n" WRITTEN_HEADER, out) == EOF)
                    rc = -1;
                wrote_header = 1;
            }
            if (rc == 0 && fprintf(out, "%s=%s\n", keys[i], vals[i]) < 0)
                rc = -1;
            if (rc != 0)
                break;
        }
    }

    /* fclose, not just ferror: buffered write failures (ENOSPC in particular)
     * surface at flush time, and an unchecked one turns "atomic write" into
     * "atomically installed a truncated config". */
    if (fclose(out) != 0)
        rc = -1;
    free(handled);

    if (rc != 0) {
        log_error("config_write: failed writing '%s': %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        log_error("config_write: could not rename '%s' -> '%s': %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_bootstrap_local(const char *local_path, const char *example_path)
{
    /* O_EXCL rather than stat-then-copy: seeding must never clobber a config
     * the user has edited, and the check-then-act version loses that race. */
    int dst = open(local_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dst < 0) {
        if (errno == EEXIST)
            return 0;
        log_error("config_write: could not create '%s': %s", local_path, strerror(errno));
        return -1;
    }

    FILE *src = fopen(example_path, "r");
    if (!src) {
        log_error("config_write: could not read template '%s': %s", example_path, strerror(errno));
        close(dst);
        unlink(local_path);
        return -1;
    }

    char buf[4096];
    size_t got;
    int rc = 0;
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0) {
        size_t off = 0;
        while (off < got) {
            ssize_t w = write(dst, buf + off, got - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
        if (rc != 0)
            break;
    }
    if (ferror(src))
        rc = -1;

    fclose(src);
    if (close(dst) != 0)
        rc = -1;

    if (rc != 0) {
        log_error("config_write: failed seeding '%s' from '%s': %s",
                  local_path, example_path, strerror(errno));
        unlink(local_path);
        return -1;
    }

    log_info("config: seeded %s from %s", local_path, example_path);
    return 1;
}

const char *config_default_path(void)
{
    struct stat st;
    if (stat(CONFIG_LOCAL_PATH, &st) == 0 && S_ISREG(st.st_mode))
        return CONFIG_LOCAL_PATH;
    return CONFIG_EXAMPLE_PATH;
}
