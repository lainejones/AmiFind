/*
 * AmiFind - GadTools GUI front end (AmigaOS 3.x, no external deps)
 *
 *  +------------------------------------------------+
 *  |  Pattern   [____________________]              |
 *  |  Search in [SYS:________________]              |
 *  |  [ Search ] [ Stop ]   Ready                   |
 *  |  +------------------------------------------+  |
 *  |  | DH0:c/List                               |  |
 *  |  | DH0:s/Startup-Sequence                   |  |
 *  |  | ...                                      |  |
 *  |  +------------------------------------------+  |
 *  +------------------------------------------------+
 *
 * The search runs synchronously when Search is pressed; while it runs the
 * poll callback pumps Intuition messages so the Stop button and close
 * gadget stay live. Results are collected into an Exec list and attached
 * to the listview when the scan finishes.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/wb.h>
#include <proto/icon.h>

#include <string.h>

#include "finder.h"

unsigned long __stack = 60000;   /* force a generous stack (deep recursion) */

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiFindGUI 1.0 (07.06.2026)";

/* --- temporary boot-trace: writes progress markers to SYS:AFtest/trace --- */
static BPTR g_tr = 0;
static void TR(CONST_STRPTR s)
{
#ifdef AFTRACE
    if (g_tr) { Write(g_tr, (APTR)s, (LONG)strlen((const char *)s));
                Write(g_tr, (APTR)"\n", 1); Flush(g_tr); }
#else
    (void)s;
#endif
}

/* library bases (exec & dos are auto-opened by the C startup) */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *AslBase       = NULL;   /* optional: file requester */
struct Library       *WorkbenchBase = NULL;   /* optional: AppIcon/iconify */
struct Library       *IconBase      = NULL;   /* optional: AppIcon icon    */

enum {
    GAD_PATTERN = 1,
    GAD_PATH,
    GAD_PICK,
    GAD_SEARCH,
    GAD_STOP,
    GAD_STATUS,
    GAD_LIST
};

struct Gui {
    struct Screen *scr;
    APTR           vi;
    struct Window *win;
    struct Gadget *glist;          /* gadtools gadget list (for FreeGadgets) */
    struct Gadget *gPattern;
    struct Gadget *gPath;
    struct Gadget *gPick;
    struct Gadget *gSearch;
    struct Gadget *gStop;
    struct Gadget *gStatus;
    struct Gadget *gList;

    struct List    results;        /* nodes shown in the listview            */
    char           status[80];
    char           savePat[210];   /* Find text, preserved across resize/icon */
    char           savePath[210];  /* In text,   preserved across resize/icon */
    BOOL           stopReq;        /* Stop pressed during a search           */
    BOOL           closeReq;       /* window closed during a search          */
    BOOL           resizePending;  /* NEWSIZE arrived mid-search -> relayout after */
    BOOL           outOfMem;       /* a result was dropped (AllocVec failed)  */
    ULONG          uiCounter;

    struct MsgPort   *appPort;     /* AppIcon message port (iconified)        */
    struct AppIcon   *appIcon;     /* non-NULL while iconified                */
    struct DiskObject *dobj;       /* icon used for the AppIcon               */
    BOOL              iconified;

    struct Menu      *menu;        /* Project menu (Iconify / Quit)           */
};

#define MENU_ICONIFY 1
#define MENU_QUIT    2

static void relayout(struct Gui *g);   /* defined below; used by doSearch */

/* ------------------------------------------------------------------ */
/* result list management                                             */
/* ------------------------------------------------------------------ */

static void freeResults(struct Gui *g)
{
    struct Node *n;
    while ((n = RemHead(&g->results)))
        FreeVec(n);                /* node + name were one allocation        */
}

/* one allocation holds struct Node followed by the text */
static void addResult(struct Gui *g, CONST_STRPTR text)
{
    int len = (int)strlen((const char *)text) + 1;
    struct Node *n = (struct Node *)AllocVec(sizeof(struct Node) + len,
                                             MEMF_CLEAR);
    if (!n) { g->outOfMem = TRUE; return; }   /* note the drop for the status */
    n->ln_Name = (char *)(n + 1);
    CopyMem((APTR)text, n->ln_Name, len);
    AddTail(&g->results, n);
}

