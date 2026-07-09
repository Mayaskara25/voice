#include "dictation_directives.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct prefix_rule {
    const char *phrase;
    enum dictation_cleanup_override override;
    const char *style;
    bool lowercase;
    bool spell;
};

static const struct prefix_rule RULES[] = {
    { "<keep all lowercase>", DICTATION_CLEANUP_SKIP,  NULL,       true,  false },
    { "keep all lowercase",   DICTATION_CLEANUP_SKIP,  NULL,       true,  false },
    { "all lowercase",        DICTATION_CLEANUP_SKIP,  NULL,       true,  false },
    { "lowercase",            DICTATION_CLEANUP_SKIP,  NULL,       true,  false },
    { "<spell>",              DICTATION_CLEANUP_SKIP,  NULL,       false, true  },
    { "spell word",           DICTATION_CLEANUP_SKIP,  NULL,       false, true  },
    { "spell",                DICTATION_CLEANUP_SKIP,  NULL,       false, true  },
    { "<literal>",            DICTATION_CLEANUP_SKIP,  NULL,       false, false },
    { "type literally",       DICTATION_CLEANUP_SKIP,  NULL,       false, false },
    { "literal",              DICTATION_CLEANUP_SKIP,  NULL,       false, false },
    { "no cleanup",           DICTATION_CLEANUP_SKIP,  NULL,       false, false },
    { "<command>",            DICTATION_CLEANUP_STYLE, "commands", false, false },
    { "command",              DICTATION_CLEANUP_STYLE, "commands", false, false },
    { "<code>",               DICTATION_CLEANUP_STYLE, "code",     false, false },
    { "code mode",            DICTATION_CLEANUP_STYLE, "code",     false, false },
};

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, n + 1);
    return copy;
}

static char *trim_in_place(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    return s;
}

static bool is_boundary_char(char c)
{
    return c == '\0' || isspace((unsigned char)c) || c == ':' || c == ',' || c == '-';
}

static bool consume_prefix(const char *s, const char *phrase, const char **payload)
{
    size_t n = strlen(phrase);
    if (strncasecmp(s, phrase, n) != 0 || !is_boundary_char(s[n]))
        return false;

    const char *p = s + n;
    while (*p && (isspace((unsigned char)*p) || *p == ':' || *p == ',' || *p == '-'))
        p++;
    *payload = p;
    return *p != '\0';
}

static void lowercase_ascii(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static bool is_single_letter_token(const char *start, size_t len)
{
    return len == 1 && isalpha((unsigned char)start[0]);
}

static char *collapse_spelled_letters(const char *s)
{
    char *out = malloc(strlen(s) + 1);
    if (!out)
        return NULL;

    size_t o = 0;
    bool saw_letter = false;
    bool all_letters = true;
    const char *p = s;

    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == '-'))
            p++;
        if (!*p)
            break;

        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '-')
            p++;
        size_t len = (size_t)(p - start);

        if (!is_single_letter_token(start, len)) {
            all_letters = false;
            break;
        }

        out[o++] = (char)tolower((unsigned char)start[0]);
        saw_letter = true;
    }

    if (!saw_letter || !all_letters) {
        free(out);
        return xstrdup(s);
    }

    out[o] = '\0';
    return out;
}

static int set_result(struct dictation_directive_result *out,
                      char *text,
                      enum dictation_cleanup_override override,
                      const char *style)
{
    if (!text)
        return -1;

    out->text = text;
    out->cleanup_override = override;
    out->cleanup_style[0] = '\0';
    if (style)
        snprintf(out->cleanup_style, sizeof(out->cleanup_style), "%s", style);
    return 0;
}

int dictation_directives_apply(const char *raw, struct dictation_directive_result *out)
{
    memset(out, 0, sizeof(*out));
    out->cleanup_override = DICTATION_CLEANUP_DEFAULT;

    if (!raw)
        return -1;

    char *copy = xstrdup(raw);
    if (!copy)
        return -1;
    char *input = trim_in_place(copy);
    if (input[0] == '\0') {
        free(copy);
        return -1;
    }

    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        const char *payload = NULL;
        if (!consume_prefix(input, RULES[i].phrase, &payload))
            continue;

        char *text = RULES[i].spell ? collapse_spelled_letters(payload) : xstrdup(payload);
        free(copy);
        if (!text)
            return -1;

        char *trimmed = trim_in_place(text);
        if (trimmed != text)
            memmove(text, trimmed, strlen(trimmed) + 1);
        if (RULES[i].lowercase)
            lowercase_ascii(text);

        return set_result(out, text, RULES[i].override, RULES[i].style);
    }

    char *text = xstrdup(input);
    free(copy);
    return set_result(out, text, DICTATION_CLEANUP_DEFAULT, NULL);
}

void dictation_directives_free(struct dictation_directive_result *res)
{
    if (!res)
        return;
    free(res->text);
    res->text = NULL;
    res->cleanup_override = DICTATION_CLEANUP_DEFAULT;
    res->cleanup_style[0] = '\0';
}
