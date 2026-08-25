#include "terminal_translate.h"

#include <stdio.h>
#include <string.h>

/* console.device introduces its own sequences with the C1 byte, not ESC [. */
#define AMIGA_CSI 0x9b

/* AROS's getparamcommand() abandons a CSI that carries more than sixteen
 * parameters, and an abandoned CSI is printed rather than obeyed, so a long
 * xterm SGR has to leave here as several short ones. */
#define AMIGA_SGR_PARAM_MAX 16

#define ACE_TERMINAL_PARAM_MAX 24

/* AROS stores parsed parameters in a UBYTE. */
#define AMIGA_PARAM_MAX 255

enum xterm_sequence_kind {
    XTERM_NONE = 0,
    XTERM_ESCAPE,
    XTERM_CSI,
    XTERM_OSC,
    XTERM_OSC_ESCAPE,
};

static int emit(const struct ace_terminal_sink *sink, const void *bytes,
                size_t length)
{
    return sink->write(sink->context, bytes, length);
}

static int emit_text(const struct ace_terminal_sink *sink, const char *text)
{
    return emit(sink, text, strlen(text));
}

/* CSI <value> <final>, the shape of nearly every console.device command. */
static int emit_csi(const struct ace_terminal_sink *sink, unsigned value,
                    char final)
{
    char sequence[16];
    int length = snprintf(sequence, sizeof(sequence), "%c%u%c", AMIGA_CSI,
                          value, final);

    return emit(sink, sequence, (size_t)length);
}

static int emit_csi_pair(const struct ace_terminal_sink *sink, unsigned first,
                         unsigned second, char final)
{
    char sequence[24];
    int length = snprintf(sequence, sizeof(sequence), "%c%u;%u%c", AMIGA_CSI,
                          first, second, final);

    return emit(sink, sequence, (size_t)length);
}