static void setStatus(struct Gui *g, CONST_STRPTR text)
{
    strncpy(g->status, (const char *)text, sizeof g->status - 1);
    g->status[sizeof g->status - 1] = '\0';
    if (g->win && g->gStatus)        /* both must be live (iconified -> neither) */
        GT_SetGadgetAttrs(g->gStatus, g->win, NULL,
                          GTTX_Text, (ULONG)g->status, TAG_END);
}

/* ------------------------------------------------------------------ */
/* search callbacks                                                   */
/* ------------------------------------------------------------------ */

static BOOL guiFound(CONST_STRPTR path, BOOL isDir, LONG size, APTR user)
{
    struct Gui *g = (struct Gui *)user;
    char line[300];
    if (isDir) {
        strncpy(line, (const char *)path, sizeof line - 8);
        line[sizeof line - 8] = '\0';
        strcat(line, "  (dir)");
    } else {
        (void)size;
        strncpy(line, (const char *)path, sizeof line - 1);
        line[sizeof line - 1] = '\0';
    }
    addResult(g, line);
    return TRUE;
}

/* process pending Intuition messages without blocking; returns TRUE to abort */
static BOOL pumpMessages(struct Gui *g)
{
    struct IntuiMessage *imsg;
    while ((imsg = GT_GetIMsg(g->win->UserPort))) {
        ULONG    cls = imsg->Class;
        struct Gadget *gad = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);

        switch (cls) {
        case IDCMP_CLOSEWINDOW:
            g->closeReq = TRUE;
            g->stopReq  = TRUE;
            break;
        case IDCMP_GADGETUP:
            if (gad->GadgetID == GAD_STOP)
                g->stopReq = TRUE;
            break;
        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(g->win);
            GT_EndRefresh(g->win, TRUE);
            break;
        case IDCMP_NEWSIZE:
            /* can't relayout mid-search (it frees the gadgets we're driving);
             * remember it and rebuild once the scan returns. */
            g->resizePending = TRUE;
            break;
        default:
            break;
        }
    }
    return g->stopReq;
}

static BOOL guiPoll(APTR user)
{
    struct Gui *g = (struct Gui *)user;
    BOOL abort = pumpMessages(g);
    /* refresh the status roughly every ~1k entries so the user sees motion */
    if (((++g->uiCounter) & 1023) == 0) {
        char buf[80];
        /* tiny manual itoa-free status: reuse dos for formatting via sprintf
         * is unavailable in -noixemul; keep it simple. */
        strcpy(buf, "Searching...");
        setStatus(g, buf);
    }
    return abort;
}

/* ------------------------------------------------------------------ */
/* run a search from the current gadget contents                      */
/* ------------------------------------------------------------------ */

static CONST_STRPTR gadgetString(struct Gadget *g)
{
    struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
    return (CONST_STRPTR)si->Buffer;
}

/* pop up an ASL drawer requester and put the chosen path in the In field */
static void doPickPath(struct Gui *g)
{
    struct FileRequester *fr;
    if (!AslBase) return;                 /* asl.library not available */

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select a volume, assign or drawer to search",
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_InitialDrawer, (ULONG)gadgetString(g->gPath),
            ASLFR_InitialWidth,  360,
            ASLFR_InitialHeight, 280,
            TAG_END);
    if (!fr) return;

    if (AslRequestTags(fr, ASLFR_Window, (ULONG)g->win, TAG_END)) {
        char buf[256];
        strncpy(buf, (const char *)fr->fr_Drawer, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        GT_SetGadgetAttrs(g->gPath, g->win, NULL, GTST_String, (ULONG)buf, TAG_END);
    }
    FreeAslRequest(fr);
}

