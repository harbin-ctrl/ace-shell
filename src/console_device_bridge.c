#include "console_device_bridge.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <exec/types.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/intuition.h>
#include <utility/tagitem.h>
#include <devices/conunit.h>
#include <graphics/rastport.h>

#include "consoleif.h"
#include "console_gcc.h"

#include "aros_boopsi_runtime.h"
#include "aros_graphics_runtime.h"

/* One ace-console process owns one console unit.  This sink keeps the
   imported console classes independent of GTK and the socket transport. */
static int console_input_fd = -1;

/* Set while the retained stream is being re-rendered.  A repaint is ACE's
   substitute for the per-cell character map charmapconclass would have kept,
   so it puts output the program has already produced back through the
   console a second time.  Rendering that again is the point; answering it
   again is not, and the difference only shows up on the input side. */
static int console_replaying;

static void inject_input(const void *data, size_t length)
{
    const unsigned char *bytes = data;

    while (length != 0 && console_input_fd >= 0) {
        ssize_t written = write(console_input_fd, bytes, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (written == 0)
            break;
        bytes += written;
        length -= (size_t)written;
    }
}

/*
 * con_inject() is how stdconclass.c answers a DSR cursor-position-report
 * request (ESC[6n) by injecting the reply back into the console's input
 * queue -- real behavior, but implemented at ACE's shell socket boundary
 * because the rendering path deliberately does not run console.c's task and
 * message-port machinery.
 *
 * A replay is silent. The program asked its question once and was answered
 * once; the query bytes are still in the retained stream only because that
 * stream is how ACE redraws, and re-answering would put a reply the program
 * never asked for into its input -- in the middle, as it happens, of the
 * reply it is waiting for, since a repaint is exactly what a resize does.
 */
VOID con_inject(struct ConsoleBase *console_device, struct ConUnit *unit,
                const UBYTE *data, LONG size)
{
    (void)console_device;
    (void)unit;
    if (!data || console_replaying)
        return;
    if (size < 0)
        size = (LONG)strlen((const char *)data);
    if (size > 0)
        inject_input(data, (size_t)size);
}

struct ace_console_device {
    struct ConsoleBase console_base;
    Class *console_class;
    Class *std_class;
    /* Whether this device holds a reference on the process-wide BOOPSI
       class list, so a partly-built device gives back only what it took. */
    int boopsi_held;
    struct Window amiga_window;
    struct TextFont *font;
    struct RastPort *rp;
    Object *unit;
    struct Window scrollback_window;
    struct RastPort *scrollback_rp;
    Object *scrollback_unit;
    int scrollback_active;
    int scrollback_lines;
    size_t scrollback_start;
    size_t scrollback_end;
    uint32_t palette[ACE_GFX_PEN_COUNT];
    unsigned char *history;
    size_t history_length;
    size_t history_capacity;
    int history_valid;
    /* ESC c is a complete Amiga reset sequence, but its ESC may be the last
     * byte in one CMD_WRITE and its c the first byte in the next. */
    int pending_reset_escape;
    /* The grid the last SIZEWINDOW report described, so a drag that crosses
       many pixel steps inside one character cell reports once. */
    int reported_xmax;
    int reported_ymax;
};

static const uint32_t default_palette[ACE_GFX_PEN_COUNT] = {
    0x000000u, 0xffffffu, 0xff5555u, 0x55ff55u,
    0x5555ffu, 0xffff55u, 0xff55ffu, 0x55ffffu,
};

static struct TextFont *load_font(const char *const *candidates, int pixel_size,
                                  int weight)
{
    struct ace_gfx_font_choice choice;
    struct TextFont *font;
    const char *reason = NULL;
    int i;
    int pass;

    choice.pixel_size = pixel_size;
    /* Two passes: the requested weight, then the ordinary one.  A weight is a
       preference, not a requirement -- most families ship only Regular and
       Bold, so insisting on a Medium that a fallback font has never had would
       leave the console with no font at all rather than a slightly wrong one.
       The first pass is what makes a real Medium win where one exists. */
    for (pass = 0; pass < 2; pass++) {
        choice.weight = pass == 0 ? weight : 0;
        if (pass == 1 && weight <= 0)
            break;
        for (i = 0; candidates && candidates[i]; i++) {
            choice.family = candidates[i];
            font = ace_gfx_load_font(&choice, &reason);
            if (font)
                return font;
        }
    }
    fprintf(stderr, "ace-console: no complete monospace font family found\n");
    return NULL;
}

/*
 * A console has to be at least one character cell in each direction.
 * consoleclass.c computes cu_XMax/cu_YMax as (pixels / cell) - 1, so a
 * console narrower or shorter than a single cell gets -1 -- a grid with no
 * columns or no rows -- and the class chain then spins forever trying to
 * place a character in it. Confirmed by attaching to a hung process: the
 * stack sits inside AROS's own dispatch_consoleclass(), not in ACE's code.
 * Every path that sizes a console goes through here.
 */
static void clamp_to_cell(const struct TextFont *font, int *width, int *height)
{
    if (!font)
        return;
    if (*width < (int)font->tf_XSize)
        *width = font->tf_XSize;
    if (*height < (int)font->tf_YSize)
        *height = font->tf_YSize;
}

struct ace_console_device *ace_console_device_open(
    int width, int height, const char *const *font_candidates,
    int pixel_size, int weight)
{
    struct ace_console_device *device;
    struct TagItem tags[] = {
        { A_Console_Window, 0 },
        { TAG_DONE, 0 },
    };

    device = calloc(1, sizeof(*device));
    if (!device)
        return NULL;
    device->history_valid = 1;

    if (ace_boopsi_init() != 0) {
        fprintf(stderr, "ace_console_device_open: ace_boopsi_init failed\n");
        goto fail;
    }
    device->boopsi_held = 1;

    /*
     * makeStdConClass()'s real body subclasses CONSOLECLASSPTR, a macro for
     * ConsoleDevice->consoleClass (console_gcc.h) -- it has to be set before
     * makeStdConClass() runs, not just before it returns, or MakeClass()
     * gets called with a NULL superclass and fails.
     */
    device->console_class = makeConsoleClass(&device->console_base);
    if (!device->console_class) {
        fprintf(stderr, "ace_console_device_open: consoleClass creation failed\n");
        goto fail;
    }
    device->console_base.consoleClass = device->console_class;

    device->std_class = makeStdConClass(&device->console_base);
    if (!device->std_class) {
        fprintf(stderr, "ace_console_device_open: stdConClass creation failed\n");
        goto fail;
    }
    device->console_base.stdConClass = device->std_class;

    device->font = load_font(font_candidates, pixel_size, weight);
    if (!device->font) {
        fprintf(stderr, "ace_console_device_open: font load failed\n");
        goto fail;
    }
    clamp_to_cell(device->font, &width, &height);

    device->rp = ace_gfx_create_rastport(width, height, device->font,
                                         default_palette);
    if (!device->rp) {
        fprintf(stderr, "ace_console_device_open: rastport creation failed\n");
        goto fail;
    }
    memcpy(device->palette, default_palette, sizeof(device->palette));

    device->amiga_window.RPort = device->rp;
    device->amiga_window.Width = (UWORD)width;
    device->amiga_window.Height = (UWORD)height;
    device->amiga_window.Flags = WFLG_WINDOWACTIVE;

    tags[0].ti_Data = (IPTR)&device->amiga_window;
    device->unit = NewObjectA(device->std_class, NULL, tags);
    if (!device->unit) {
        fprintf(stderr, "ace_console_device_open: NewObjectA failed\n");
        goto fail;
    }

    return device;

fail:
    ace_console_device_close(device);
    return NULL;
}

void ace_console_device_close(struct ace_console_device *device)
{
    if (!device)
        return;
    if (device->scrollback_unit)
        DisposeObject(device->scrollback_unit);
    if (device->scrollback_rp)
        ace_gfx_destroy_rastport(device->scrollback_rp);
    if (device->unit)
        DisposeObject(device->unit);
    if (device->rp)
        ace_gfx_destroy_rastport(device->rp);
    if (device->font)
        ace_gfx_unload_font(device->font);
    /*
     * The classes are this device's, made by makeConsoleClass() and
     * makeStdConClass() at open.  They used to be left to the global
     * teardown below, which meant a device that closed while another was
     * open freed that one's classes too -- the survivor then dispatched
     * every method through freed memory.  Subclass first: FreeClass()
     * refuses a class that still has one, and both refuse while an object
     * of theirs is alive, which is why the DisposeObject() calls come
     * first.
     */
    if (device->std_class)
        FreeClass(device->std_class);
    if (device->console_class)
        FreeClass(device->console_class);
    free(device->history);
    if (device->boopsi_held)
        ace_boopsi_cleanup();
    free(device);
}

/*
 * The retained stream repaints the console after a font, palette, or geometry
 * change and supplies modal scrollback. Left unbounded it would make those
 * operations cost time proportional to how long the shell had been running --
 * the console would have to re-render, and re-scroll past, every line ever
 * written.
 *
 * What is kept is sized from the console's own grid -- several screenfuls of
 * text, so a repaint reproduces every visible line and scrollback has a useful
 * tail -- with a floor for tiny windows and a ceiling so a very large one
 * cannot make a rebuild slow again. Older bytes are dropped from the front at
 * a line boundary: a partial escape sequence at the cut would otherwise be
 * replayed as stray text.
 */
#define ACE_CONSOLE_HISTORY_SCREENS 64
#define ACE_CONSOLE_HISTORY_MIN (128u * 1024u)
#define ACE_CONSOLE_HISTORY_MAX (4u * 1024u * 1024u)

/*
 * A repaint only has to reproduce what ends up visible, so it replays a few
 * screenfuls of the tail rather than everything retained. More than one
 * screenful, because a stream does not divide into screens evenly -- long
 * lines wrap, escape sequences occupy bytes but no cells -- and the extra
 * scrolls off the top exactly as it did the first time round.
 */
#define ACE_CONSOLE_REPLAY_SCREENS 3

/* Bytes of stream it takes to fill the console once. Zero if unknown. */
static size_t console_screenful(struct ace_console_device *device)
{
    size_t columns;
    size_t rows;

    if (!device->font || device->font->tf_XSize == 0 ||
        device->font->tf_YSize == 0)
        return 0;
    columns = (size_t)device->amiga_window.Width / device->font->tf_XSize;
    rows = (size_t)device->amiga_window.Height / device->font->tf_YSize;
    /* One newline per line on top of the cells themselves. */
    return rows * (columns + 1);
}

static size_t history_limit(struct ace_console_device *device)
{
    size_t screenful = console_screenful(device);
    size_t limit;

    if (screenful == 0)
        return ACE_CONSOLE_HISTORY_MIN;
    limit = ACE_CONSOLE_HISTORY_SCREENS * screenful;
    if (limit < ACE_CONSOLE_HISTORY_MIN)
        limit = ACE_CONSOLE_HISTORY_MIN;
    if (limit > ACE_CONSOLE_HISTORY_MAX)
        limit = ACE_CONSOLE_HISTORY_MAX;
    return limit;
}

static void trim_history(struct ace_console_device *device)
{
    size_t limit = history_limit(device);
    size_t cut;

    if (device->history_length <= limit)
        return;

    cut = device->history_length - limit;
    while (cut < device->history_length && device->history[cut] != '\n')
        cut++;
    if (cut < device->history_length)
        cut++; /* start just after the newline, not on it */
    if (cut >= device->history_length) {
        device->history_length = 0;
        return;
    }
    memmove(device->history, device->history + cut,
            device->history_length - cut);
    device->history_length -= cut;
}

static int save_history(struct ace_console_device *device,
                        const void *data, size_t length)
{
    size_t needed;
    size_t capacity;
    unsigned char *history;

    if (length == 0)
        return 0;
    if (!device->history_valid ||
        length > (size_t)-1 - device->history_length) {
        device->history_valid = 0;
        return -1;
    }
    trim_history(device);
    needed = device->history_length + length;
    if (needed <= device->history_capacity) {
        memcpy(device->history + device->history_length, data, length);
        device->history_length = needed;
        return 0;
    }

    capacity = device->history_capacity ? device->history_capacity : 4096;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    history = realloc(device->history, capacity);
    if (!history) {
        device->history_valid = 0;
        return -1;
    }
    device->history = history;
    device->history_capacity = capacity;
    memcpy(device->history + device->history_length, data, length);
    device->history_length = needed;

    return 0;
}

static void write_direct(struct ace_console_device *device, Object *unit,
                         const void *data, size_t length)
{
    const unsigned char *bytes = data;

    while (length != 0) {
        ULONG chunk = length > 65536 ? 65536 : (ULONG)length;

        writeToConsole((struct ConUnit *)unit, (STRPTR)bytes, chunk,
                       &device->console_base);
        bytes += chunk;
        length -= chunk;
    }
}

/* AmigaOS defines ESC c as Reset to Initial State.  The imported AROS
 * console classes expose the C_ESC token but do not implement that reset, so
 * ACE handles this one documented sequence at the console-device boundary.
 * Form Feed is the Amiga-native clear-and-home operation and is implemented
 * by both the classic console and the imported renderer. */
static void write_amiga_stream(struct ace_console_device *device, Object *unit,
                               const void *data, size_t length,
                               int *pending_reset_escape)
{
    const unsigned char *bytes = data;
    unsigned char ordinary[8192];
    size_t ordinary_length = 0;

    while (length-- != 0) {
        unsigned char byte = *bytes++;

        if (*pending_reset_escape) {
            if (byte == 'c') {
                static const unsigned char form_feed = 0x0c;

                if (ordinary_length != 0) {
                    write_direct(device, unit, ordinary, ordinary_length);
                    ordinary_length = 0;
                }
                write_direct(device, unit, &form_feed, sizeof(form_feed));
                *pending_reset_escape = 0;
                continue;
            }
            ordinary[ordinary_length++] = '\033';
            *pending_reset_escape = 0;
        }

        if (byte == '\033') {
            *pending_reset_escape = 1;
            continue;
        }
        ordinary[ordinary_length++] = byte;
        if (ordinary_length == sizeof(ordinary)) {
            write_direct(device, unit, ordinary, ordinary_length);
            ordinary_length = 0;
        }
    }
    if (ordinary_length != 0)
        write_direct(device, unit, ordinary, ordinary_length);
}

static void destroy_scrollback_view(struct ace_console_device *device)
{
    if (device->scrollback_unit)
        DisposeObject(device->scrollback_unit);
    if (device->scrollback_rp)
        ace_gfx_destroy_rastport(device->scrollback_rp);
    device->scrollback_unit = NULL;
    device->scrollback_rp = NULL;
    device->scrollback_active = 0;
    device->scrollback_lines = 0;
    device->scrollback_start = 0;
    device->scrollback_end = 0;
}

/* Return the byte position at which a historical view should stop after
 * moving back by visual newline-sized lines. The AROS console remains the
 * authority for wrapping and escape sequences; this boundary only chooses a
 * replay endpoint, and starting at a line boundary keeps the retained stream
 * useful after it has been trimmed. */
static size_t history_end_for_lines(struct ace_console_device *device,
                                    int lines, int *actual)
{
    size_t end = device->history_length;
    int moved = 0;

    while (moved < lines && end != 0) {
        size_t position = end - 1;

        while (position != 0 && device->history[position - 1] != '\n')
            position--;
        end = position;
        moved++;
    }
    if (actual)
        *actual = moved;
    return end;
}

static size_t replay_start_for_end(struct ace_console_device *device,
                                   size_t end);

/* Build a second AROS console unit from the retained stream. The live unit
 * continues to receive output while this one remains unchanged, which is what
 * makes scrollback modal without suspending the program in the shell. */
static int create_scrollback_view(struct ace_console_device *device,
                                  size_t end)
{
    struct RastPort *rp;
    Object *unit;
    size_t start;
    int pending_reset_escape = 0;
    struct TagItem tags[] = {
        { A_Console_Window, 0 },
        { TAG_DONE, 0 },
    };

    rp = ace_gfx_create_rastport(device->amiga_window.Width,
                                 device->amiga_window.Height, device->font,
                                 device->palette);
    if (!rp)
        return -1;

    device->scrollback_window = device->amiga_window;
    device->scrollback_window.RPort = rp;
    tags[0].ti_Data = (IPTR)&device->scrollback_window;
    unit = NewObjectA(device->std_class, NULL, tags);
    if (!unit) {
        ace_gfx_destroy_rastport(rp);
        return -1;
    }

    Console_NewWindowSize(unit);
    console_replaying++;
    start = replay_start_for_end(device, end);
    if (end > start)
        write_amiga_stream(device, unit, device->history + start, end - start,
                           &pending_reset_escape);
    console_replaying--;
    device->scrollback_rp = rp;
    device->scrollback_unit = unit;
    device->scrollback_start = start;
    device->scrollback_end = end;
    return 0;
}

/*
 * Where in the retained stream a repaint should start: far enough back to
 * fill the console several times over, cut at a line boundary so a partial
 * escape sequence is never replayed as stray text. Everything older than
 * that would only scroll off the top again, and re-rendering it is the whole
 * cost of a repaint.
 */
static size_t replay_start_for_end(struct ace_console_device *device,
                                   size_t end)
{
    size_t screenful = console_screenful(device);
    size_t want;
    size_t cut;

    if (screenful == 0)
        return 0;
    want = ACE_CONSOLE_REPLAY_SCREENS * screenful;
    if (end <= want)
        return 0;

    cut = end - want;
    while (cut < end && device->history[cut] != '\n')
        cut++;
    if (cut < end)
        cut++; /* start just after the newline, not on it */
    /* No line boundary in the tail: replaying all of it is still correct. */
    return cut < end ? cut : 0;
}

static size_t replay_start(struct ace_console_device *device)
{
    return replay_start_for_end(device, device->history_length);
}

static void replay_history(struct ace_console_device *device, Object *unit)
{
    size_t start = replay_start(device);
    int pending_reset_escape = 0;

    console_replaying++;
    if (device->history_length > start)
        write_amiga_stream(device, unit, device->history + start,
                           device->history_length - start,
                           &pending_reset_escape);
    console_replaying--;
}

struct console_text_line {
    unsigned char *data;
    size_t length;
    size_t capacity;
};

struct console_text_document {
    struct console_text_line *lines;
    size_t line_count;
    size_t line_capacity;
    int columns;
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_x;
    size_t saved_y;
};

static void free_text_document(struct console_text_document *document)
{
    for (size_t index = 0; index < document->line_count; index++)
        free(document->lines[index].data);
    free(document->lines);
    memset(document, 0, sizeof(*document));
}

static int ensure_text_line(struct console_text_document *document,
                            size_t index)
{
    size_t capacity;
    struct console_text_line *lines;

    if (index < document->line_count)
        return 0;
    if (index < document->line_capacity) {
        document->line_count = index + 1;
        return 0;
    }
    capacity = document->line_capacity ? document->line_capacity : 16;
    while (capacity <= index) {
        if (capacity > (size_t)-1 / 2)
            return -1;
        capacity *= 2;
    }
    lines = realloc(document->lines, capacity * sizeof(*lines));
    if (!lines)
        return -1;
    memset(lines + document->line_capacity, 0,
           (capacity - document->line_capacity) * sizeof(*lines));
    document->lines = lines;
    document->line_capacity = capacity;
    document->line_count = index + 1;
    return 0;
}

static int ensure_text_cell(struct console_text_document *document,
                            size_t x)
{
    struct console_text_line *line;
    size_t capacity;
    unsigned char *data;

    if (ensure_text_line(document, document->cursor_y) != 0)
        return -1;
    line = &document->lines[document->cursor_y];
    if (x < line->capacity) {
        if (x >= line->length) {
            memset(line->data + line->length, ' ', x - line->length + 1);
            line->length = x + 1;
        }
        return 0;
    }
    capacity = line->capacity ? line->capacity : 32;
    while (capacity <= x) {
        if (capacity > (size_t)-1 / 2)
            return -1;
        capacity *= 2;
    }
    data = realloc(line->data, capacity);
    if (!data)
        return -1;
    memset(data + line->length, ' ', x - line->length + 1);
    line->data = data;
    line->capacity = capacity;
    line->length = x + 1;
    return 0;
}

static int text_document_init(struct console_text_document *document,
                              int columns)
{
    memset(document, 0, sizeof(*document));
    document->columns = columns > 0 ? columns : 1;
    return ensure_text_line(document, 0);
}

static int text_put(struct console_text_document *document, unsigned char byte)
{
    if (document->cursor_x >= (size_t)document->columns) {
        document->cursor_x = 0;
        document->cursor_y++;
    }
    if (ensure_text_cell(document, document->cursor_x) != 0)
        return -1;
    document->lines[document->cursor_y].data[document->cursor_x] = byte;
    document->cursor_x++;
    return 0;
}

static int text_newline(struct console_text_document *document)
{
    document->cursor_x = 0;
    document->cursor_y++;
    return ensure_text_line(document, document->cursor_y);
}

static void text_clear_all(struct console_text_document *document)
{
    for (size_t index = 1; index < document->line_count; index++) {
        free(document->lines[index].data);
        document->lines[index].data = NULL;
        document->lines[index].length = 0;
        document->lines[index].capacity = 0;
    }
    if (document->line_count != 0)
        document->line_count = 1;
    for (size_t index = 0; index < document->line_count; index++)
        document->lines[index].length = 0;
    document->cursor_x = 0;
    document->cursor_y = 0;
}

static int csi_argument(const int *parameters, int count, int index, int value)
{
    if (index >= count || parameters[index] <= 0)
        return value;
    return parameters[index];
}

static void text_erase_line(struct console_text_document *document, int mode)
{
    struct console_text_line *line;

    if (ensure_text_line(document, document->cursor_y) != 0)
        return;
    line = &document->lines[document->cursor_y];
    if (mode == 2)
        line->length = 0;
    else if (mode == 1) {
        if (ensure_text_cell(document, document->cursor_x) != 0)
            return;
        memset(line->data, ' ', document->cursor_x + 1);
    } else if (document->cursor_x < line->length) {
        line->length = document->cursor_x;
    }
}

static void text_erase_display(struct console_text_document *document)
{
    struct console_text_line *line;

    if (ensure_text_line(document, document->cursor_y) != 0)
        return;
    line = &document->lines[document->cursor_y];
    if (document->cursor_x < line->length)
        line->length = document->cursor_x;
    for (size_t index = document->cursor_y + 1;
         index < document->line_count; index++) {
        free(document->lines[index].data);
        document->lines[index].data = NULL;
        document->lines[index].length = 0;
        document->lines[index].capacity = 0;
    }
    document->line_count = document->cursor_y + 1;
}

static void text_csi(struct console_text_document *document, int final,
                     const int *parameters, int count)
{
    int amount;

    switch (final) {
    case 'A':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_y = document->cursor_y > (size_t)amount
                                  ? document->cursor_y - amount
                                  : 0;
        break;
    case 'B':
    case 'e':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_y += (size_t)amount;
        (void)ensure_text_line(document, document->cursor_y);
        break;
    case 'C':
    case 'a':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_x += (size_t)amount;
        break;
    case 'D':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_x = document->cursor_x > (size_t)amount
                                  ? document->cursor_x - amount
                                  : 0;
        break;
    case 'E':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_y += (size_t)amount;
        document->cursor_x = 0;
        (void)ensure_text_line(document, document->cursor_y);
        break;
    case 'F':
        amount = csi_argument(parameters, count, 0, 1);
        document->cursor_y = document->cursor_y > (size_t)amount
                                  ? document->cursor_y - amount
                                  : 0;
        document->cursor_x = 0;
        break;
    case 'G':
    case '`':
        document->cursor_x = (size_t)(csi_argument(parameters, count, 0, 1) - 1);
        break;
    case 'd':
        document->cursor_y = (size_t)(csi_argument(parameters, count, 0, 1) - 1);
        (void)ensure_text_line(document, document->cursor_y);
        break;
    case 'H':
    case 'f':
        document->cursor_y = (size_t)(csi_argument(parameters, count, 0, 1) - 1);
        document->cursor_x = (size_t)(csi_argument(parameters, count, 1, 1) - 1);
        (void)ensure_text_line(document, document->cursor_y);
        break;
    case 'J':
        /* AmigaOS 3.1's ED command is parameterless and erases from the
         * cursor through the end of the display.  A home followed by CSI J
         * is therefore the native full-window clear idiom.  Do not import
         * later ANSI CSI 2J semantics here: the Amiga console parser does
         * not define a parameter for J. */
        if (count == 0 || (count == 1 && parameters[0] < 0))
            text_erase_display(document);
        break;
    case 'K':
        text_erase_line(document, csi_argument(parameters, count, 0, 0));
        break;
    case 'P':
        amount = csi_argument(parameters, count, 0, 1);
        if (ensure_text_line(document, document->cursor_y) != 0)
            break;
        {
            struct console_text_line *line =
                &document->lines[document->cursor_y];
            size_t available;

            if (document->cursor_x >= line->length)
                break;
            available = line->length - document->cursor_x;
            if ((size_t)amount > available)
                amount = (int)available;
            memmove(line->data + document->cursor_x,
                    line->data + document->cursor_x + amount,
                    line->length - document->cursor_x - (size_t)amount);
            line->length -= (size_t)amount;
        }
        break;
    case 's':
        document->saved_x = document->cursor_x;
        document->saved_y = document->cursor_y;
        break;
    case 'u':
        document->cursor_x = document->saved_x;
        document->cursor_y = document->saved_y;
        (void)ensure_text_line(document, document->cursor_y);
        break;
    default:
        break;
    }
}

static size_t text_skip_csi(struct console_text_document *document,
                            const unsigned char *data, size_t length,
                            size_t index)
{
    int parameters[16] = {0};
    int count = 0;
    int value = -1;

    for (; index < length; index++) {
        unsigned char byte = data[index];

        if (byte >= '0' && byte <= '9') {
            if (value < 0)
                value = 0;
            if (value <= (INT_MAX - (byte - '0')) / 10)
                value = value * 10 + (byte - '0');
        } else if (byte == ';') {
            if (count < (int)(sizeof(parameters) / sizeof(parameters[0])))
                parameters[count++] = value;
            value = -1;
        } else if (byte >= 0x40 && byte <= 0x7e) {
            if (count < (int)(sizeof(parameters) / sizeof(parameters[0])))
                parameters[count++] = value;
            text_csi(document, byte, parameters, count);
            return index + 1;
        }
    }
    return length;
}

static size_t text_skip_escape(struct console_text_document *document,
                               const unsigned char *data, size_t length,
                               size_t position)
{
    size_t index = position + 1;

    /* Amiga console.device uses the single-byte C1 CSI introducer.  Keep
     * accepting ESC [ as well for the ANSI-compatible paths, but make the
     * native form first-class so copied history never exposes 0x9b or its
     * final byte as text. */
    if (data[position] == 0x9b)
        return text_skip_csi(document, data, length, index);
    if (index >= length)
        return length;
    if (data[index] == '[')
        return text_skip_csi(document, data, length, index + 1);
    if (data[index] == ']') {
        for (index++; index < length; index++) {
            if (data[index] == '\a')
                return index + 1;
            if (data[index] == '\033' && index + 1 < length &&
                data[index + 1] == '\\')
                return index + 2;
        }
        return length;
    }
    if (data[index] == '7') {
        document->saved_x = document->cursor_x;
        document->saved_y = document->cursor_y;
    } else if (data[index] == '8') {
        document->cursor_x = document->saved_x;
        document->cursor_y = document->saved_y;
        (void)ensure_text_line(document, document->cursor_y);
    } else if (data[index] == 'c') {
        /* RIS is the other documented Amiga clear-screen spelling. */
        text_clear_all(document);
    }
    return index + 1;
}

static int build_text_document(struct ace_console_device *device,
                               size_t start, size_t end,
                               struct console_text_document *document)
{
    int columns = device->font && device->font->tf_XSize
                      ? device->amiga_window.Width / device->font->tf_XSize
                      : 1;

    if (text_document_init(document, columns) != 0)
        return -1;
    for (size_t index = start; index < end; index++) {
        unsigned char byte = device->history[index];

        if (byte == '\033' || byte == 0x9b) {
            index = text_skip_escape(document, device->history, end, index);
            if (index == 0)
                break;
            index--;
        } else if (byte == '\n') {
            if (text_newline(document) != 0)
                goto fail;
        } else if (byte == '\r') {
            document->cursor_x = 0;
        } else if (byte == 0x0c) {
            /* FORM FEED clears the window and homes the cursor. */
            text_clear_all(document);
        } else if (byte == '\b') {
            if (document->cursor_x != 0)
                document->cursor_x--;
        } else if (byte == '\t') {
            do {
                if (text_put(document, ' ') != 0)
                    goto fail;
            } while (document->cursor_x % 8 != 0);
        } else if (byte >= 0x20 && byte != 0x7f) {
            if (text_put(document, byte) != 0)
                goto fail;
        }
    }
    return 0;

fail:
    free_text_document(document);
    return -1;
}

static int append_copy_bytes(char **result, size_t *length, size_t *capacity,
                             const unsigned char *data, size_t amount)
{
    char *next;
    size_t needed;

    if (amount == 0)
        return 0;
    if (amount > (size_t)-1 - *length - 1)
        return -1;
    needed = *length + amount + 1;
    if (needed > *capacity) {
        size_t next_capacity = *capacity ? *capacity : 128;

        while (next_capacity < needed) {
            if (next_capacity > (size_t)-1 / 2)
                next_capacity = needed;
            else
                next_capacity *= 2;
        }
        next = realloc(*result, next_capacity);
        if (!next)
            return -1;
        *result = next;
        *capacity = next_capacity;
    }
    memcpy(*result + *length, data, amount);
    *length += amount;
    (*result)[*length] = '\0';
    return 0;
}

static int append_text_line(char **result, size_t *length, size_t *capacity,
                            const struct console_text_line *line,
                            size_t start, size_t end)
{
    if (start >= line->length || start >= end)
        return 0;
    if (end > line->length)
        end = line->length;
    while (end > start && line->data[end - 1] == ' ')
        end--;
    return append_copy_bytes(result, length, capacity, line->data + start,
                             end - start);
}

static char *document_to_text(const struct console_text_document *document,
                              size_t first, size_t last, int start_column,
                              int end_column, size_t *length_out)
{
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (document->line_count == 0)
        first = last = 0;
    for (size_t index = first; index <= last && index < document->line_count;
         index++) {
        size_t start = index == first ? (size_t)start_column : 0;
        size_t end = index == last ? (size_t)end_column + 1
                                   : (size_t)document->columns;

        if (append_text_line(&result, &length, &capacity,
                             &document->lines[index], start, end) != 0)
            goto fail;
        if (index != last &&
            append_copy_bytes(&result, &length, &capacity,
                              (const unsigned char *)"\n", 1) != 0)
            goto fail;
    }
    if (!result) {
        result = malloc(1);
        if (!result)
            return NULL;
        result[0] = '\0';
    }
    if (length_out)
        *length_out = length;
    return result;

fail:
    free(result);
    return NULL;
}

static char *copy_document_all(const struct console_text_document *document,
                               size_t *length_out)
{
    if (document->line_count == 0)
        return document_to_text(document, 0, 0, 0, 0, length_out);
    return document_to_text(document, 0, document->line_count - 1, 0,
                            document->columns - 1, length_out);
}

static int device_cell_size(struct ace_console_device *device,
                            int *width_out, int *height_out)
{
    if (!device || !device->font || device->font->tf_XSize == 0 ||
        device->font->tf_YSize == 0)
        return -1;
    if (width_out)
        *width_out = device->font->tf_XSize;
    if (height_out)
        *height_out = device->font->tf_YSize;
    return 0;
}

int ace_console_device_cell_size(struct ace_console_device *device,
                                 int *width_out, int *height_out)
{
    return device_cell_size(device, width_out, height_out);
}

size_t ace_console_device_grid_capacity(struct ace_console_device *device)
{
    int cell_width;
    int cell_height;

    if (!device || device_cell_size(device, &cell_width, &cell_height) != 0)
        return 0;
    return ((size_t)device->amiga_window.Width / (size_t)cell_width) *
           ((size_t)device->amiga_window.Height / (size_t)cell_height);
}

void ace_console_device_set_render_deferred(struct ace_console_device *device,
                                            int deferred)
{
    if (device)
        ace_gfx_set_render_deferred(device->rp, deferred);
}

void ace_console_device_render_grid(struct ace_console_device *device)
{
    if (device)
        ace_gfx_render_grid(device->rp);
}

uint64_t ace_console_device_grid_fingerprint(
    struct ace_console_device *device)
{
    return device ? ace_gfx_grid_fingerprint(device->rp) : 0;
}

char *ace_console_device_copy_all(struct ace_console_device *device,
                                   size_t *length_out)
{
    struct console_text_document document;
    char *result;

    if (!device || !device->history_valid ||
        build_text_document(device, 0, device->history_length, &document) != 0)
        return NULL;
    result = copy_document_all(&document, length_out);
    free_text_document(&document);
    return result;
}

char *ace_console_device_copy_selection(struct ace_console_device *device,
                                        int start_column, int start_row,
                                        int end_column, int end_row,
                                        size_t *length_out)
{
    struct console_text_document document;
    size_t start;
    size_t end;
    size_t first_line;
    int cell_width;
    int cell_height;
    int columns;
    int rows;
    int temporary;
    char *result;

    if (!device || !device->history_valid ||
        device_cell_size(device, &cell_width, &cell_height) != 0)
        return NULL;
    if (start_row > end_row ||
        (start_row == end_row && start_column > end_column)) {
        temporary = start_column;
        start_column = end_column;
        end_column = temporary;
        temporary = start_row;
        start_row = end_row;
        end_row = temporary;
    }
    columns = device->amiga_window.Width / cell_width;
    rows = device->amiga_window.Height / cell_height;
    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;
    if (start_column < 0)
        start_column = 0;
    if (end_column >= columns)
        end_column = columns - 1;
    if (start_row < 0)
        start_row = 0;
    if (end_row >= rows)
        end_row = rows - 1;
    if (start_row > end_row || start_column > end_column)
        return NULL;

    if (device->scrollback_active) {
        start = device->scrollback_start;
        end = device->scrollback_end;
    } else {
        start = replay_start_for_end(device, device->history_length);
        end = device->history_length;
    }
    if (build_text_document(device, start, end, &document) != 0)
        return NULL;
    first_line = document.line_count > (size_t)rows
                     ? document.line_count - (size_t)rows
                     : 0;
    result = document_to_text(&document, first_line + (size_t)start_row,
                              first_line + (size_t)end_row, start_column,
                              end_column, length_out);
    free_text_document(&document);
    return result;
}

/*
 * Build a new RastPort/unit pair, replay the AROS console stream into it,
 * then retire the old pair.  This is ACE's equivalent of charmapconclass's
 * retained per-cell lines plus charmapcon_refresh(): stdconclass itself only
 * paints the live RastPort and cannot repaint after its font, palette, or
 * dimensions change.
 */
static int replace_render_state(struct ace_console_device *device,
                                int width, int height,
                                struct TextFont *font,
                                const uint32_t palette[ACE_GFX_PEN_COUNT])
{
    struct RastPort *rp;
    struct RastPort *old_rp;
    Object *unit;
    Object *old_unit;
    UWORD old_width;
    UWORD old_height;
    struct TagItem tags[] = {
        { A_Console_Window, 0 },
        { TAG_DONE, 0 },
    };

    if (!device || !font || !device->history_valid)
        return -1;
    destroy_scrollback_view(device);
    clamp_to_cell(font, &width, &height);
    if (width > 65535)
        width = 65535;
    if (height > 65535)
        height = 65535;

    rp = ace_gfx_create_rastport(width, height, font, palette);
    if (!rp)
        return -1;

    old_rp = device->rp;
    old_unit = device->unit;
    old_width = device->amiga_window.Width;
    old_height = device->amiga_window.Height;
    device->amiga_window.RPort = rp;
    device->amiga_window.Width = (UWORD)width;
    device->amiga_window.Height = (UWORD)height;
    tags[0].ti_Data = (IPTR)&device->amiga_window;
    unit = NewObjectA(device->std_class, NULL, tags);
    if (!unit) {
        device->amiga_window.RPort = old_rp;
        device->amiga_window.Width = old_width;
        device->amiga_window.Height = old_height;
        ace_gfx_destroy_rastport(rp);
        return -1;
    }

    device->rp = rp;
    device->unit = unit;
    /* The constructor has the new dimensions already, but issue the same
     * notification that AROS's console task sends after a real window resize.
     * This keeps the class-chain geometry update explicit for both resize and
     * font/palette rebuilds. */
    Console_NewWindowSize(unit);
    replay_history(device, unit);

    if (old_unit)
        DisposeObject(old_unit);
    ace_gfx_destroy_rastport(old_rp);
    return 0;
}

int ace_console_device_set_scrollback(struct ace_console_device *device,
                                      int lines)
{
    size_t end;
    int actual;

    if (!device || lines < 0 || !device->history_valid ||
        device->history_length == 0) {
        if (device)
            destroy_scrollback_view(device);
        return 0;
    }
    end = history_end_for_lines(device, lines, &actual);
    destroy_scrollback_view(device);
    if (create_scrollback_view(device, end) != 0)
        return 0;
    device->scrollback_active = 1;
    device->scrollback_lines = actual;
    return actual;
}

void ace_console_device_clear_scrollback(struct ace_console_device *device)
{
    if (device)
        destroy_scrollback_view(device);
}

int ace_console_device_scrollback_active(struct ace_console_device *device)
{
    return device ? device->scrollback_active : 0;
}

int ace_console_device_scrollback_lines(struct ace_console_device *device)
{
    return device ? device->scrollback_lines : 0;
}

cairo_surface_t *ace_console_device_scrollback_surface(
    struct ace_console_device *device)
{
    return device && device->scrollback_rp
               ? ace_gfx_rastport_surface(device->scrollback_rp)
               : NULL;
}

int ace_console_device_scrollback_origin_y(struct ace_console_device *device)
{
    return device && device->scrollback_rp
               ? ace_gfx_rastport_origin_y(device->scrollback_rp)
               : 0;
}

void ace_console_device_write(struct ace_console_device *device,
                              const void *data, size_t length)
{
    if (!device || !data || length == 0)
        return;
    (void)save_history(device, data, length);
    write_amiga_stream(device, device->unit, data, length,
                       &device->pending_reset_escape);
}

int ace_console_device_set_font(struct ace_console_device *device,
                                const char *family, int pixel_size, int weight)
{
    struct ace_gfx_font_choice choice;
    struct TextFont *font;
    struct TextFont *old_font;

    if (!device || !family || pixel_size <= 0)
        return -1;
    choice.family = family;
    choice.pixel_size = pixel_size;
    choice.weight = weight;
    font = ace_gfx_load_font(&choice, NULL);
    if (!font)
        return -1;
    if (replace_render_state(device, device->amiga_window.Width,
                             device->amiga_window.Height, font,
                             device->palette) != 0) {
        ace_gfx_unload_font(font);
        return -1;
    }
    ace_gfx_unload_font(device->font);
    device->font = font;
    return 0;
}

int ace_console_device_set_palette(
    struct ace_console_device *device,
    const uint32_t rgb[ACE_CONSOLE_PEN_COUNT])
{
    if (!device || !rgb)
        return -1;
    if (replace_render_state(device, device->amiga_window.Width,
                             device->amiga_window.Height, device->font,
                             rgb) != 0)
        return -1;
    memcpy(device->palette, rgb, sizeof(device->palette));
    return 0;
}

void ace_console_device_set_pen_rgb(struct ace_console_device *device,
                                    int pen, uint32_t rgb)
{
    uint32_t palette[ACE_GFX_PEN_COUNT];

    if (!device || pen < 0 || pen >= ACE_GFX_PEN_COUNT)
        return;
    memcpy(palette, device->palette, sizeof(palette));
    palette[pen] = rgb;
    (void)ace_console_device_set_palette(device, palette);
}

int ace_console_device_resize(struct ace_console_device *device,
                              int width, int height)
{
    struct ConUnit *unit;
    WORD old_xmax;
    WORD old_ymax;

    if (!device || width <= 0 || height <= 0)
        return -1;
    /* Clamped before the early-out, so a window dragged below one cell
     * settles on the clamped size instead of retrying on every step. */
    clamp_to_cell(device->font, &width, &height);
    if (width == device->amiga_window.Width &&
        height == device->amiga_window.Height)
        return 0;
    destroy_scrollback_view(device);
    if (ace_gfx_resize_rastport(device->rp, width, height) != 0)
        return -1;
    device->amiga_window.Width = (UWORD)width;
    device->amiga_window.Height = (UWORD)height;

    /*
     * A resize that leaves the character grid the same size -- anything
     * smaller than one cell, which is most of the steps a drag delivers --
     * changes nothing about the layout. The font is unchanged, so every cell
     * keeps its pixel position, and the pixels already on screen stay valid
     * across the geometry update.
     *
     * A resize that does change the grid has to repaint, because the text on
     * screen was laid out against the old column count and the old bottom
     * row. Repainting the retained stream is what ACE has instead of the
     * character map AROS's own charmapconclass would have used -- the same
     * mechanism a typeface or palette change goes through -- and it is what
     * re-wraps the text to the new width and puts the last line back on the
     * last row.
     *
     * Shrinking would need this in any case: console_newwindowsize() clamps
     * the cursor into the new grid, and stdcon_newwindowsize() responds to a
     * cursor that moved by clearing the whole console and redrawing the
     * cursor -- a character-cell renderer with no retained character map has
     * no way to tidy up the cursor it left at the old position without
     * wiping what it cannot redraw. Clamping only ever goes downwards, so
     * enlarging never triggers that clear; it needs the repaint for the
     * layout alone, which is why the test here is the grid rather than the
     * cursor.
     *
     * The console is cleared and homed first, with a real form feed, because
     * the replay has to start from the top left, wherever AROS has left the
     * cursor.
     */
    unit = (struct ConUnit *)device->unit;
    old_xmax = unit->cu_XMax;
    old_ymax = unit->cu_YMax;
    Console_NewWindowSize(device->unit);
    if ((unit->cu_XMax != old_xmax || unit->cu_YMax != old_ymax) &&
        device->history_length != 0) {
        static const unsigned char form_feed = 0x0c;

        write_direct(device, device->unit, &form_feed, sizeof(form_feed));
        replay_history(device, device->unit);
    }
    return 0;
}

void ace_console_device_set_input_fd(struct ace_console_device *device, int fd)
{
    (void)device;
    console_input_fd = fd;
}

void ace_console_device_notify_resize(struct ace_console_device *device)
{
    struct ConUnit *unit;
    char report[128];
    int length;

    if (!device || console_input_fd < 0 || !device->unit)
        return;
    unit = (struct ConUnit *)device->unit;
    if (!CHECK_RAWEVENT((Object *)unit, IECLASS_SIZEWINDOW))
        return;

    /*
     * A program answers this report by asking the console for its bounds,
     * and it reads that answer with a plain read of whatever arrives next.
     * So a second report sent before the program has asked does not tell it
     * anything new -- it lands in the answer's place and is read as one, and
     * a size report is not a valid answer to a bounds request.
     *
     * Reporting once per character grid is what keeps that from happening
     * during a drag, which delivers a resize every frame: the pixel steps in
     * between change nothing a console program can act on, since a console
     * program is laid out in cells.
     */
    if (unit->cu_XMax == device->reported_xmax &&
        unit->cu_YMax == device->reported_ymax)
        return;
    device->reported_xmax = unit->cu_XMax;
    device->reported_ymax = unit->cu_YMax;

    /* Match consoleTask's report_raw_event() format: event class, subclass,
       code, qualifier, pixel X/Y, seconds, and microseconds. */
    length = snprintf(report, sizeof(report),
                      "\233%u;0;0;0;%u;%u;0;0|",
                      (unsigned)IECLASS_SIZEWINDOW,
                      (unsigned)device->amiga_window.Width,
                      (unsigned)device->amiga_window.Height);
    if (length > 0 && (size_t)length < sizeof(report))
        inject_input(report, (size_t)length);
}

cairo_surface_t *ace_console_device_surface(struct ace_console_device *device)
{
    return device ? ace_gfx_rastport_surface(device->rp) : NULL;
}

int ace_console_device_origin_y(struct ace_console_device *device)
{
    return device ? ace_gfx_rastport_origin_y(device->rp) : 0;
}

void ace_console_device_size(struct ace_console_device *device,
                             int *width_out, int *height_out)
{
    if (width_out)
        *width_out = device ? device->amiga_window.Width : 0;
    if (height_out)
        *height_out = device ? device->amiga_window.Height : 0;
}

int ace_console_device_take_damage(struct ace_console_device *device,
                                   int *x_out, int *y_out,
                                   int *width_out, int *height_out)
{
    if (!device)
        return 0;
    return ace_gfx_take_damage(device->rp, x_out, y_out, width_out,
                               height_out);
}
