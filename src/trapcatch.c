/*
 * trapcatch.c - capture the PC of a CPU exception (guru) to a file.
 *
 * Installs a tc_TrapCode handler on the current task. When a processor
 * exception fires (illegal instruction, address error, ...), the handler
 * grabs the faulting PC + exception number from the supervisor stack
 * frame, then redirects execution (via RTE) to crashExit(), which writes
 * the details to a file and parks the task. No MMU required, so this works
 * on a plain 68EC020 A1200 as well as 68030+ boxes.
 *
 * Stack layout on entry to tc_TrapCode (supervisor mode):
 *     0(sp) : exception/trap number   (longword, pushed by exec)
 *     4(sp) : SR                       (word)   <-- start of CPU frame
 *     6(sp) : PC                       (longword)
 *    10(sp) : format/vector word       (68010+ only)
 * Removing the trap-number longword leaves a valid frame for RTE; the PC
 * sits at 2(sp) after that, identical on 68000 and 68020+ for group-1
 * exceptions such as illegal instruction.
 */

#include <exec/types.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

volatile ULONG g_crashPC   = 0;
volatile ULONG g_crashType = 0;
volatile ULONG g_ref       = 0;   /* runtime addr of installTrapCatch */
static   CONST_STRPTR g_logPath = (CONST_STRPTR)"PROGDIR:AmiFind.crash";

extern void trapStub(void);

/* writes the captured fault to a file, then parks forever (never returns) */
__attribute__((used, noinline))
static void crashExit(void)
{
    BPTR f = Open((STRPTR)g_logPath, MODE_NEWFILE);
    if (f) {
        char buf[40];
        int  i = 0, k;
        const char *hx = "0123456789abcdef";
        buf[i++]='T'; buf[i++]='Y'; buf[i++]='P'; buf[i++]='E'; buf[i++]='=';
        for (k = 0; k < 8; k++) buf[i++] = hx[(g_crashType >> ((7-k)*4)) & 0xF];
        buf[i++]=' ';
        buf[i++]='P'; buf[i++]='C'; buf[i++]='=';
        for (k = 0; k < 8; k++) buf[i++] = hx[(g_crashPC   >> ((7-k)*4)) & 0xF];
        buf[i++]=' ';
        buf[i++]='R'; buf[i++]='E'; buf[i++]='F'; buf[i++]='=';
        for (k = 0; k < 8; k++) buf[i++] = hx[(g_ref       >> ((7-k)*4)) & 0xF];
        buf[i++]='\n';
        Write(f, buf, i);
        Close(f);
    }
    for (;;) Wait(0L);          /* park: harmless hang, host reads the file */
}

/* the actual trap handler (entered in supervisor mode, ends with RTE) */
asm(
"   .globl _trapStub         \n"
"_trapStub:                   \n"
"   move.l (%sp),%d1          \n"   /* trap/exception number              */
"   lea    _g_crashType,%a1   \n"
"   move.l %d1,(%a1)          \n"
"   addq.l #4,%sp             \n"   /* drop trap number -> sp at SR/PC    */
"   move.l 2(%sp),%d0         \n"   /* faulting PC                        */
"   lea    _g_crashPC,%a1     \n"
"   move.l %d0,(%a1)          \n"
"   lea    _crashExit,%a0     \n"
"   move.l %a0,2(%sp)         \n"   /* resume at crashExit after RTE      */
"   rte                       \n"
);

void installTrapCatch(CONST_STRPTR logPath)
{
    struct Task *me = FindTask(NULL);
    if (logPath) g_logPath = logPath;
    g_ref = (ULONG)(APTR)installTrapCatch;   /* reference for PC->offset */
    me->tc_TrapCode = (APTR)trapStub;
}