static void doSearch(struct Gui *g)
{
    struct Matcher     m;
    struct SearchStats st;
    CONST_STRPTR pat  = gadgetString(g->gPattern);
    CONST_STRPTR path = gadgetString(g->gPath);
    LONG err;

    if (!path || path[0] == '\0') { setStatus(g, "Enter a path to search."); return; }
    if (!matcherInit(&m, pat))    { setStatus(g, "Bad search pattern.");     return; }

    /* detach old list, free it */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)~0, TAG_END);
    freeResults(g);

    g->stopReq = FALSE;
    g->outOfMem = FALSE;
    g->uiCounter = 0;
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, TRUE, TAG_END);
    setStatus(g, "Searching...");

    err = searchTree(path, &m, guiFound, g, guiPoll, g, &st);

    /* attach collected results */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->results, TAG_END);
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, FALSE, TAG_END);

    if (err) {
        setStatus(g, "Cannot open that path.");
    } else {
        /* build "<n> matches" without sprintf (no ixemul) */
        char buf[80];
        char num[16];
        int  i = 0, j;
        ULONG v = st.matches;
        if (v == 0) num[i++] = '0';
        while (v) { num[i++] = (char)('0' + (v % 10)); v /= 10; }
        strcpy(buf, "");
        for (j = i - 1; j >= 0; j--) { int l = (int)strlen(buf); buf[l] = num[j]; buf[l+1] = 0; }
        strcat(buf, st.aborted ? " match(es) - stopped" : " match(es)");
        if (g->outOfMem) strcat(buf, " (truncated: low mem)");
        setStatus(g, buf);
    }

    /* a resize that arrived mid-search was deferred; apply it now */
    if (g->resizePending) { g->resizePending = FALSE; relayout(g); }
}

/* ------------------------------------------------------------------ */
/* gadget / window construction                                       */
/* ------------------------------------------------------------------ */

/* (Re)create all gadgets sized to the live window. Coordinates are relative
 * to the window's own borders so it works at any window size. Field contents
 * (pattern/path) are passed in so they survive a resize-rebuild. */
static BOOL makeGadgets(struct Gui *g)
{
    struct NewGadget ng;
    struct Gadget *gad;
    struct Window *w = g->win;
    WORD fh  = g->scr->Font ? g->scr->Font->ta_YSize : 8;
    WORD cl  = w->BorderLeft + 2;            /* content left  (window coords) */
    WORD cr  = w->Width  - w->BorderRight - 2;
    WORD cb  = w->Height - w->BorderBottom - 1;
    WORD row = fh + 6;
    WORD gh  = fh + 3;
    WORD lab = 46;                           /* label column - keeps "Find" on-window */
    WORD y;

    gad = CreateContext(&g->glist);
    if (!gad) return FALSE;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = g->vi;
    ng.ng_TextAttr   = g->scr->Font;

    y = w->BorderTop + 2;

    /* Find string */
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab); ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Find"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_PATTERN;
    gad = g->gPattern = CreateGadget(STRING_KIND, gad, &ng,
                                     GTST_MaxChars, 200,
                                     GTST_String, (ULONG)g->savePat, TAG_END);
    /* In string (leave room for the picker button) */
    y += row;
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab) - 44;
    ng.ng_GadgetText = (UBYTE *)"In"; ng.ng_GadgetID = GAD_PATH;
    gad = g->gPath = CreateGadget(STRING_KIND, gad, &ng,
                                  GTST_MaxChars, 200,
                                  GTST_String, (ULONG)g->savePath, TAG_END);
    /* picker "..." */
    ng.ng_LeftEdge = cr - 40; ng.ng_Width = 40;
    ng.ng_GadgetText = (UBYTE *)"..."; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_GadgetID = GAD_PICK;
    gad = g->gPick = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* Search / Stop / Iconify + status */
    y += row;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y; ng.ng_Width = 72; ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Search"; ng.ng_GadgetID = GAD_SEARCH;
    gad = g->gSearch = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 78;
    ng.ng_GadgetText = (UBYTE *)"Stop"; ng.ng_GadgetID = GAD_STOP;
    gad = g->gStop = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    /* status text fills the rest of the row (Iconify is a title-bar gadget) */
    ng.ng_LeftEdge = cl + 158; ng.ng_Width = cr - (cl + 158);
    if (ng.ng_Width < 8) ng.ng_Width = 8;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_STATUS;
    gad = g->gStatus = CreateGadget(TEXT_KIND, gad, &ng,
                                    GTTX_Text, (ULONG)g->status,
                                    GTTX_Border, FALSE, TAG_END);
    /* Listview fills the rest */
    y += row + 2;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y;
    ng.ng_Width = cr - cl; ng.ng_Height = cb - y;
    if (ng.ng_Width  < 8) ng.ng_Width  = 8;
    if (ng.ng_Height < 8) ng.ng_Height = 8;   /* tall fonts can drive this <=0 */
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GAD_LIST;
    gad = g->gList = CreateGadget(LISTVIEW_KIND, gad, &ng,
                                  GTLV_Labels, (ULONG)&g->results,
                                  GTLV_ReadOnly, TRUE, TAG_END);
    return (gad != NULL);
}

