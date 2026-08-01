/* test_setup: the parts of the setup window that don't need an X display --
 * the log ring's whole-line eviction, the tail view's line alignment, and the
 * selection -> configs/local.conf write. Opens no Display, so `make test` stays
 * headless-safe. */
#include "config.h"
#include "config_write.h"
#include "setup_gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_log_ring(void)
{
    enum { CAP = 16 };
    char buf[CAP + 1] = "";
    size_t len = 0;

    len = setup_log_append(buf, CAP, len, "line1\n", 6);
    CHECK(len == 6 && strcmp(buf, "line1\n") == 0, "first append");

    len = setup_log_append(buf, CAP, len, "line2\n", 6);
    CHECK(len == 12 && strcmp(buf, "line1\nline2\n") == 0, "second append fits");

    /* Overflow: the oldest WHOLE line goes, never half of one -- a partial
     * first line renders as garbage in the pane. */
    len = setup_log_append(buf, CAP, len, "line3\n", 6);
    CHECK(len == 12 && strcmp(buf, "line2\nline3\n") == 0,
          "overflow evicts a whole line, not just the overflowing bytes");
    CHECK(buf[len] == '\0', "the ring stays NUL-terminated for mu_text");

    /* A single write larger than the whole ring keeps its newest tail. */
    len = setup_log_append(buf, CAP, len, "0123456789abcdefghij", 20);
    CHECK(len == CAP, "an oversized append is capped at the ring size");
    CHECK(strcmp(buf, "456789abcdefghij") == 0, "an oversized append keeps its newest bytes");

    /* No newline anywhere: eviction still has to make room. */
    char nb[CAP + 1] = "";
    size_t nl = setup_log_append(nb, CAP, 0, "aaaaaaaaaa", 10);
    nl = setup_log_append(nb, CAP, nl, "bbbbbbbbbb", 10);
    CHECK(nl <= CAP, "a newline-free ring never exceeds its capacity");
    CHECK(memcmp(nb + nl - 4, "bbbb", 4) == 0, "newest bytes survive with no newline to cut at");
}

static void test_log_view(void)
{
    const char *s = "line1\nline2\nline3\n";
    size_t len = strlen(s);

    CHECK(setup_log_view(s, len, 100) == s, "a short log is shown whole");

    const char *v = setup_log_view(s, len, 8);
    CHECK(strcmp(v, "line3\n") == 0,
          "the tail view starts at a line boundary, not mid-line");
}

