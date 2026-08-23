/*
 * Host services for the real AROS BOOPSI sources.
 *
 * ACE compiles rom/intuition/{rootclass,makeclass,freeclass,addclass,
 * removeclass,findclass,newobjecta,disposeobject,setattrsa,getattr,
 * nextobject}.c and compiler/alib/{domethod,dosupermethod,coercemethod}.c
 * unchanged.  Those sources call Exec for memory, pools, semaphores and list
 * handling, and expect a library base holding the class list.  This file is
 * that seam and nothing more: no part of the class, object or dispatch
 * behaviour lives here.
 */

#include "aros_boopsi_runtime.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>

/* rootclass.c defines this through AROS_UFH3; see compat aros/asmcall.h. */
extern IPTR rootDispatcher(Class *cl, Object *o, Msg msg);

/*
 * struct IntIntuitionBase and the extern declaration of IntuitionBase come
 * from the forced include, ace_boopsi_intern.h, so the AROS sources and this
 * one cannot disagree about the layout.  This is the single definition.
 */
struct IntIntuitionBase *IntuitionBase;

/* -------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------- */

/*
 * AllocMem()/FreeMem() carry the allocation size at the call site the way
 * AmigaOS does, so the host allocator only has to honour MEMF_CLEAR.
 */
APTR AllocMem(ULONG byteSize, ULONG requirements)
{
    void *memory;

    if (byteSize == 0)
        return NULL;
    memory = malloc(byteSize);
    if (memory && (requirements & MEMF_CLEAR))
        memset(memory, 0, byteSize);
    return memory;
}

void FreeMem(APTR memoryBlock, ULONG byteSize)
{
    (void)byteSize;
    free(memoryBlock);
}

/*
 * AllocVec()/FreeVec() record their own size, which on AmigaOS means a hidden
 * header ahead of the block.  malloc() already tracks it, so the host forms
 * are the same allocation without the header.  GetMsgFromStack() in
 * compiler/alib/alib_util.c is the caller that matters here.
 */
APTR AllocVec(ULONG byteSize, ULONG requirements)
{
    return AllocMem(byteSize, requirements);
}

void FreeVec(APTR memoryBlock)
{
    free(memoryBlock);
}

/* -------------------------------------------------------------------------
 * Memory pools and semaphores
 *
 * Moved to src/aros_exec_memory.c: Regina's library build needs the same
 * pools and semaphores for its per-task state, and nothing in them was about
 * BOOPSI.  Every link that takes this object takes that one too.
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Lists
 * ---------------------------------------------------------------------- */

void AddHead(struct List *list, struct Node *node)
{
    ADDHEAD(list, node);
}

void AddTail(struct List *list, struct Node *node)
{
    ADDTAIL(list, node);
}

/*
 * Guarded rather than a bare REMOVE(): real AmigaOS Remove() requires the
 * node actually be linked, but the console handler (src/aros_console_editor.c,
 * which shares this definition -- see proto/exec.h) calls it speculatively
 * on nodes that may not be, so an unguarded REMOVE() would dereference a
 * NULL/stale ln_Pred/ln_Succ. The guard is a no-op for an already-linked
 * node, so it costs BOOPSI's own real callers (rootclass.c's OM_REMOVE)
 * nothing.
 */
void Remove(struct Node *node)
{
    if (node && node->ln_Pred && node->ln_Succ)
        REMOVE(node);
}

/* -------------------------------------------------------------------------
 * Bootstrap
 *
 * A real AROS build reaches this through InitRootClass() in
 * rom/intuition/intuition_init.c.  The steps below are that function: the
 * class list and its lock, then a rootclass whose dispatcher is the real
 * rootDispatcher() from rom/intuition/rootclass.c, added to the list under
 * its standard name so FindClass("rootclass") resolves.
 * ---------------------------------------------------------------------- */

/* Users of the process-wide class list, not initialisations: the list
   outlives any one of them and may only be torn down by the last. */
static unsigned boopsi_users;

int ace_boopsi_init(void)
{
    struct IntIntuitionBase *base;

    if (IntuitionBase) {
        boopsi_users++;
        return 0;
    }
    base = calloc(1, sizeof(*base));
    if (!base)
        return -1;

    InitSemaphore(&base->ClassListLock);
    NEWLIST((struct List *)&base->ClassList);

    base->RootClass.cl_Dispatcher.h_Entry = (APTR)rootDispatcher;
    base->RootClass.cl_ID = (ClassID)ROOTCLASS;
    base->RootClass.cl_UserData = (IPTR)base;

    IntuitionBase = base;
    AddClass(&base->RootClass);
    boopsi_users = 1;
    return 0;
}

void ace_boopsi_cleanup(void)
{
    struct IntIntuitionBase *base = IntuitionBase;
    struct IClass *iclass;

    if (!base || boopsi_users == 0)
        return;
    /* Somebody else is still holding classes and objects on this list. */
    if (--boopsi_users > 0)
        return;

    /*
     * Drop every registered class except the rootclass, which is embedded in
     * the base.  FreeClass() refuses a class that still has subclasses or
     * objects, so repeat until a pass frees nothing and leaks are visible to
     * the caller rather than hidden by forced teardown.
     */
    for (;;) {
        int freed = 0;

        iclass = (struct IClass *)base->ClassList.mlh_Head;
        while (((struct MinNode *)iclass)->mln_Succ) {
            struct IClass *next =
                (struct IClass *)((struct MinNode *)iclass)->mln_Succ;

            if (iclass != &base->RootClass && FreeClass(iclass))
                freed = 1;
            iclass = next;
        }
        if (!freed)
            break;
    }

    RemoveClass(&base->RootClass);
    IntuitionBase = NULL;
    free(base);
}

struct IClass *ace_boopsi_rootclass(void)
{
    return IntuitionBase ? &IntuitionBase->RootClass : NULL;
}