/* rebuild the gadgets for the current window size (initial + on resize) */
/* save the current Find/In text into the persistent buffers */
static void saveFields(struct Gui *g)
{
    if (!g->glist) return;
    strncpy(g->savePat, (const char *)gadgetString(g->gPattern), sizeof g->savePat - 1);
    g->savePat[sizeof g->savePat - 1] = '\0';
    strncpy(g->savePath, (const char *)gadgetString(g->gPath), sizeof g->savePath - 1);
    g->savePath[sizeof g->savePath - 1] = '\0';
}

static void relayout(struct Gui *g)
{
    if (g->glist) {                          /* preserve typed-in text */
        saveFields(g);
        RemoveGList(g->win, g->glist, -1);
        FreeGadgets(g->glist);
        g->glist = NULL;
        EraseRect(g->win->RPort, g->win->BorderLeft, g->win->BorderTop,
                  g->win->Width - g->win->BorderRight - 1,
                  g->win->Height - g->win->BorderBottom - 1);
    }

    if (makeGadgets(g)) {
        AddGList(g->win, g->glist, ~0, -1, NULL);
        RefreshGList(g->glist, g->win, NULL, -1);
        GT_RefreshWindow(g->win, NULL);
    } else {
        /* low memory: makeGadgets failed mid-chain. Free the partial,
         * never-added list and clear every gadget pointer so nothing
         * (saveFields, doSearch, setStatus, the next relayout) touches
         * freed gadgets. */
        if (g->glist) { FreeGadgets(g->glist); g->glist = NULL; }
        g->gPattern = g->gPath = g->gPick = g->gSearch =
        g->gStop    = g->gStatus = g->gList = NULL;
    }
}

