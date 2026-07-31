#include "model_catalog.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* config.c's trim() is file-static and this file must not drag config.o in, so
 * the same six-line idiom is duplicated rather than exported. Kept byte-for-byte
 * identical to config.c:36 so the two can't drift in behaviour. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* Copies `src` into a fixed field, rejecting (rather than truncating) anything
 * too long: a silently truncated URL is a 404 three phases later, in the
 * downloader, where it looks like upstream moved the file. Returns 0 on success. */
static int copy_field(char *dst, size_t dstsz, const char *src,
                      const char *what, const char *path, int lineno)
{
    size_t len = strlen(src);
    if (len == 0) {
        log_warn("models: %s:%d: empty %s, skipping line", path, lineno, what);
        return -1;
    }
    if (len >= dstsz) {
        log_warn("models: %s:%d: %s too long (%zu >= %zu), skipping line",
                 path, lineno, what, len, dstsz);
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

/* Splits `s` in place on '|' into exactly `n` fields. Returns 0 on success, -1
 * if the count doesn't match. Note there is deliberately no inline-'#'-comment
 * handling here (config.c:122 does have it): a '#' is legal inside a URL or a
 * display name, and stripping it would silently corrupt the record. */
static int split_fields(char *s, char **out, size_t n)
{
    size_t i = 0;
    for (;;) {
        char *bar = strchr(s, '|');
        if (i >= n)
            return -1;              /* too many fields */
        if (!bar) {
            out[i++] = s;
            break;
        }
        *bar = '\0';
        out[i++] = s;
        s = bar + 1;
    }
    return i == n ? 0 : -1;
}

static int parse_line(struct model_entry *m, char *line, const char *path, int lineno)
{
    char *f[6];
    if (split_fields(line, f, 6) != 0) {
        log_warn("models: %s:%d: expected 6 '|'-separated fields, skipping line", path, lineno);
        return -1;
    }
    for (size_t i = 0; i < 6; i++)
        f[i] = trim(f[i]);

    memset(m, 0, sizeof(*m));
    if (copy_field(m->kind, sizeof(m->kind), f[0], "kind", path, lineno) != 0 ||
        copy_field(m->id, sizeof(m->id), f[1], "id", path, lineno) != 0 ||
        copy_field(m->display, sizeof(m->display), f[2], "display name", path, lineno) != 0 ||
        copy_field(m->filename, sizeof(m->filename), f[3], "filename", path, lineno) != 0 ||
        copy_field(m->url, sizeof(m->url), f[4], "url", path, lineno) != 0)
        return -1;

    if (strcmp(m->kind, "whisper") != 0 && strcmp(m->kind, "llama") != 0) {
        log_warn("models: %s:%d: unknown kind '%s' (want whisper|llama), skipping line",
                 path, lineno, m->kind);
        return -1;
    }

    /* Size is advisory (progress bar only), so a bad one degrades the bar to a
     * byte counter rather than dropping an otherwise usable model. */
    errno = 0;
    char *end = NULL;
    long size = strtol(f[5], &end, 10);
    if (errno != 0 || end == f[5] || *end != '\0' || size < 0) {
        log_warn("models: %s:%d: bad size_bytes '%s', treating as unknown", path, lineno, f[5]);
        size = 0;
    }
    m->size = size;
    return 0;
}

int catalog_load(struct model_catalog *c, const char *path)
{
    c->e = NULL;
    c->n = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("models: could not open '%s': %s", path, strerror(errno));
        return -1;
    }

    /* getline rather than config.c's fixed char[1024]: a catalog record is a
     * URL plus a display name, and a silently split line would parse as two
     * malformed ones. */
    char *line = NULL;
    size_t cap = 0;
    size_t alloc = 0;
    int lineno = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, f)) != -1) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;

        if (c->n == alloc) {
            size_t next = alloc ? alloc * 2 : 8;
            struct model_entry *grown = realloc(c->e, next * sizeof(*grown));
            if (!grown) {
                log_error("models: out of memory reading '%s'", path);
                break;
            }
            c->e = grown;
            alloc = next;
        }

        if (parse_line(&c->e[c->n], s, path, lineno) == 0)
            c->n++;
    }

    free(line);
    fclose(f);
    return 0;
}

void catalog_refresh_presence(struct model_catalog *c, const char *models_dir)
{
    for (size_t i = 0; i < c->n; i++) {
        struct model_entry *m = &c->e[i];
        int n = snprintf(m->path, sizeof(m->path), "%s/%s", models_dir, m->filename);
        if (n < 0 || (size_t)n >= sizeof(m->path)) {
            log_warn("models: path for '%s' exceeds %zu bytes, treating as missing",
                     m->id, sizeof(m->path));
            m->path[0] = '\0';
            m->present = false;
            continue;
        }
        struct stat st;
        m->present = (stat(m->path, &st) == 0 && S_ISREG(st.st_mode));
    }
}

const struct model_entry *catalog_find(const struct model_catalog *c, const char *id)
{
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].id, id) == 0)
            return &c->e[i];
    }
    return NULL;
}

void catalog_free(struct model_catalog *c)
{
    free(c->e);
    c->e = NULL;
    c->n = 0;
}