static void test_write_selection(void)
{
    char dir[] = "/tmp/dictation_setup_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }

    char example[256], local[256];
    snprintf(example, sizeof(example), "%s/example.conf", dir);
    snprintf(local, sizeof(local), "%s/local.conf", dir);

    FILE *f = fopen(example, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs("# a documented template\n"
          "whisper_model_path=./models/placeholder.bin\n"
          "llama_model_path=./models/placeholder.gguf\n"
          "n_threads=8\n", f);
    fclose(f);

    CHECK(setup_write_selection(local, example,
                                "./models/ggml-base.en.bin",
                                "./models/q.gguf") == 0, "selection is written");

    struct app_config cfg;
    CHECK(config_load(local, &cfg) == 0, "local.conf loads");
    CHECK(strcmp(cfg.whisper_model_path, "./models/ggml-base.en.bin") == 0,
          "whisper_model_path written");
    CHECK(strcmp(cfg.llama_model_path, "./models/q.gguf") == 0, "llama_model_path written");
    CHECK(cfg.n_threads == 8, "an unrelated setting survived");

    FILE *r = fopen(local, "r");
    char text[4096] = "";
    size_t n = fread(text, 1, sizeof(text) - 1, r);
    text[n] = '\0';
    fclose(r);
    CHECK(strstr(text, "# a documented template") != NULL,
          "the template's comments survived into local.conf");

    /* "none (raw whisper)" writes an empty path, which disables cleanup. */
    CHECK(setup_write_selection(local, example, "./models/ggml-base.en.bin", "") == 0,
          "the 'none' cleanup choice is written");
    CHECK(config_load(local, &cfg) == 0, "local.conf still loads");
    CHECK(cfg.llama_model_path[0] == '\0', "empty llama_model_path disables cleanup");
    CHECK(strcmp(cfg.whisper_model_path, "./models/ggml-base.en.bin") == 0,
          "the whisper choice is unchanged by a cleanup change");

    unlink(local);
    unlink(example);
    rmdir(dir);
}

/* Pure string logic, so every awkward boundary is cheap to assert -- and these
 * are exactly the cases clicking through the window would never reach. */
static void test_path_within(void)
{
    CHECK(setup_path_within("/home/u/voice/models/a.bin", "/home/u/voice") == 1,
          "a file under the root is inside it");
    CHECK(setup_path_within("/home/u/voice", "/home/u/voice") == 1,
          "the root itself counts as inside");
    CHECK(setup_path_within("/home/u/voice/", "/home/u/voice") == 1,
          "trailing slash on the path");
    CHECK(setup_path_within("/home/u/voice/models/a.bin", "/home/u/voice/") == 1,
          "trailing slash on the root");

    /* The one that matters: a plain strncmp() prefix test passes this, and
     * this function authorises deletion. */
    CHECK(setup_path_within("/home/u/voice-backup/models/a.bin", "/home/u/voice") == 0,
          "a sibling sharing a name prefix is NOT inside");
    CHECK(setup_path_within("/home/u/voicex", "/home/u/voice") == 0,
          "prefix without a separator is NOT inside");

    CHECK(setup_path_within("/etc/passwd", "/home/u/voice") == 0,
          "an unrelated path is outside");
    CHECK(setup_path_within("/home/u/voice/models/a.bin", "") == 0,
          "an empty root matches nothing (fails safe)");
    CHECK(setup_path_within("", "/home/u/voice") == 0, "an empty path matches nothing");
    CHECK(setup_path_within(NULL, "/home/u/voice") == 0, "NULL is handled");
}

static void test_plan_removal(void)
{
    char dir[] = "/tmp/dictation_rm_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "scratch dir");

    /* Graded sizes so the compiler can prove each snprintf fits -- otherwise
     * -Wformat-truncation fires on every one of these and the build stops
     * being warning-clean. */
    char root[64], subdir[128], outside_dir[128];
    char inside[192], link_in[256], outside[256], link_out[256], dangling[256], missing[256];
    char link_inside[256];
    snprintf(root, sizeof(root), "%s/project", dir);
    snprintf(subdir, sizeof(subdir), "%s/models", root);
    mkdir(root, 0700);
    mkdir(subdir, 0700);

    snprintf(inside, sizeof(inside), "%s/real.bin", root);
    FILE *f = fopen(inside, "wb");
    if (f) { fwrite("0123456789", 1, 10, f); fclose(f); }

    /* A plain file. */
    snprintf(link_in, sizeof(link_in), "%s/plain.bin", subdir);
    f = fopen(link_in, "wb");
    if (f) { fwrite("abcd", 1, 4, f); fclose(f); }

    struct model_removal r;
    CHECK(setup_plan_removal(link_in, root, &r) == 0 && r.ok, "plain file can be removed");
    CHECK(!r.is_symlink && r.target[0] == '\0', "plain file has no target");
    CHECK(r.bytes == 4, "plain file frees its own size");

    /* A symlink whose target is inside the project: both go. */

    snprintf(link_inside, sizeof(link_inside), "%s/linked.bin", subdir);
    CHECK(symlink(inside, link_inside) == 0, "symlink into the project");
    CHECK(setup_plan_removal(link_inside, root, &r) == 0 && r.ok, "linked model can be removed");
    CHECK(r.is_symlink && r.target_inside, "target recognised as inside the project");
    CHECK(r.bytes == 10, "frees the TARGET's size, not the link's");

    /* A RELATIVE symlink -- the form the README's `ln -sf` actually creates
     * (models/x.bin -> ../whisper.cpp/models/x.bin), and so the form that runs
     * in production. realpath resolves it against the *link's* directory, not
     * the CWD, which is the coupling worth pinning down. */
    char rel_link[256];
    snprintf(rel_link, sizeof(rel_link), "%s/relative.bin", subdir);
    CHECK(symlink("../real.bin", rel_link) == 0, "relative symlink into the project");
    CHECK(setup_plan_removal(rel_link, root, &r) == 0 && r.ok, "relative link can be removed");
    CHECK(r.is_symlink && r.target_inside,
          "a relative target still resolves as inside the project");
    CHECK(r.bytes == 10, "a relative link frees the target's size");
    CHECK(strstr(r.target, "..") == NULL, "the stored target is fully resolved, not '..'-relative");

    /* A symlink pointing outside: the link goes, the target must not. */
    snprintf(outside_dir, sizeof(outside_dir), "%s/elsewhere", dir);
    mkdir(outside_dir, 0700);
    snprintf(outside, sizeof(outside), "%s/other.bin", outside_dir);
    f = fopen(outside, "wb");
    if (f) { fwrite("xxxxxxx", 1, 7, f); fclose(f); }
    snprintf(link_out, sizeof(link_out), "%s/external.bin", subdir);
    CHECK(symlink(outside, link_out) == 0, "symlink out of the project");
    CHECK(setup_plan_removal(link_out, root, &r) == 0 && r.ok, "external link can be removed");
    CHECK(r.is_symlink && !r.target_inside, "target recognised as outside the project");
    CHECK(r.bytes == 0, "an outside target frees nothing, because it is kept");

    CHECK(setup_apply_removal(&r) == 0, "removing an external link succeeds");
    struct stat st;
    CHECK(stat(outside, &st) == 0, "the OUTSIDE target survives -- not ours to delete");
    CHECK(lstat(link_out, &st) != 0, "the link itself is gone");

    /* Dangling link: nothing to free, but clearing the stale name is right. */
    snprintf(dangling, sizeof(dangling), "%s/dangling.bin", subdir);
    CHECK(symlink("/nonexistent/nope.bin", dangling) == 0, "dangling symlink");
    CHECK(setup_plan_removal(dangling, root, &r) == 0 && r.ok, "a dangling link may be removed");
    CHECK(r.bytes == 0, "a dangling link frees nothing");
    CHECK(setup_apply_removal(&r) == 0 && lstat(dangling, &st) != 0, "dangling link removed");

    /* If the target cannot be deleted, the link must SURVIVE. Deleting it
     * anyway would strand the bytes: nothing would name them, the row would
     * read "missing", and re-downloading would allocate the space again. Forced
     * by making the target's directory unwritable. */
    char keepdir[128], keeptgt[256], keeplink[256];
    snprintf(keepdir, sizeof(keepdir), "%s/locked", root);
    mkdir(keepdir, 0700);
    snprintf(keeptgt, sizeof(keeptgt), "%s/held.bin", keepdir);
    f = fopen(keeptgt, "wb");
    if (f) { fwrite("zzzzz", 1, 5, f); fclose(f); }
    snprintf(keeplink, sizeof(keeplink), "%s/held-link.bin", subdir);
    CHECK(symlink(keeptgt, keeplink) == 0, "symlink to a soon-to-be-locked target");
    CHECK(setup_plan_removal(keeplink, root, &r) == 0 && r.target_inside, "plan the locked one");
    CHECK(chmod(keepdir, 0500) == 0, "make the target's directory unwritable");

    int rc_locked = setup_apply_removal(&r);
    CHECK(rc_locked != 0, "a failed target delete is reported as failure");
    CHECK(lstat(keeplink, &st) == 0,
          "the link SURVIVES a failed target delete -- otherwise the bytes are stranded");
    CHECK(stat(keeptgt, &st) == 0, "the target is still there too");

    chmod(keepdir, 0700);
    unlink(keeptgt); unlink(keeplink); rmdir(keepdir);

    /* Refusals. */
    CHECK(setup_plan_removal(subdir, root, &r) != 0 && !r.ok, "refuses a directory");

    snprintf(missing, sizeof(missing), "%s/nope.bin", subdir);
    CHECK(setup_plan_removal(missing, root, &r) != 0 && !r.ok, "refuses a missing file");
    CHECK(setup_plan_removal("", root, &r) != 0, "refuses an empty path");
    CHECK(setup_apply_removal(&r) != 0, "apply refuses a plan that was not ok");

    /* Now really delete the in-project link and confirm the target went too. */
    CHECK(setup_plan_removal(link_inside, root, &r) == 0, "re-plan the linked model");
    CHECK(setup_apply_removal(&r) == 0, "removing a linked model succeeds");
    CHECK(lstat(link_inside, &st) != 0, "the link is gone");
    CHECK(stat(inside, &st) != 0, "the in-project target is gone too");

    unlink(link_in); unlink(outside); unlink(rel_link);
    rmdir(outside_dir); rmdir(subdir); rmdir(root); rmdir(dir);
}

int main(void)
{
    test_log_ring();
    test_log_view();
    test_write_selection();
    test_path_within();
    test_plan_removal();

    if (failures) { fprintf(stderr, "test_setup: %d failure(s)\n", failures); return 1; }
    printf("test_setup: OK\n");
    return 0;
}