/* Project menu - iconify/quit via the right mouse button */
static struct NewMenu amiNewMenu[] = {
    { NM_TITLE, (STRPTR)"Project",      NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Iconify",      (STRPTR)"I", 0, 0, (APTR)MENU_ICONIFY },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,    NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",         (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL,                   NULL,        0, 0, NULL }
};

static void setupMenu(struct Gui *g)
{
    if (!GadToolsBase) return;
    g->menu = CreateMenus(amiNewMenu, TAG_END);
    if (g->menu) {
        if (LayoutMenus(g->menu, g->vi, TAG_END))
            SetMenuStrip(g->win, g->menu);
        else { FreeMenus(g->menu); g->menu = NULL; }
    }
}

static BOOL buildWindow(struct Gui *g)
{
    struct Screen *sc;
    WORD wtop, winw, winh, maxw, maxh, left;

    g->scr = LockPubScreen(NULL);
    if (!g->scr) return FALSE;
    g->vi = GetVisualInfo(g->scr, TAG_END);
    if (!g->vi) return FALSE;
    sc = g->scr;
    strcpy(g->status, "Ready");

    /* a modest default window, clamped to the screen; user can resize it */
    maxw = sc->Width;
    maxh = sc->Height;
    winw = 320; if (winw > maxw) winw = maxw;
    winh = 200; if (winh > maxh - (sc->BarHeight + 2)) winh = maxh - (sc->BarHeight + 2);
    wtop = sc->BarHeight + 2;
    left = (maxw - winw) / 2; if (left < 0) left = 0;

    TR("buildWindow: locked screen + vi");
    g->win = OpenWindowTags(NULL,
        WA_Title,     (ULONG)"AmiFind",
        WA_Left,      left, WA_Top, wtop,
        WA_Width,     winw, WA_Height, winh,
        WA_MinWidth,  260,  WA_MinHeight, 110,
        WA_MaxWidth,  maxw, WA_MaxHeight, maxh,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                      WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE |
                      IDCMP_MENUPICK | BUTTONIDCMP | STRINGIDCMP | LISTVIEWIDCMP,
        WA_PubScreen, (ULONG)g->scr,
        TAG_END);
    TR("window opened");
    if (!g->win) return FALSE;

    relayout(g);                             /* build + attach the gadgets   */
    setupMenu(g);                            /* Project menu (right-click iconify) */
    TR("refreshed - entering event loop");
    return TRUE;
}

/* close just the window + its display resources (results are preserved) */
static void closeWin(struct Gui *g)
{
    if (g->win && g->menu)  { ClearMenuStrip(g->win); }
    if (g->menu)            { FreeMenus(g->menu); g->menu = NULL; }
    if (g->win)   { CloseWindow(g->win);          g->win = NULL; }
    if (g->glist) { FreeGadgets(g->glist);        g->glist = NULL; }
    /* the individual gadget pointers point into the freed list */
    g->gPattern = g->gPath = g->gPick = g->gSearch =
    g->gStop    = g->gStatus = g->gList = NULL;
    if (g->vi)    { FreeVisualInfo(g->vi);         g->vi = NULL; }
    if (g->scr)   { UnlockPubScreen(NULL, g->scr); g->scr = NULL; }
}

/* hide the window to a Workbench AppIcon */
static void doIconify(struct Gui *g)
{
    if (!WorkbenchBase || !IconBase) return;     /* not available */

    saveFields(g);
    if (!g->appPort) g->appPort = CreateMsgPort();
    if (!g->appPort) return;
    if (!g->dobj) {
        g->dobj = (struct DiskObject *)GetDiskObject((STRPTR)"PROGDIR:AmiFindGUI");
        if (!g->dobj) g->dobj = (struct DiskObject *)GetDiskObject((STRPTR)"ENV:Sys/def_Tool");
        if (!g->dobj) { setStatus(g, "No icon available to iconify."); return; }
    }
    closeWin(g);
    g->appIcon = AddAppIconA(0L, 0L, (STRPTR)"AmiFind", g->appPort,
                             (BPTR)0, g->dobj, NULL);
    if (!g->appIcon) {                            /* failed: reopen */
        if (!buildWindow(g))
            g->closeReq = TRUE;  /* no window AND no AppIcon: exit cleanly
                                  * instead of Wait()ing forever on a port
                                  * nothing will ever signal */
        return;
    }
    g->iconified = TRUE;
}

/* bring the window back from the AppIcon */
static void doUniconify(struct Gui *g)
{
    struct Message *m;
    if (g->appIcon) { RemoveAppIcon(g->appIcon); g->appIcon = NULL; }
    if (g->appPort) while ((m = GetMsg(g->appPort))) ReplyMsg(m);
    g->iconified = FALSE;
    if (!buildWindow(g))                         /* restores savePat/savePath */
        g->closeReq = TRUE;      /* window gone, AppIcon already removed:
                                  * exit cleanly rather than zombie-Wait */
}

/* ------------------------------------------------------------------ */

static void eventLoop(struct Gui *g)
{
    BOOL done = FALSE;
    while (!done) {
        ULONG winSig = (g->win && g->win->UserPort)
                     ? (1UL << g->win->UserPort->mp_SigBit) : 0;
        ULONG appSig = g->appPort ? (1UL << g->appPort->mp_SigBit) : 0;
        ULONG got;

        if (!winSig && !appSig) break;        /* nothing to wait on */
        got = Wait(winSig | appSig);

        /* ---- window events ---- */
        if (winSig && (got & winSig)) {
            struct IntuiMessage *imsg;
            while (g->win && (imsg = GT_GetIMsg(g->win->UserPort))) {
                ULONG    cls  = imsg->Class;
                UWORD    code = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    done = TRUE;
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GAD_SEARCH ||
                        gad->GadgetID == GAD_PATTERN) {  /* Enter in Find = Search */
                        doSearch(g);
                        if (g->closeReq) done = TRUE;
                    } else if (gad->GadgetID == GAD_PICK) {
                        doPickPath(g);
                    }
                    break;
                case IDCMP_MENUPICK: {
                    UWORD mn = code;
                    while (mn != MENUNULL && g->win && g->menu) {
                        struct MenuItem *it = ItemAddress(g->menu, mn);
                        ULONG ud;
                        if (!it) break;
                        ud = (ULONG)GTMENUITEM_USERDATA(it);
                        if (ud == MENU_ICONIFY)   doIconify(g);
                        else if (ud == MENU_QUIT) done = TRUE;
                        if (g->closeReq)          done = TRUE;
                        if (g->iconified || done) break;
                        mn = it->NextSelect;
                    }
                    break;
                }
                case IDCMP_NEWSIZE:
                    /* defer: relaying out here frees gadgets that later-queued
                     * messages in this same drain still reference (stale gad).
                     * Rebuild once the queue is fully drained, below. */
                    g->resizePending = TRUE;
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(g->win);
                    GT_EndRefresh(g->win, TRUE);
                    break;
                default:
                    break;
                }
                if (g->iconified || done) break;  /* window gone / quitting */
            }
            /* apply a deferred resize now that the queue is drained and every
             * message referencing the old gadgets has been handled */
            if (g->win && !done && !g->iconified && g->resizePending) {
                g->resizePending = FALSE;
                relayout(g);
            }
        }

