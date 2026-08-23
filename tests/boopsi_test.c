/*
 * Exercises the real AROS BOOPSI sources through the ACE host seam.
 *
 * Nothing here reimplements class or object behaviour: the two classes below
 * are ordinary BOOPSI subclasses, and every MakeClass(), NewObjectA(),
 * DoMethodA(), DoSuperMethodA(), CoerceMethodA(), SetAttrsA(), GetAttr(),
 * DisposeObject() and FreeClass() call lands in rom/intuition or
 * compiler/alib.  What is being checked is that AROS's dispatch, its
 * instance-data layout and its reference counting all behave once they sit on
 * ACE's memory, pool and semaphore seam.
 */

#include "aros_boopsi_runtime.h"

#include <assert.h>
#include <string.h>

#include <exec/types.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <utility/tagitem.h>
#include <proto/alib.h>
#include <proto/intuition.h>

#define SHAPECLASS  "ace-test-shapeclass"
#define SQUARECLASS "ace-test-squareclass"

#define SHAPEA_Width  (TAG_USER + 1)
#define SHAPEA_Height (TAG_USER + 2)
#define SQUAREA_Side  (TAG_USER + 3)

/* A method neither class inherits from rootclass. */
#define SHAPEM_Area   (0x0401)
/* A method only shapeclass answers, used to prove super-dispatch. */
#define SHAPEM_Name   (0x0402)

struct shapedata
{
    LONG width;
    LONG height;
    LONG guard;
};

struct squaredata
{
    LONG side;
    LONG guard;
};

static int shape_disposals;
static int square_disposals;

static struct TagItem *find_tag(struct TagItem *tags, Tag want)
{
    for (; tags && tags->ti_Tag != TAG_DONE; tags++) {
        if (tags->ti_Tag == want)
            return tags;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */

static IPTR shape_dispatch(Class *cl, Object *o, Msg msg)
{
    struct shapedata *data;
    struct TagItem *tag;

    switch (msg->MethodID) {
    case OM_NEW:
        o = (Object *)DoSuperMethodA(cl, o, msg);
        if (!o)
            return 0;
        data = INST_DATA(cl, o);
        data->guard = 0x5AFE;
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SHAPEA_Width);
        if (tag)
            data->width = (LONG)tag->ti_Data;
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SHAPEA_Height);
        if (tag)
            data->height = (LONG)tag->ti_Data;
        return (IPTR)o;

    case OM_SET:
        data = INST_DATA(cl, o);
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SHAPEA_Width);
        if (tag)
            data->width = (LONG)tag->ti_Data;
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SHAPEA_Height);
        if (tag)
            data->height = (LONG)tag->ti_Data;
        return DoSuperMethodA(cl, o, msg);

    case OM_GET:
        data = INST_DATA(cl, o);
        if (((struct opGet *)msg)->opg_AttrID == SHAPEA_Width) {
            *((struct opGet *)msg)->opg_Storage = (IPTR)data->width;
            return 1;
        }
        if (((struct opGet *)msg)->opg_AttrID == SHAPEA_Height) {
            *((struct opGet *)msg)->opg_Storage = (IPTR)data->height;
            return 1;
        }
        return DoSuperMethodA(cl, o, msg);

    case OM_DISPOSE:
        data = INST_DATA(cl, o);
        assert(data->guard == 0x5AFE);
        shape_disposals++;
        return DoSuperMethodA(cl, o, msg);

    case SHAPEM_Area:
        data = INST_DATA(cl, o);
        return (IPTR)(data->width * data->height);

    case SHAPEM_Name:
        return (IPTR)SHAPECLASS;

    default:
        return DoSuperMethodA(cl, o, msg);
    }
}

