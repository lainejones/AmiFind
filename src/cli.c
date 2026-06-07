/*
 * AmiFind - command line front end
 *
 *   AmiFind PATTERN [PATH]
 *
 *   PATTERN   text to look for. Plain text = case-insensitive substring.
 *             Contains AmigaDOS wildcards (#? * ? [] ~ |) = pattern match.
 *   PATH      volume / assign / directory to search (default SYS:).
 *
 * Press Ctrl-C to abort.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include "finder.h"

unsigned long __stack = 60000;   /* force a generous stack (deep recursion) */

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiFind 1.0 (07.06.2026)";

#define TEMPLATE "PATTERN/A,PATH"
enum { ARG_PATTERN, ARG_PATH, ARG_COUNT };

static BOOL cliPoll(APTR user)
{
    (void)user;
    /* read AND clear CTRL_C so the break is consumed (the shell / a later run
     * won't see a stale break flag) */
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? TRUE : FALSE;
}

static BOOL cliFound(CONST_STRPTR path, BOOL isDir, LONG size, APTR user)
{
    (void)user;
    if (isDir) Printf("%s  (dir)\n", (LONG)path);
    else       Printf("%s  (%ld bytes)\n", (LONG)path, size);
    return TRUE;
}

int main(void)
{
    struct RDArgs *rd;
    LONG args[ARG_COUNT] = { 0, 0 };
    int rc = 0;

    rd = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rd) {
        PrintFault(IoErr(), "AmiFind");
        return 20;
    }

    {
        CONST_STRPTR pat  = (CONST_STRPTR)args[ARG_PATTERN];
        CONST_STRPTR path = args[ARG_PATH] ? (CONST_STRPTR)args[ARG_PATH]
                                           : (CONST_STRPTR)"SYS:";
        struct Matcher    m;
        struct SearchStats st;
        LONG err;

        if (!matcherInit(&m, pat)) {
            Printf("AmiFind: bad search pattern\n");
            FreeArgs(rd);
            return 20;
        }

        err = searchTree(path, &m, cliFound, NULL, cliPoll, NULL, &st);
        if (err) {
            PrintFault(err, (STRPTR)path);
            rc = 20;
        } else {
            Printf("\n%ld match(es) - scanned %ld dirs, %ld files%s\n",
                   (LONG)st.matches, (LONG)st.dirsScanned,
                   (LONG)st.filesScanned, (LONG)(st.aborted ? " (aborted)" : ""));
            if (st.aborted) rc = RETURN_WARN;    /* 5: stopped via Ctrl-C */
        }
    }

    FreeArgs(rd);
    return rc;
}