        /* ---- AppIcon clicked: restore the window ---- */
        if (g->appPort && (got & appSig)) {
            struct Message *m;
            BOOL wake = FALSE;
            while ((m = GetMsg(g->appPort))) { ReplyMsg(m); wake = TRUE; }
            if (wake && g->iconified) {
                doUniconify(g);
                if (g->closeReq) done = TRUE;    /* restore failed */
            }
        }
    }
}

#ifdef AFSTRESS
/* Stress harness: build + briefly run + tear down the window many times in
 * one boot, to flush out a rare startup race. Trap-catcher logs any fault. */
extern void installTrapCatch(CONST_STRPTR);
static void initList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type = NT_UNKNOWN;
}
int main(void)
{
    int n;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    installTrapCatch((CONST_STRPTR)"SYS:AFtest/crash");
    if (IntuitionBase && GfxBase && GadToolsBase) {
        for (n = 0; n < 400; n++) {
            struct Gui g;
            int t;
            memset(&g, 0, sizeof g);
            initList(&g.results);
            if (buildWindow(&g)) {
                for (t = 0; t < 6; t++) {
                    struct IntuiMessage *im;
                    while ((im = GT_GetIMsg(g.win->UserPort))) {
                        ULONG cls = im->Class;
                        GT_ReplyIMsg(im);
                        if (cls == IDCMP_REFRESHWINDOW) {
                            GT_BeginRefresh(g.win); GT_EndRefresh(g.win, TRUE);
                        }
                    }
                    Delay(1);
                }
                closeWin(&g);
            }
        }
        { BPTR f = Open((STRPTR)"SYS:AFtest/survived", MODE_NEWFILE);
          if (f) { Write(f, (APTR)"survived 400 cycles\n", 20); Close(f); } }
    }
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
#else
int main(void)
{
    struct Gui g;
    int rc = 20;

    memset(&g, 0, sizeof g);
#ifdef AFCATCH
    { extern void installTrapCatch(CONST_STRPTR);
      installTrapCatch(NULL); }   /* logs to PROGDIR:AmiFind.crash */
#endif
    /* init the result list without pulling in amiga.lib's NewList() */
    g.results.lh_Head     = (struct Node *)&g.results.lh_Tail;
    g.results.lh_Tail     = NULL;
    g.results.lh_TailPred = (struct Node *)&g.results.lh_Head;
    g.results.lh_Type     = NT_UNKNOWN;
    strcpy(g.savePath, "SYS:");        /* default In; savePat stays empty */

#ifdef AFTRACE
    g_tr = Open((STRPTR)"SYS:AFtest/trace", MODE_NEWFILE);
#endif
    TR("main: start");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    AslBase       = OpenLibrary("asl.library", 38);          /* optional */
    WorkbenchBase = OpenLibrary("workbench.library", 37);    /* optional (iconify) */
    IconBase      = OpenLibrary("icon.library", 37);         /* optional (iconify) */

    TR("libraries opened");
    if (IntuitionBase && GfxBase && GadToolsBase && buildWindow(&g)) {
        TR("buildWindow ok");
        eventLoop(&g);
        rc = 0;
    }
    TR("done");
    if (g_tr) Close(g_tr);

    closeWin(&g);
    if (g.appIcon) RemoveAppIcon(g.appIcon);
    if (g.appPort) { struct Message *m; while ((m = GetMsg(g.appPort))) ReplyMsg(m);
                     DeleteMsgPort(g.appPort); }
    if (g.dobj)    FreeDiskObject(g.dobj);
    freeResults(&g);
    if (IconBase)      CloseLibrary(IconBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
#endif /* AFSTRESS */