static IPTR square_dispatch(Class *cl, Object *o, Msg msg)
{
    struct squaredata *data;
    struct TagItem *tag;

    switch (msg->MethodID) {
    case OM_NEW:
        o = (Object *)DoSuperMethodA(cl, o, msg);
        if (!o)
            return 0;
        data = INST_DATA(cl, o);
        data->guard = 0xC0DE;
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SQUAREA_Side);
        if (tag)
            data->side = (LONG)tag->ti_Data;
        return (IPTR)o;

    case OM_SET:
        data = INST_DATA(cl, o);
        tag = find_tag(((struct opSet *)msg)->ops_AttrList, SQUAREA_Side);
        if (tag)
            data->side = (LONG)tag->ti_Data;
        return DoSuperMethodA(cl, o, msg);

    case OM_GET:
        data = INST_DATA(cl, o);
        if (((struct opGet *)msg)->opg_AttrID == SQUAREA_Side) {
            *((struct opGet *)msg)->opg_Storage = (IPTR)data->side;
            return 1;
        }
        return DoSuperMethodA(cl, o, msg);

    case OM_DISPOSE:
        data = INST_DATA(cl, o);
        assert(data->guard == 0xC0DE);
        square_disposals++;
        return DoSuperMethodA(cl, o, msg);

    /* Overrides the superclass, so a plain DoMethodA must land here. */
    case SHAPEM_Area:
        data = INST_DATA(cl, o);
        return (IPTR)(data->side * data->side);

    default:
        return DoSuperMethodA(cl, o, msg);
    }
}

/* ---------------------------------------------------------------------- */

static void test_bootstrap(void)
{
    struct IClass *root;

    assert(ace_boopsi_init() == 0);
    root = ace_boopsi_rootclass();
    assert(root != NULL);

    /* AROS's own rootDispatcher, installed as the class's hook entry. */
    assert(root->cl_Dispatcher.h_Entry != NULL);
    assert(strcmp((const char *)root->cl_ID, ROOTCLASS) == 0);

    /* Public, so FindClass() resolves it through the AROS class list. */
    assert(FindClass((ClassID)ROOTCLASS) == root);
    assert(FindClass((ClassID)"no-such-class") == NULL);

    /* Repeating the bootstrap must not install a second rootclass.  It
       takes a second reference to this one, which the teardown below gives
       back: the class list is process-wide, and the console device learned
       the hard way that whoever closes first must not take it from whoever
       is still using it. */
    assert(ace_boopsi_init() == 0);
    assert(ace_boopsi_rootclass() == root);
}

static void test_class_construction(struct IClass **shape_out,
                                    struct IClass **square_out)
{
    struct IClass *root = ace_boopsi_rootclass();
    struct IClass *shape;
    struct IClass *square;

    shape = MakeClass((ClassID)SHAPECLASS, NULL, root,
                      sizeof(struct shapedata), 0);
    assert(shape != NULL);
    shape->cl_Dispatcher.h_Entry = (APTR)shape_dispatch;
    AddClass(shape);

    /* Resolving the superclass by name exercises MakeClass's FindClass path. */
    square = MakeClass((ClassID)SQUARECLASS, (ClassID)SHAPECLASS, NULL,
                       sizeof(struct squaredata), 0);
    assert(square != NULL);
    square->cl_Dispatcher.h_Entry = (APTR)square_dispatch;
    AddClass(square);

    assert(FindClass((ClassID)SHAPECLASS) == shape);
    assert(FindClass((ClassID)SQUARECLASS) == square);
    assert(square->cl_Super == shape);
    assert(shape->cl_Super == root);

    /* MakeClass() refuses a duplicate public ID. */
    assert(MakeClass((ClassID)SHAPECLASS, NULL, root, 4, 0) == NULL);
    /* ...and refuses a class with no superclass at all. */
    assert(MakeClass((ClassID)"ace-test-orphan", NULL, NULL, 4, 0) == NULL);

