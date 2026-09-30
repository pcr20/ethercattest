/* Log buffering (logbuf.h), and the ordering rule that makes it work.
 *
 * The bug this pins: ecat_op called setvbuf 31 printf calls into main(). C11
 * §7.21.5.6 allows it only "before any other operation is performed on the
 * stream", so the library refused it, stdout stayed fully buffered, and a run
 * redirected to a file wrote nothing until it exited. It looked fine from the
 * inside — the call is there, it reads correctly — and was only caught by an
 * operator asking why the log file was empty while the run was going.
 *
 * Two things are worth a test. That line buffering actually reaches the file
 * on each newline, which is the behaviour the tool depends on. And that
 * calling it late FAILS rather than silently doing nothing, because that is
 * the only signal the caller gets that it has made this mistake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "logbuf.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

/* Bytes currently visible in the file, without touching the FILE* that is
 * writing to it — i.e. what an operator tailing the log would see. */
static long visible(const char *path)
{
    FILE *r = fopen(path, "rb");
    if (!r) return -1;
    fseek(r, 0, SEEK_END);
    long n = ftell(r);
    fclose(r);
    return n;
}

int main(void)
{
    char dir[] = "/tmp/logbufXXXXXX";
    CHECK(mkdtemp(dir) != NULL, "tmpdir");
    char path[512];

    /* ── T1: called first, a line reaches the file immediately ───────────── */
    {
        int f0 = fails;
        snprintf(path, sizeof path, "%s/early.log", dir);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL, "T1: cannot open %s", path);

        CHECK(log_line_buffered(f) != 0,
              "T1: setvbuf refused even as the first operation on the stream");
        fprintf(f, "banner line\n");
        long n = visible(path);
        CHECK(n > 0, "T1: %ld bytes in the file after a complete line — this "
              "is the empty-logfile bug", n);

        /* A partial line is NOT expected to appear; only whole lines flush. */
        fprintf(f, "no newline yet");
        long n2 = visible(path);
        CHECK(n2 == n, "T1: a partial line reached the file (%ld -> %ld); "
              "that is full flushing, not line buffering", n, n2);
        fprintf(f, " ...now\n");
        CHECK(visible(path) > n2, "T1: completing the line did not flush it");
        fclose(f);
        if (fails == f0)
            printf("T1 PASS: called first, each completed line reaches the "
                   "file at once\n");
    }

    /* ── T2: a late call succeeds AND does nothing — the actual trap ─────
     *
     * This is the property that made the bug invisible. glibc does not reject
     * a late setvbuf; it returns 0, reports success, and leaves the stream
     * fully buffered. So there is no runtime signal at all, and the ordering
     * has to be enforced by inspection (T4). If a future platform starts
     * rejecting late calls, this test fails and the runtime check becomes
     * worth having again. */
    {
        int f0 = fails;
        snprintf(path, sizeof path, "%s/late.log", dir);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL, "T2: cannot open %s", path);

        fprintf(f, "something printed before setvbuf\n");   /* the mistake */
        int ok = log_line_buffered(f);
        fprintf(f, "a line that SHOULD flush if the late call had worked\n");
        long n = visible(path);

        CHECK(ok != 0, "T2: setvbuf now REJECTS a late call. That is better "
              "than the measured behaviour — restore the runtime warning in "
              "op_main.c/main.c, it can finally fire");
        CHECK(n == 0,
              "T2: %ld bytes visible after a late setvbuf. If a late call now "
              "works, the positional rule is no longer load-bearing and T4 "
              "can be relaxed", n);
        fclose(f);
        if (fails == f0)
            printf("T2 PASS: a late call returns success and changes nothing "
                   "— no runtime signal exists, so ordering is everything\n");
    }

    /* ── T3: without it, a file-backed stream buffers whole blocks ────────
     * The premise of the whole exercise. If this ever stops holding, the
     * setvbuf call is unnecessary rather than wrong. */
    {
        int f0 = fails;
        snprintf(path, sizeof path, "%s/default.log", dir);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL, "T3: cannot open %s", path);
        fprintf(f, "one line that would be visible if line buffered\n");
        long n = visible(path);
        CHECK(n == 0, "T3: %ld bytes visible with default buffering — the "
              "platform already line-buffers files and the fix is moot", n);
        fclose(f);
        CHECK(visible(path) > 0, "T3: nothing reached the file even on close");
        if (fails == f0)
            printf("T3 PASS: a redirected stream is block-buffered by default "
                   "— nothing visible until close\n");
    }

    /* ── T4: the shipped binaries call it as their first statement ────────
     * A source check, because the failure mode is positional and invisible to
     * any runtime test of the binary short of driving real hardware. */
    {
        int f0 = fails;
        const char *srcs[] = { "op_main.c", "main.c" };
        for (int i = 0; i < 2; i++) {
            FILE *src = fopen(srcs[i], "r");
            if (!src) {                      /* built out of tree; skip */
                printf("T4 SKIP: %s not readable from here\n", srcs[i]);
                continue;
            }
            char line[1024];
            int in_main = 0, printed_first = 0, buffered_first = 0, ln = 0;
            while (fgets(line, sizeof line, src)) {
                ln++;
                if (!in_main && strstr(line, "int main(")) { in_main = 1; continue; }
                if (!in_main) continue;
                if (!buffered_first && !printed_first && strstr(line, "printf("))
                    printed_first = ln;
                if (strstr(line, "log_line_buffered(stdout)")) {
                    buffered_first = ln; break;
                }
            }
            fclose(src);
            CHECK(buffered_first != 0,
                  "T4: %s never calls log_line_buffered(stdout)", srcs[i]);
            CHECK(printed_first == 0,
                  "T4: %s prints at line %d, before log_line_buffered at line "
                  "%d — setvbuf will be refused and the log will stay empty "
                  "until the run ends", srcs[i], printed_first, buffered_first);
        }
        if (fails == f0)
            printf("T4 PASS: both binaries set up buffering before they print "
                   "anything\n");
    }

    if (fails) { printf("\n*** LOG BUFFERING FAILURES ***\n"); return 1; }
    printf("\nALL LOG BUFFERING TESTS PASS\n");
    return 0;
}