static int emit_repeat(const struct ace_terminal_sink *sink,
                       const char *unit, size_t count)
{
    while (count--) {
        if (emit_text(sink, unit) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ACE console keyboard -> xterm                                       */
/* ------------------------------------------------------------------ */

void ace_amiga_to_xterm_init(struct ace_amiga_to_xterm *state)
{
    memset(state, 0, sizeof(*state));
}

static int sequence_final(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

static int utf8_continuations(unsigned char byte)
{
    if (byte >= 0xc2 && byte <= 0xdf)
        return 1;
    if (byte >= 0xe0 && byte <= 0xef)
        return 2;
    if (byte >= 0xf0 && byte <= 0xf4)
        return 3;
    return 0;
}

static int amiga_emit_sequence(struct ace_amiga_to_xterm *state,
                               const struct ace_terminal_sink *sink)
{
    static const struct {
        const char *ace;
        const char *xterm;
    } translations[] = {
        { "\233A", "\033[A" }, { "\233B", "\033[B" },
        { "\233C", "\033[C" }, { "\233D", "\033[D" },
        { "\233T", "\033[1;2A" }, { "\233S", "\033[1;2B" },
        { "\233 A", "\033[1;2D" }, { "\233 @", "\033[1;2C" },
        { "\233Z", "\033[Z" }, { "\23340~", "\033[2~" },
        { "\23341~", "\033[5~" }, { "\23342~", "\033[6~" },
        { "\23344~", "\033OH" }, { "\23345~", "\033OF" },
        { "\23350~", "\033[2;2~" }, { "\23354~", "\033[1;2H" },
        { "\23355~", "\033[1;2F" }, { "\2330~", "\033OP" },
        { "\2331~", "\033OQ" }, { "\2332~", "\033OR" },
        { "\2333~", "\033OS" }, { "\2334~", "\033[15~" },
        { "\2335~", "\033[17~" }, { "\2336~", "\033[18~" },
        { "\2337~", "\033[19~" }, { "\2338~", "\033[20~" },
        { "\2339~", "\033[21~" }, { "\23310~", "\033[1;2P" },
        { "\23311~", "\033[1;2Q" }, { "\23312~", "\033[1;2R" },
        { "\23313~", "\033[1;2S" }, { "\23314~", "\033[15;2~" },
        { "\23315~", "\033[17;2~" }, { "\23316~", "\033[18;2~" },
        { "\23317~", "\033[19;2~" }, { "\23318~", "\033[20;2~" },
        { "\23319~", "\033[21;2~" },
    };
    size_t index;
    int resize_report = 0;

    if (state->sequence_length >= 5 && state->sequence[0] == AMIGA_CSI &&
        memcmp(state->sequence + 1, "12;", 3) == 0 &&
        state->sequence[state->sequence_length - 1] == '|') {
        resize_report = state->sequence[4] >= '0' &&
                        state->sequence[4] <= '9';
        for (index = 4; resize_report &&
             index + 1 < state->sequence_length; index++) {
            unsigned char byte = state->sequence[index];

            if ((byte < '0' || byte > '9') && byte != ';')
                resize_report = 0;
        }
    }
    if (resize_report) {
        state->resize_pending = 1;
        state->sequence_length = 0;
        return 0;
    }
    for (index = 0; index < sizeof(translations) / sizeof(translations[0]);
         index++) {
        size_t source_length = strlen(translations[index].ace);

        if (source_length == state->sequence_length &&
            memcmp(state->sequence, translations[index].ace,
                   source_length) == 0) {
            state->sequence_length = 0;
            return emit_text(sink, translations[index].xterm);
        }
    }
    if (emit(sink, state->sequence, state->sequence_length) != 0)
        return -1;
    state->sequence_length = 0;
    return 0;
}

int ace_amiga_to_xterm_feed(struct ace_amiga_to_xterm *state,
                            const struct ace_terminal_sink *sink,
                            const unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned char byte = bytes[index];

        if (state->utf8_continuations) {
            /* U+009B is encoded as C2 9B in UTF-8.  Forwarding the
             * continuation byte here prevents it from being mistaken for
             * a standalone ACE CSI introducer. */
            if (emit(sink, &byte, 1) != 0)
                return -1;
            if (byte >= 0x80 && byte <= 0xbf)
                state->utf8_continuations--;
            else
                state->utf8_continuations = utf8_continuations(byte);
            continue;
        }
        if (state->sequence_length) {
            state->sequence[state->sequence_length++] = byte;
            if (state->sequence_length == sizeof(state->sequence) ||
                sequence_final(byte)) {
                if (amiga_emit_sequence(state, sink) != 0)
                    return -1;
            }
            continue;
        }
        if (byte == AMIGA_CSI) {
            state->sequence[0] = byte;
            state->sequence_length = 1;
            continue;
        }
        state->utf8_continuations = utf8_continuations(byte);
        if (byte == '\b') {
            byte = 0x7f;
        } else if (byte == 0x7f) {
            if (emit_text(sink, "\033[3~") != 0)
                return -1;
            continue;
        }
        if (emit(sink, &byte, 1) != 0)
            return -1;
    }
    return 0;
}

int ace_amiga_to_xterm_flush(struct ace_amiga_to_xterm *state,
                             const struct ace_terminal_sink *sink)
{
    if (!state->sequence_length)
        return 0;
    if (emit(sink, state->sequence, state->sequence_length) != 0)
        return -1;
    state->sequence_length = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Linux program output -> ACE console: colour                         */
/* ------------------------------------------------------------------ */

/*
 * console.device numbers pens, it does not name colours: SGR 30+n selects
 * pen n, and pen 0 is the window background while pen 1 is the text colour.
 * ACE fills the remaining six with accents in the order red, green, blue,
 * yellow, magenta, cyan -- see the palettes in amiga_console.c -- so an
 * ANSI colour has to be renumbered on the way through.
 *
 *   ANSI  0 blk  1 red  2 grn  3 yel  4 blu  5 mag  6 cyn  7 wht
 *   pen   0      2      3      5      4      6      7      1
 */
static const unsigned char ansi_to_pen[8] = { 0, 2, 3, 5, 4, 6, 7, 1 };

/* The sixteen colours xterm renders for SGR 30-37 and 90-97, used to find
 * the closest pen for an indexed or truecolour request. */
static const unsigned char ansi_color_rgb[16][3] = {
    {   0,   0,   0 }, { 170,   0,   0 }, {   0, 170,   0 }, { 170,  85,   0 },
    {   0,   0, 170 }, { 170,   0, 170 }, {   0, 170, 170 }, { 170, 170, 170 },
    {  85,  85,  85 }, { 255,  85,  85 }, {  85, 255,  85 }, { 255, 255,  85 },
    {  85,  85, 255 }, { 255,  85, 255 }, {  85, 255, 255 }, { 255, 255, 255 },
};

/* xterm's 256-colour table: sixteen named colours, a 6x6x6 cube, then a
 * 24-step grey ramp. */
static void xterm_index_rgb(unsigned index, unsigned char *rgb)
{
    static const unsigned char cube_level[6] = { 0, 95, 135, 175, 215, 255 };

    if (index < 16) {
        memcpy(rgb, ansi_color_rgb[index], 3);
        return;
    }
    if (index < 232) {
        unsigned value = index - 16;

        rgb[0] = cube_level[(value / 36) % 6];
        rgb[1] = cube_level[(value / 6) % 6];
        rgb[2] = cube_level[value % 6];
        return;
    }
    rgb[0] = rgb[1] = rgb[2] = (unsigned char)(8 + 10 * (index - 232));
}

/*
 * An eight-pen palette has one neutral axis and six hues, so lightness must
 * not be allowed to answer a question about hue: plain distance calls the
 * dark teal that htop paints a panel with "grey", and grey is the window
 * background. A colour is therefore matched against the four neutrals only
 * when it really is close to grey, and against the twelve hues otherwise.
 */
static unsigned nearest_ansi_color(const unsigned char *rgb)
{
    unsigned char high = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
    unsigned char low = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    unsigned best = 0;
    long best_distance = -1;
    int neutral;
    unsigned index;

    high = high > rgb[2] ? high : rgb[2];
    low = low < rgb[2] ? low : rgb[2];
    neutral = (high - low) * 3 <= high;
    for (index = 0; index < 16; index++) {
        long dr;
        long dg;
        long db;
        long distance;
        /* Black, silver, grey and white are the neutral pens. */
        int candidate_neutral = (index & 7) == 0 || (index & 7) == 7;

        if (candidate_neutral != neutral)
            continue;
        dr = (long)rgb[0] - ansi_color_rgb[index][0];
        dg = (long)rgb[1] - ansi_color_rgb[index][1];
        db = (long)rgb[2] - ansi_color_rgb[index][2];
        distance = dr * dr + dg * dg + db * db;
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return best;
}

/*
 * Record one xterm colour as an Amiga SGR parameter. Bright foregrounds have
 * no pen of their own, so brightness is carried by the console's bold flag
 * instead -- except bright black, which would otherwise select pen 0 and
 * paint dimmed text in the window background colour.
 */
static void select_pen(struct ace_xterm_to_amiga *state, unsigned color,
                       int background, unsigned char *out, size_t *count)
{
    unsigned pen = ansi_to_pen[color & 7];

    if (background) {
        out[(*count)++] = (unsigned char)(40 + pen);
        return;
    }
    if (color == 8)
        pen = 1;
    out[(*count)++] = (unsigned char)(30 + pen);
    state->bright_foreground = color >= 8;
}

/* ------------------------------------------------------------------ */
/* Linux program output -> ACE console: text                           */
/* ------------------------------------------------------------------ */

/*
 * ACE renders each console byte as Latin-1 (see the glyph cache in
 * aros_graphics_runtime.c), so UTF-8 above U+00FF has to be folded to
 * something the font can draw. Line drawing and the geometric shapes
 * ncurses programs use for meters and sort markers get an ASCII stand-in;
 * anything else becomes '?' rather than three bytes of mojibake.
 */
static const struct {
    unsigned long code;
    const char *ascii;
} unicode_folds[] = {
    { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" }, { 0x2013, "-" },
    { 0x2014, "-" }, { 0x2015, "-" }, { 0x2018, "'" }, { 0x2019, "'" },
    { 0x201a, "'" }, { 0x201c, "\"" }, { 0x201d, "\"" }, { 0x201e, "\"" },
    { 0x2022, "*" }, { 0x2026, "..." }, { 0x2030, "%" }, { 0x2039, "<" },
    { 0x203a, ">" }, { 0x2044, "/" }, { 0x20ac, "E" },
    { 0x2190, "<" }, { 0x2191, "^" }, { 0x2192, ">" }, { 0x2193, "v" },
    { 0x2500, "-" }, { 0x2501, "-" }, { 0x2502, "|" }, { 0x2503, "|" },
    { 0x2571, "/" }, { 0x2572, "\\" },
    { 0x25b2, "^" }, { 0x25b3, "^" }, { 0x25b6, ">" }, { 0x25b7, ">" },
    { 0x25ba, ">" }, { 0x25bc, "v" }, { 0x25bd, "v" }, { 0x25c0, "<" },
    { 0x25c1, "<" }, { 0x25c4, "<" },
    { 0x2713, "v" }, { 0x2714, "v" }, { 0x2717, "x" }, { 0x2718, "x" },
    { 0x27a4, ">" },
};

static const char *fold_unicode(unsigned long code)
{
    size_t index;

    for (index = 0; index < sizeof(unicode_folds) / sizeof(unicode_folds[0]);
         index++) {
        if (unicode_folds[index].code == code)
            return unicode_folds[index].ascii;
    }
    /* Box drawing and block elements are structural: a wrong-looking frame
     * still reads as a frame, a '?' does not. */
    if (code >= 0x2500 && code <= 0x257f)
        return "+";
    if (code >= 0x2580 && code <= 0x2590)
        return "#";
    if (code >= 0x2591 && code <= 0x2593)
        return ":";
    if (code >= 0x2594 && code <= 0x259f)
        return "#";
    if (code >= 0x25a0 && code <= 0x25ff)
        return "*";
    if (code >= 0x2190 && code <= 0x21ff)
        return ">";
    return "?";
}

static int put_char(struct ace_xterm_to_amiga *state,
                    const struct ace_terminal_sink *sink, unsigned char byte)
{
    if ((byte >= 0x20 && byte < 0x7f) || byte >= 0xa0)
        state->last_graphic = byte;
    return emit(sink, &byte, 1);
}

static int put_text(struct ace_xterm_to_amiga *state,
                    const struct ace_terminal_sink *sink, const char *text)
{
    while (*text) {
        if (put_char(state, sink, (unsigned char)*text++) != 0)
            return -1;
    }
    return 0;
}

/* Emit the accumulated UTF-8 character as Latin-1 or an ASCII stand-in. */
static int put_unicode(struct ace_xterm_to_amiga *state,
                       const struct ace_terminal_sink *sink)
{
    static const unsigned char lead_mask[4] = { 0x7f, 0x1f, 0x0f, 0x07 };
    unsigned long code = state->utf8[0] & lead_mask[state->utf8_needed];
    size_t index;

    for (index = 1; index <= state->utf8_needed; index++)
        code = (code << 6) | (state->utf8[index] & 0x3f);
    state->utf8_length = 0;
    state->utf8_needed = 0;
    /* 0x80-0x9F are the console's own C1 controls -- 0x9B is its CSI -- so
     * that range must never be reached by folding text. */
    if (code >= 0xa0 && code <= 0xff)
        return put_char(state, sink, (unsigned char)code);
    return put_text(state, sink, fold_unicode(code));
}

/* Give back a partial or malformed UTF-8 run untouched: a lone 0x9B from a
 * program that already speaks to the Amiga console must stay a CSI. */
static int put_raw_utf8(struct ace_xterm_to_amiga *state,
                        const struct ace_terminal_sink *sink)
{
    size_t index;

    for (index = 0; index < state->utf8_length; index++) {
        if (emit(sink, &state->utf8[index], 1) != 0)
            return -1;
    }
    state->utf8_length = 0;
    state->utf8_needed = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Linux program output -> ACE console: control sequences              */
/* ------------------------------------------------------------------ */

/* Parse the parameter bytes of a CSI. Omitted parameters read back as -1 so
 * a caller can tell "CSI H" from "CSI 1;1H"; ':' separates the parts of a
 * T.416 colour and is treated like ';'. */
static size_t parse_params(const unsigned char *bytes, size_t length,
                           int *params, size_t maximum)
{
    size_t count = 0;
    size_t index = 0;
    int value = -1;

    while (index <= length && count < maximum) {
        if (index == length || bytes[index] == ';' || bytes[index] == ':') {
            params[count++] = value;
            value = -1;
            if (index == length)
                break;
        } else if (bytes[index] >= '0' && bytes[index] <= '9') {
            value = (value < 0 ? 0 : value) * 10 + (bytes[index] - '0');
            if (value > 65535)
                value = 65535;
        }
        index++;
    }
    return count;
}

static unsigned param_or(const int *params, size_t count, size_t index,
                         unsigned fallback)
{
    if (index >= count || params[index] < 0)
        return fallback;
    return (unsigned)params[index];
}

/* console.device counts in a UBYTE and treats a zero count as no movement,
 * while xterm reads an omitted or zero parameter as one. */
static unsigned movement_count(const int *params, size_t count, size_t index)
{
    unsigned value = param_or(params, count, index, 1);

    if (value < 1)
        value = 1;
    if (value > AMIGA_PARAM_MAX)
        value = AMIGA_PARAM_MAX;
    return value;
}

static unsigned repeat_count(const int *params, size_t count, size_t index)
{
    unsigned value = movement_count(params, count, index);

    return value > ACE_TERMINAL_REPEAT_MAX ? ACE_TERMINAL_REPEAT_MAX : value;
}

static int flush_sgr(const struct ace_terminal_sink *sink,
                     const unsigned char *out, size_t count)
{
    char sequence[4 * AMIGA_SGR_PARAM_MAX + 2];
    size_t index;
    int length = 0;

    if (!count)
        return 0;
    sequence[length++] = AMIGA_CSI;
    for (index = 0; index < count; index++) {
        length += snprintf(sequence + length, sizeof(sequence) - (size_t)length,
                           "%s%u", index ? ";" : "", out[index]);
    }
    sequence[length++] = 'm';
    return emit(sink, sequence, (size_t)length);
}

/*
 * Translate one xterm SGR into the console's own rendition command. The
 * console understands SGR 0/3/4/7/8 and the pen selections; everything else
 * -- faint, blink, strike-through, the 256-colour and truecolour forms -- is
 * either reduced to a pen or dropped, because an unknown CSI is printed.
 */
static int translate_sgr(struct ace_xterm_to_amiga *state,
                         const struct ace_terminal_sink *sink,
                         const int *params, size_t count)
{
    unsigned char out[AMIGA_SGR_PARAM_MAX];
    size_t out_count = 0;
    size_t index;
    int desired_bold;

    /* A parameterless SGR is a reset in xterm, but the console's parser sees
     * no parameters and would change nothing. */
    if (!count) {
        state->bold_requested = state->bright_foreground = 0;
        state->bold_emitted = 0;
        return emit_csi(sink, 0, 'm');
    }
    for (index = 0; index < count; index++) {
        int param = params[index] < 0 ? 0 : params[index];

        if (out_count + 2 > AMIGA_SGR_PARAM_MAX) {
            if (flush_sgr(sink, out, out_count) != 0)
                return -1;
            out_count = 0;
        }
        switch (param) {
        case 0:
            state->bold_requested = state->bright_foreground = 0;
            state->bold_emitted = 0;
            out[out_count++] = 0;
            break;
        /* Bold is emitted below, once, as the union of the two ways xterm
           can ask for it. */
        case 1:
            state->bold_requested = 1;
            break;
        case 22:
            state->bold_requested = 0;
            break;
        case 3:
        case 4:
        case 7:
        case 8:
        case 23:
        case 24:
        case 27:
        case 28:
            out[out_count++] = (unsigned char)param;
            break;
        case 39:
            state->bright_foreground = 0;
            out[out_count++] = 39;
            break;
        case 49:
            out[out_count++] = 49;
            break;
        case 38:
        case 48: {
            int background = param == 48;
            unsigned selector = param_or(params, count, index + 1, 0);
            unsigned char rgb[3];

            if (selector == 5) {
                unsigned value = param_or(params, count, index + 2, 0);

                xterm_index_rgb(value > 255 ? 255 : value, rgb);
                index += 2;
            } else if (selector == 2) {
                /* T.416 allows an empty colour-space slot: 38:2::R:G:B. */
                size_t first = index + 2;

                if (first < count && params[first] < 0)
                    first++;
                rgb[0] = (unsigned char)param_or(params, count, first, 0);
                rgb[1] = (unsigned char)param_or(params, count, first + 1, 0);
                rgb[2] = (unsigned char)param_or(params, count, first + 2, 0);
                index = first + 2;
            } else {
                break;
            }
            select_pen(state, nearest_ansi_color(rgb), background, out,
                       &out_count);
            break;
        }
        default:
            if (param >= 30 && param <= 37)
                select_pen(state, (unsigned)param - 30, 0, out, &out_count);
            else if (param >= 40 && param <= 47)
                select_pen(state, (unsigned)param - 40, 1, out, &out_count);
            else if (param >= 90 && param <= 97)
                select_pen(state, (unsigned)param - 90 + 8, 0, out, &out_count);
            else if (param >= 100 && param <= 107)
                select_pen(state, (unsigned)param - 100 + 8, 1, out,
                           &out_count);
            break;
        }
    }
    desired_bold = state->bold_requested || state->bright_foreground;
    if (desired_bold != state->bold_emitted) {
        if (out_count == AMIGA_SGR_PARAM_MAX) {
            if (flush_sgr(sink, out, out_count) != 0)
                return -1;
            out_count = 0;
        }
        out[out_count++] = desired_bold ? 1 : 22;
        state->bold_emitted = desired_bold;
    }
    return flush_sgr(sink, out, out_count);
}

/* DEC private modes. Only the three the console can actually honour are
 * translated; the rest -- mouse reporting, bracketed paste, cursor blink --
 * are state ACE cannot render and must not print. */
static int translate_private_mode(const struct ace_terminal_sink *sink,
                                  const int *params, size_t count, int set)
{
    size_t index;

    for (index = 0; index < count; index++) {
        switch (params[index]) {
        case 47:
        case 1047:
        case 1049:
            /* Entering or leaving the alternate screen: the console has one
               screen, so both ends of the swap start from a clean window. */
            if (emit_text(sink, "\2331;1H\233J") != 0)
                return -1;
            break;
        case 25:
            if (emit_text(sink, set ? "\2331 p" : "\2330 p") != 0)
                return -1;
            break;
        case 7:
            if (emit_text(sink, set ? "\233>7h" : "\233>7l") != 0)
                return -1;
            break;
        default:
            break;
        }
    }
    return 0;
}

/*
 * Erase n characters from the cursor without moving it. console.device has
 * no such command, so the cells are overwritten with blanks and the cursor
 * walked back. Both the write and the walk wrap the same way, so the pair
 * stays exact even when the run reaches the right margin.
 */
static int translate_erase_chars(const struct ace_terminal_sink *sink,
                                 unsigned count)
{
    unsigned remaining = count;

    while (remaining--) {
        if (emit_text(sink, " ") != 0)
            return -1;
    }
    return emit_csi(sink, count, 'D');
}

static int translate_csi(struct ace_xterm_to_amiga *state,
                         const struct ace_terminal_sink *sink)
{
    const unsigned char *sequence = state->sequence;
    size_t length = state->sequence_length;
    unsigned char final = sequence[length - 1];
    const unsigned char *body = sequence + 2;
    size_t body_length = length - 3;
    int params[ACE_TERMINAL_PARAM_MAX];
    size_t count;
    size_t index;

    /* A private introducer (?, <, =, >) or an intermediate byte marks an
     * xterm-only command; none of them exist on the Amiga side. */
    if (body_length && body[0] >= 0x3c && body[0] <= 0x3f) {
        count = parse_params(body + 1, body_length - 1, params,
                             ACE_TERMINAL_PARAM_MAX);
        if (body[0] == '?' && (final == 'h' || final == 'l'))
            return translate_private_mode(sink, params, count,
                                          final == 'h');
        return 0;
    }
    for (index = 0; index < body_length; index++) {
        if (body[index] >= 0x20 && body[index] <= 0x2f)
            return 0;
    }
    count = parse_params(body, body_length, params, ACE_TERMINAL_PARAM_MAX);

    switch (final) {
    case 'm':
        return translate_sgr(state, sink, params, count);
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'E':
    case 'F':
    case 'I':
    case 'L':
    case 'M':
    case 'S':
    case 'T':
    case 'Z':
        return emit_csi(sink, movement_count(params, count, 0), (char)final);
    case 'H':
    case 'f':
        /* The console fills an omitted CUP parameter with the current
           position where xterm uses the home corner, so both are spelled
           out here. */
        return emit_csi_pair(sink, movement_count(params, count, 0),
                             movement_count(params, count, 1), 'H');
    case 'd':
        /* Row only: the console's default column is the current one, which
           is exactly what VPA means. */
        return emit_csi(sink, movement_count(params, count, 0), 'H');
    case 'G':
    case '`': {
        /* No absolute-column command exists, but returning to the margin
           and stepping forward is the same move. */
        unsigned column = movement_count(params, count, 0);

        if (emit_text(sink, "\r") != 0)
            return -1;
        if (column < 2)
            return 0;
        return emit_csi(sink, column - 1, 'C');
    }
    case 'J':
        /* The console erases from the cursor whatever parameter it is
           given, so only that form and the full clear can be honoured. */
        if (param_or(params, count, 0, 0) == 2)
            return emit_text(sink, "\2331;1H\233J");
        if (param_or(params, count, 0, 0) != 0)
            return 0;
        return emit_text(sink, "\233J");
    case 'K':
        if (param_or(params, count, 0, 0) == 2)
            return emit_text(sink, "\r\233K");
        if (param_or(params, count, 0, 0) != 0)
            return 0;
        return emit_text(sink, "\233K");
    case 'P':
        /* The console's delete- and insert-character commands ignore their
           count, so the count is spent here instead. */
        return emit_repeat(sink, "\233P", repeat_count(params, count, 0));
    case '@':
        return emit_repeat(sink, "\233@", repeat_count(params, count, 0));
    case 'X':
        return translate_erase_chars(sink, repeat_count(params, count, 0));
    case 'b': {
        unsigned repeats = repeat_count(params, count, 0);

        if (!state->last_graphic)
            return 0;
        while (repeats--) {
            if (emit(sink, &state->last_graphic, 1) != 0)
                return -1;
        }
        return 0;
    }
    case 'n':
        if (param_or(params, count, 0, 0) == 6)
            return emit_text(sink, "\2336n");
        return 0;
    default:
        /* Scrolling regions, cursor save/restore, window manipulation and
           device attributes have no console equivalent. Several of their
           final bytes -- 't' and 'u' among them -- mean something else
           entirely to console.device, so dropping them is not merely
           tidier, it prevents a misread command. */
        return 0;
    }
}

static int translate_escape(struct ace_xterm_to_amiga *state,
                            const struct ace_terminal_sink *sink)
{
    const unsigned char *sequence = state->sequence;
    size_t length = state->sequence_length;

    if (length == 2) {
        switch (sequence[1]) {
        case 'D':
            return emit_text(sink, "\204");  /* IND  */
        case 'E':
            return emit_text(sink, "\205");  /* NEL  */
        case 'H':
            return emit_text(sink, "\210");  /* HTS  */
        case 'M':
            return emit_text(sink, "\215");  /* RI   */
        case 'c':
            state->bold_requested = state->bright_foreground = 0;
            state->bold_emitted = 0;
            return emit_text(sink, "\2330m\2331;1H\233J");
        default:
            /* Keypad and cursor-key modes, save/restore cursor and the
               single-shift codes are xterm state ACE does not keep. */
            return 0;
        }
    }
    /* Character-set designation. ACE has no charset mode, so the choice is
       metadata and must not become visible prompt text. */
    return 0;
}

static int finish_escape(struct ace_xterm_to_amiga *state,
                         const struct ace_terminal_sink *sink)
{
    int status = translate_escape(state, sink);

    state->sequence_length = 0;
    state->kind = XTERM_NONE;
    return status;
}

static int feed_output_byte(struct ace_xterm_to_amiga *state,
                            const struct ace_terminal_sink *sink,
                            unsigned char byte)
{
    if (state->kind == XTERM_OSC || state->kind == XTERM_OSC_ESCAPE) {
        /* OSC is normally terminated by BEL; xterm also accepts ST
           (ESC backslash).  Window-title OSC 0 is presentation metadata,
           not text for the Amiga console. */
        if (state->kind == XTERM_OSC_ESCAPE) {
            if (byte == '\\' || byte == 0x9c)
                state->kind = XTERM_NONE;
            else if (byte != '\033')
                state->kind = XTERM_OSC;
        } else if (byte == '\007' || byte == 0x9c) {
            state->kind = XTERM_NONE;
        } else if (byte == '\033') {
            state->kind = XTERM_OSC_ESCAPE;
        }
        return 0;
    }

    if (state->utf8_needed) {
        if (byte >= 0x80 && byte <= 0xbf) {
            state->utf8[state->utf8_length++] = byte;
            if (state->utf8_length == state->utf8_needed + 1)
                return put_unicode(state, sink);
            return 0;
        }
        if (put_raw_utf8(state, sink) != 0)
            return -1;
    }

    if (state->sequence_length) {
        state->sequence[state->sequence_length++] = byte;
        if (state->kind == XTERM_ESCAPE && state->sequence_length == 2) {
            if (byte == '[') {
                state->kind = XTERM_CSI;
                return 0;
            }
            if (byte == ']') {
                state->sequence_length = 0;
                state->kind = XTERM_OSC;
                return 0;
            }
            if (byte != '(' && byte != ')' && byte != '#' && byte != '%')
                return finish_escape(state, sink);
            return 0;
        }
        if (state->kind == XTERM_ESCAPE && state->sequence_length == 3)
            return finish_escape(state, sink);
        if (state->kind == XTERM_CSI && state->sequence_length > 2 &&
            sequence_final(byte)) {
            int status = translate_csi(state, sink);

            state->sequence_length = 0;
            state->kind = XTERM_NONE;
            return status;
        }
        /* An overlong sequence is malformed, not text: printing it would
           spray the parameters across the window. */
        if (state->sequence_length == sizeof(state->sequence)) {
            state->sequence_length = 0;
            state->kind = XTERM_NONE;
        }
        return 0;
    }

    if (byte == '\033') {
        state->sequence[0] = byte;
        state->sequence_length = 1;
        state->kind = XTERM_ESCAPE;
        return 0;
    }
    if (utf8_continuations(byte)) {
        state->utf8[0] = byte;
        state->utf8_length = 1;
        state->utf8_needed = (size_t)utf8_continuations(byte);
        return 0;
    }
    return put_char(state, sink, byte);
}

void ace_xterm_to_amiga_init(struct ace_xterm_to_amiga *state)
{
    memset(state, 0, sizeof(*state));
}

size_t ace_xterm_to_amiga_feed(struct ace_xterm_to_amiga *state,
                               const struct ace_terminal_sink *sink,
                               const unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        /* One erase or repeat can fill most of a line, so the room for it
           is checked before the byte that asks for it is consumed. */
        if (sink->space(sink->context) < ACE_TERMINAL_EMIT_MAX)
            return index;
        if (feed_output_byte(state, sink, bytes[index]) != 0)
            return index;
    }
    return length;
}

int ace_xterm_to_amiga_flush(struct ace_xterm_to_amiga *state,
                             const struct ace_terminal_sink *sink)
{
    int status = 0;

    if (state->utf8_length)
        status = put_raw_utf8(state, sink);
    /* An unterminated OSC is metadata too.  Dropping it avoids exposing a
     * partial title such as "]0;hostname" when the Linux child exits. */
    if (state->sequence_length && state->kind != XTERM_OSC &&
        state->kind != XTERM_OSC_ESCAPE)
        status = emit(sink, state->sequence, state->sequence_length);
    state->sequence_length = 0;
    state->kind = XTERM_NONE;
    return status;
}
