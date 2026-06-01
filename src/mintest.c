/*
 * AmiFindMin - minimal GadTools window, for isolating the AmiFindGUI crash.
 *
 * Opens a window with a listview and a Quit button, adds three dummy
 * entries, and runs a normal event loop. No file searching, no callbacks,
 * no message pumping. If THIS runs cleanly but AmiFindGUI crashes, the
 * fault is in AmiFind's own logic; if this also crashes, the fault is in
 * the GadTools/Intuition path or the environment/toolchain.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include <string.h>

unsigned long __stack = 60000;          /* force a generous stack */

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;

#define GAD_QUIT 1
#define GAD_LIST 2

int main(void)
{
    struct Screen *scr;
    APTR vi;
    struct Window *win;
    struct Gadget *glist = NULL, *gad;
    struct NewGadget ng;
    struct List labels;
    struct Node n1, n2, n3;
    LONG top;
    BOOL done = FALSE;

    /* build a tiny static label list */
    labels.lh_Head     = (struct Node *)&labels.lh_Tail;
    labels.lh_Tail     = NULL;
    labels.lh_TailPred = (struct Node *)&labels.lh_Head;
    labels.lh_Type     = NT_UNKNOWN;
    memset(&n1, 0, sizeof n1); n1.ln_Name = (char *)"first entry";
    memset(&n2, 0, sizeof n2); n2.ln_Name = (char *)"second entry";
    memset(&n3, 0, sizeof n3); n3.ln_Name = (char *)"third entry";
    AddTail(&labels, &n1); AddTail(&labels, &n2); AddTail(&labels, &n3);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) goto cleanup;

    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;
    vi = GetVisualInfo(scr, TAG_END);
    if (!vi) { UnlockPubScreen(NULL, scr); goto cleanup; }

    top = scr->WBorTop + (scr->Font ? scr->Font->ta_YSize : 8) + 2;

    gad = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = scr->Font;

    ng.ng_LeftEdge = 10; ng.ng_TopEdge = top + 4;
    ng.ng_Width = 300; ng.ng_Height = 120;
    ng.ng_GadgetID = GAD_LIST;
    gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
                       GTLV_Labels, (ULONG)&labels,
                       GTLV_ReadOnly, TRUE, TAG_END);

    ng.ng_TopEdge += 128; ng.ng_Width = 80; ng.ng_Height = 16;
    ng.ng_GadgetText = (UBYTE *)"Quit";
    ng.ng_Flags = PLACETEXT_IN;
    ng.ng_GadgetID = GAD_QUIT;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    if (gad) {
        win = OpenWindowTags(NULL,
            WA_Title,      (ULONG)"AmiFindMin",
            WA_Left, 40, WA_Top, 30,
            WA_InnerWidth, 320, WA_InnerHeight, top + 160,
            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | LISTVIEWIDCMP,
            WA_PubScreen, (ULONG)scr,
            WA_Gadgets, (ULONG)glist,
            TAG_END);
        if (win) {
            GT_RefreshWindow(win, NULL);
            while (!done) {
                struct IntuiMessage *imsg;
                WaitPort(win->UserPort);
                while ((imsg = GT_GetIMsg(win->UserPort))) {
                    ULONG cls = imsg->Class;
                    struct Gadget *g = (struct Gadget *)imsg->IAddress;
                    GT_ReplyIMsg(imsg);
                    if (cls == IDCMP_CLOSEWINDOW) done = TRUE;
                    else if (cls == IDCMP_GADGETUP && g->GadgetID == GAD_QUIT) done = TRUE;
                    else if (cls == IDCMP_REFRESHWINDOW) {
                        GT_BeginRefresh(win); GT_EndRefresh(win, TRUE);
                    }
                }
            }
            CloseWindow(win);
        }
    }
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);

cleanup:
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