    /*
     * Instance data is laid out by AROS: each class's slice starts where its
     * superclass's ended, and the object is big enough for the whole chain
     * plus the _Object header that carries the class pointer.
     */
    assert(shape->cl_InstOffset == root->cl_InstOffset + root->cl_InstSize);
    assert(shape->cl_InstSize == sizeof(struct shapedata));
    assert(square->cl_InstOffset >= shape->cl_InstOffset + shape->cl_InstSize);
    assert(square->cl_InstSize == sizeof(struct squaredata));
    assert(square->cl_ObjectSize ==
           square->cl_InstOffset + square->cl_InstSize +
           sizeof(struct _Object));

    assert(shape->cl_SubclassCount == 1);
    assert(square->cl_SubclassCount == 0);

    *shape_out = shape;
    *square_out = square;
}

static void test_objects(struct IClass *shape, struct IClass *square)
{
    struct TagItem create[] = {
        { SHAPEA_Width,  7 },
        { SHAPEA_Height, 5 },
        { SQUAREA_Side,  4 },
        { TAG_DONE,      0 },
    };
    struct TagItem update[] = {
        { SHAPEA_Width, 11 },
        { SQUAREA_Side,  6 },
        { TAG_DONE,      0 },
    };
    STACKULONG area = SHAPEM_Area;
    STACKULONG name = SHAPEM_Name;
    Object *object;
    IPTR storage;

    object = NewObjectA(square, NULL, create);
    assert(object != NULL);
    assert(OCLASS(object) == square);
    assert(square->cl_ObjectCount == 1);

    /*
     * Both classes wrote into the same object through their own INST_DATA
     * offsets.  If those offsets overlapped, one guard would be gone.
     */
    assert(((struct shapedata *)INST_DATA(shape, object))->guard == 0x5AFE);
    assert(((struct squaredata *)INST_DATA(square, object))->guard == 0xC0DE);

    /* OM_NEW reached both dispatchers on the way down. */
    storage = 0;
    assert(GetAttr(SHAPEA_Width, object, &storage) == 1 && storage == 7);
    storage = 0;
    assert(GetAttr(SHAPEA_Height, object, &storage) == 1 && storage == 5);
    storage = 0;
    assert(GetAttr(SQUAREA_Side, object, &storage) == 1 && storage == 4);

    /* An attribute no class in the chain answers falls through to rootclass. */
    storage = 0xdeadbeef;
    assert(GetAttr(TAG_USER + 99, object, &storage) == 0);
    assert(storage == 0xdeadbeef);

    /* DoMethodA() starts at the object's own class, so the override wins. */
    assert(DoMethodA(object, (Msg)&area) == 4 * 4);

    /* CoerceMethodA() starts at the class named instead. */
    assert(CoerceMethodA(shape, object, (Msg)&area) == 7 * 5);

    /* A method only the superclass answers still resolves through the chain. */
    assert(strcmp((const char *)DoMethodA(object, (Msg)&name), SHAPECLASS) == 0);

    /* OM_SET splits across the chain the same way OM_NEW did. */
    SetAttrsA(object, update);
    storage = 0;
    assert(GetAttr(SHAPEA_Width, object, &storage) == 1 && storage == 11);
    storage = 0;
    assert(GetAttr(SHAPEA_Height, object, &storage) == 1 && storage == 5);
    storage = 0;
    assert(GetAttr(SQUAREA_Side, object, &storage) == 1 && storage == 6);
    assert(DoMethodA(object, (Msg)&area) == 6 * 6);

    /* A class with live objects must not be freed. */
    assert(FreeClass(square) == FALSE);
    assert(FindClass((ClassID)SQUARECLASS) == NULL); /* RemoveClass() ran */
    AddClass(square);                                /* ...so put it back */
    assert(FindClass((ClassID)SQUARECLASS) == square);

    shape_disposals = 0;
    square_disposals = 0;
    DisposeObject(object);

    /* OM_DISPOSE ran once per class, outermost first. */
    assert(square_disposals == 1);
    assert(shape_disposals == 1);
    assert(square->cl_ObjectCount == 0);

    /* DisposeObject(NULL) is documented as harmless. */
    DisposeObject(NULL);
}

