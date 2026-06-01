/* Validates trapcatch.c: install the catcher, then deliberately execute an
 * illegal instruction. The handler should write the fault file with the PC
 * of the 'illegal' opcode below. */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern void installTrapCatch(CONST_STRPTR);

int main(void)
{
    installTrapCatch((CONST_STRPTR)"SYS:AFtest/crash");
    Delay(25);
    asm volatile ("illegal");      /* 0x4AFC -> exception vector 4 */
    return 0;
}
