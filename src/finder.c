/*
 * AmiFind - shared file search engine (see finder.h)
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include "finder.h"

/* ------------------------------------------------------------------ */
/* small case-insensitive helpers                                     */
/* ------------------------------------------------------------------ */

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return c;
}

static void str_lower(char *dst, CONST_STRPTR src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = to_lower((char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

static BOOL substr_ci(CONST_STRPTR haystackRaw, CONST_STRPTR needleLower)
{
    char hay[140];
    int nl, hl, i;

    str_lower(hay, haystackRaw, (int)sizeof hay);
    nl = (int)strlen(needleLower);
    if (nl == 0) return TRUE;               /* empty pattern matches all  */
    hl = (int)strlen(hay);
    for (i = 0; i + nl <= hl; i++) {
        if (strncmp(hay + i, needleLower, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* matcher                                                            */
/* ------------------------------------------------------------------ */

BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern)
{
    LONG r = ParsePatternNoCase((CONST_STRPTR)pattern,
                                (STRPTR)m->parsed, (LONG)sizeof m->parsed);
    if (r < 0) return FALSE;                /* pattern too long / bad     */
    m->useWild = (r == 1);                  /* 1 -> wildcards present     */
    str_lower(m->lower, pattern, (int)sizeof m->lower);
    return TRUE;
}

BOOL matcherMatch(struct Matcher *m, CONST_STRPTR name)
{
    if (m->useWild)
        return MatchPatternNoCase((STRPTR)m->parsed, (STRPTR)name) ? TRUE : FALSE;
    return substr_ci(name, (CONST_STRPTR)m->lower);
}

/* ------------------------------------------------------------------ */
/* recursive walk                                                     */
/* ------------------------------------------------------------------ */

struct Ctx {
    struct Matcher    *m;
    FoundFunc          found;
    APTR               foundUser;
    PollFunc           poll;
    APTR               pollUser;
    struct SearchStats *st;
    char               path[520];           /* current path, grown/shrunk */
    BOOL               stop;
    ULONG              pollCounter;
};

static void recurse(struct Ctx *c, BPTR lock)
{
    struct FileInfoBlock *fib;
    LONG plen;

    if (c->stop) return;

    /* A FileInfoBlock must be long-word aligned; AllocDosObject guarantees
     * that and keeps it off our (limited) stack on every recursion level. */
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;

    if (Examine(lock, fib)) {
        if (fib->fib_DirEntryType > 0) {
            /* directory -> enumerate its children */
            while (!c->stop && ExNext(lock, fib)) {

                if (c->poll && ((++c->pollCounter & 63) == 0)) {
                    if (c->poll(c->pollUser)) { c->stop = TRUE; break; }
                }

                plen = (LONG)strlen(c->path);
                if (!AddPart((STRPTR)c->path, fib->fib_FileName, (ULONG)sizeof c->path)) {
                    c->path[plen] = '\0';    /* AddPart may have mutated the
                                              * buffer before failing; restore
                                              * or the rest of this dir scans
                                              * against a corrupted base path */
                    continue;                /* full path would overflow   */
                }

                if (fib->fib_DirEntryType > 0) {
                    LONG det = fib->fib_DirEntryType;
                    c->st->dirsScanned++;
                    if (matcherMatch(c->m, fib->fib_FileName)) {
                        c->st->matches++;
                        if (!c->found(c->path, TRUE, fib->fib_Size, c->foundUser))
                            c->stop = TRUE;
                    }
                    /* Report links but never recurse into them: a soft link or
                     * link-dir can point outside the root (re-scanning a whole
                     * volume, duplicate hits) or back at an ancestor (loops
                     * until AddPart caps the path). Only descend real dirs. */
                    if (!c->stop && det != ST_SOFTLINK && det != ST_LINKDIR) {
                        BPTR child = Lock((STRPTR)c->path, ACCESS_READ);
                        if (child) {
                            recurse(c, child);
                            UnLock(child);
                        }
                    }
                } else {
                    c->st->filesScanned++;
                    if (matcherMatch(c->m, fib->fib_FileName)) {
                        c->st->matches++;
                        if (!c->found(c->path, FALSE, fib->fib_Size, c->foundUser))
                            c->stop = TRUE;
                    }
                }

                c->path[plen] = '\0';        /* restore parent path        */
            }
        } else {
            /* the root itself is a plain file */
            c->st->filesScanned++;
            if (matcherMatch(c->m, fib->fib_FileName)) {
                c->st->matches++;
                if (!c->found(c->path, FALSE, fib->fib_Size, c->foundUser))
                    c->stop = TRUE;
            }
        }
    }

    FreeDosObject(DOS_FIB, fib);
}

LONG searchTree(CONST_STRPTR rootPath, struct Matcher *m,
                FoundFunc found, APTR foundUser,
                PollFunc  poll,  APTR pollUser,
                struct SearchStats *stats)
{
    struct Ctx c;
    BPTR lock;

    memset(&c, 0, sizeof c);
    c.m = m;
    c.found = found;   c.foundUser = foundUser;
    c.poll  = poll;    c.pollUser  = pollUser;
    c.st = stats;

    stats->matches = stats->dirsScanned = stats->filesScanned = 0;
    stats->aborted = FALSE;

    strncpy(c.path, (const char *)rootPath, sizeof c.path - 1);
    c.path[sizeof c.path - 1] = '\0';

    lock = Lock((STRPTR)rootPath, ACCESS_READ);
    if (!lock) {
        LONG e = IoErr();            /* never return 0 (= success) on a failed
                                      * Lock - a handler that forgets to set
                                      * IoErr would otherwise look like 0 hits */
        return e ? e : ERROR_OBJECT_NOT_FOUND;
    }

    recurse(&c, lock);
    UnLock(lock);

    stats->aborted = c.stop;
    return 0;
}