static void test_object_independence(struct IClass *square)
{
    struct TagItem first[] = { { SQUAREA_Side, 3 }, { TAG_DONE, 0 } };
    struct TagItem second[] = { { SQUAREA_Side, 9 }, { TAG_DONE, 0 } };
    STACKULONG area = SHAPEM_Area;
    Object *a;
    Object *b;

    a = NewObjectA(square, NULL, first);
    b = NewObjectA(square, NULL, second);
    assert(a && b && a != b);
    assert(square->cl_ObjectCount == 2);

    /* Two objects out of one class pool must not share instance data. */
    assert(DoMethodA(a, (Msg)&area) == 3 * 3);
    assert(DoMethodA(b, (Msg)&area) == 9 * 9);

    DisposeObject(a);
    assert(square->cl_ObjectCount == 1);
    assert(DoMethodA(b, (Msg)&area) == 9 * 9);
    DisposeObject(b);
    assert(square->cl_ObjectCount == 0);
}

static void test_class_teardown(struct IClass *shape, struct IClass *square)
{
    /* A class with a live subclass must not be freed. */
    assert(shape->cl_SubclassCount == 1);
    assert(FreeClass(shape) == FALSE);
    AddClass(shape);

    assert(FreeClass(square) == TRUE);
    assert(shape->cl_SubclassCount == 0);
    assert(FindClass((ClassID)SQUARECLASS) == NULL);

    assert(FreeClass(shape) == TRUE);
    assert(FindClass((ClassID)SHAPECLASS) == NULL);

    /* NewObjectA() by name after teardown finds nothing. */
    assert(NewObjectA(NULL, (UBYTE *)SQUARECLASS, NULL) == NULL);
}

static void test_creation_by_name(void)
{
    struct IClass *root = ace_boopsi_rootclass();
    struct IClass *shape;
    struct TagItem create[] = { { SHAPEA_Width, 2 }, { TAG_DONE, 0 } };
    STACKULONG area = SHAPEM_Area;
    Object *object;

    shape = MakeClass((ClassID)SHAPECLASS, NULL, root,
                      sizeof(struct shapedata), 0);
    assert(shape != NULL);
    shape->cl_Dispatcher.h_Entry = (APTR)shape_dispatch;
    AddClass(shape);

    /* Resolution by public name, the path NewCLI-style callers use. */
    object = NewObjectA(NULL, (UBYTE *)SHAPECLASS, create);
    assert(object != NULL);
    assert(OCLASS(object) == shape);
    assert(DoMethodA(object, (Msg)&area) == 0); /* height defaulted to zero */

    DisposeObject(object);
    assert(shape->cl_ObjectCount == 0);
    assert(FreeClass(shape) == TRUE);
}

int main(void)
{
    struct IClass *shape;
    struct IClass *square;

    test_bootstrap();
    test_class_construction(&shape, &square);
    test_objects(shape, square);
    test_object_independence(square);
    test_class_teardown(shape, square);
    test_creation_by_name();

    /* Two bootstraps were taken above, so the first teardown only gives one
       of them back and the list stays up for its other user. */
    ace_boopsi_cleanup();
    assert(ace_boopsi_rootclass() != NULL);

    ace_boopsi_cleanup();
    assert(ace_boopsi_rootclass() == NULL);

    /* Unbalanced teardown is harmless rather than a second free. */
    ace_boopsi_cleanup();
    assert(ace_boopsi_rootclass() == NULL);

    /* The seam is reusable: a second bootstrap starts from a clean list. */
    assert(ace_boopsi_init() == 0);
    assert(ace_boopsi_rootclass() != NULL);
    assert(FindClass((ClassID)SHAPECLASS) == NULL);
    ace_boopsi_cleanup();

    return 0;
}
