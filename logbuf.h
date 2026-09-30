#ifndef ECAT_LOGBUF_H
#define ECAT_LOGBUF_H
/* ── Keeping a redirected log current ───────────────────────────────────────
 *
 * A run redirected to a file (`./ecat_op ... > run.log`) gets a FULLY buffered
 * stdout, not a line-buffered one — the C library picks the mode from what the
 * stream is connected to, and a file is not a terminal. So nothing reaches the
 * file until 4 KB has accumulated or the process exits, and an operator
 * watching the log sees an empty file for minutes at a time. Worse, a run
 * killed rather than stopped loses everything it had printed: that is how the
 * 2026-09-29 two-slave run lost 1,486 s of work.
 *
 * The fix is one setvbuf call, and the trap is WHERE it goes. C11 §7.21.5.6:
 *
 *     The setvbuf function may be used only after the stream pointed to by
 *     stream has been associated with an open file and BEFORE ANY OTHER
 *     OPERATION (other than an unsuccessful call to setvbuf) is performed on
 *     the stream.
 *
 * Call it after the first printf and glibc does the worst possible thing: it
 * RETURNS SUCCESS and silently ignores the request. The stream stays fully
 * buffered, the call reads correctly, and nothing warns you. Measured on this
 * machine — a late call returns 0 and leaves the file at 0 bytes until exit,
 * while the same call made first flushes every line (t_logbuf T1, T2).
 *
 * So the return value CANNOT detect the mistake, and no runtime check can.
 * The only enforcement is positional: log_line_buffered(stdout) must be the
 * first statement of main(). t_logbuf T4 reads op_main.c and main.c and fails
 * if anything prints ahead of it.
 *
 * ecat_op had the call 31 printf calls into main() from 2026-09-30 until an
 * operator asked why the log file was empty while the run was going. It was
 * invisible from the inside: the call is there and it looks right. */
#include <stdio.h>

/* Make `f` line-buffered. Returns non-zero if the library accepted the
 * request — which it does even when called too late to mean anything, so do
 * not use this to check the ordering. See the note above. */
static inline int log_line_buffered(FILE *f)
{
    return setvbuf(f, NULL, _IOLBF, 0) == 0;
}

#endif /* ECAT_LOGBUF_H */
