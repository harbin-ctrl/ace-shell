/*
 * ACE's graphics.library: the RastPort/BitMap/Text() surface the real AROS
 * console classes (rom/devs/console/{stdconclass,consoleclass}.c) draw
 * through.
 *
 * This file is deliberately authored, not compiled from AROS source. Every
 * other seam in ACE exists to run more real AROS code; this one is the
 * hardware boundary itself -- the point past which "real AROS code" would
 * mean a HIDD driver stack talking to a framebuffer, which is not what ACE
 * wants to become. What AROS's console classes require of graphics.library
 * is ten drawing calls plus a scratch-buffer trio for the character-cell
 * cursor; the shapes and semantics below come from reading those calls'
 * real implementations in rom/graphics, not from guessing.
 *
 * Fonts are the one place ACE improves on real Amiga hardware rather than
 * emulating it faithfully. Real console.device only ever reads three scalars
 * off a TextFont -- tf_XSize, tf_YSize, tf_Baseline -- to lay out a fixed
 * character-cell grid ("For now one should use only non-proportional
 * fonts", consoleclass.c's own words); it never reads tf_CharData or
 * tf_CharLoc in this codepath. So the glyphs themselves are free to come
 * from a host TrueType font chosen by the user rather than a bitmap font
 * ACE would have to draw and ship. SetSoftStyle()'s bold/italic requests
 * are rendered with the font's own real bold/italic faces rather than
 * synthesized by smearing or shearing a single face the way real Amiga
 * hardware did -- ace_gfx_font_family_complete() is what enforces that a
 * chosen family actually has those faces before it can be selected.
 * Underline is drawn as a rule below the baseline either way: it is a
 * soft-style flag on real hardware too, never a font glyph.
 */

#include "aros_graphics_runtime.h"

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <fontconfig/fontconfig.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <proto/arossupport.h>

#include "ace_graphics_intern.h"

/* -------------------------------------------------------------------------
 * Fonts
 *
 * struct TextFont is a real, publicly laid-out AROS struct -- callers
 * dereference rp->Font->tf_XSize directly -- so it cannot carry ACE-private
 * fields itself. ace_gfx_font_private embeds it as the first member instead,
 * the same "public struct as the head of a private one" shape
 * aros_boopsi_runtime.c already uses for pool blocks: a struct TextFont*
 * handed to a caller is safely cast back to its owning private struct
 * because the two addresses coincide.
 * ---------------------------------------------------------------------- */

#define ACE_GFX_STYLE_REGULAR     0
#define ACE_GFX_STYLE_BOLD        1
#define ACE_GFX_STYLE_ITALIC      2
#define ACE_GFX_STYLE_BOLD_ITALIC 3

/* A resolved cell is the information needed to redraw a character without
 * touching its already-antialiased pixels. The real console's cursor is a
 * full-cell COMPLEMENT operation, so the host seam has to retain the logical
 * character that produced that cell instead of trying to reverse-engineer it
 * from the cairo surface. */
struct ace_gfx_cell {
    UBYTE character;
    UBYTE algo_style;
    UBYTE opaque;
    UBYTE valid;
    /* These are Amiga pen numbers, not resolved RGB values. Complement on
       the original bitplanes was a pen-index XOR; retaining that operation
       is what keeps the cursor inside the active eight-colour palette. */
    UBYTE foreground_pen;
    UBYTE background_pen;
};

/*
 * Glyphs are looked up once per style and then drawn through
 * cairo_show_glyphs() rather than cairo_show_text(). The toy text API
 * re-runs UTF-8 decoding and a FreeType character-map lookup on every call;
 * a console redraws the same 96 ASCII codes millions of times, so the
 * mapping is cached at font-load time instead. Drawing from explicit glyph
 * indices also lets Text() place every glyph on the console's own integer
 * cell pitch instead of letting the font's fractional advance accumulate
 * across a run.
 *
 * Bytes are mapped to code points as Latin-1: console.device hands
 * graphics.library raw bytes, and tf_LoChar/tf_HiChar below already declare
 * 32..255 as the covered range.
 */
#define ACE_GFX_GLYPH_LO 0x20
#define ACE_GFX_GLYPH_HI 0xff

struct ace_gfx_font_private {
    struct TextFont tf;
    char family[128];
    int pixel_size;
    cairo_font_face_t *face[4];
    cairo_scaled_font_t *scaled[4];
    unsigned long glyph[4][ACE_GFX_GLYPH_HI + 1];
    int glyph_cached[4];
};

/* Is this the name of a family that is actually installed, rather than one of
   fontconfig's generic aliases? */
static int family_is_installed(const char *family)
{
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *properties;
    FcFontSet *fonts;
    int installed = 0;

    if (!pattern)
        return 0;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    properties = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
    if (properties) {
        fonts = FcFontList(NULL, pattern, properties);
        if (fonts) {
            /* FcFontList does not substitute, so a result here means a font
               really carries this family name. */
            installed = fonts->nfont > 0;
            FcFontSetDestroy(fonts);
        }
        FcObjectSetDestroy(properties);
    }
    FcPatternDestroy(pattern);
    return installed;
}

/*
 * "monospace", "sans-serif" and the rest are not families; they are names
 * fontconfig resolves to whichever family the system has chosen for that
 * role, and resolving to a differently named family is them working, not
 * failing. A font chooser offers them alongside real families -- so does
 * ACE's own list of fallbacks -- and load_face() below rejects any match
 * whose family is not the one asked for, which is right for a real family
 * and wrong for one of these.
 *
 * So an alias is turned into the family it names before anything else looks
 * at it, and everything downstream sees an ordinary family.
 *
 * Only these names, and only when nothing is installed under them. Asking
 * fontconfig to match any unknown name would answer with the default font
 * just as readily, and accepting that would turn a misspelled family into a
 * silent substitution -- which is the thing load_face() was written to stop.
 */
static int family_is_generic(const char *family)
{
    static const char *const generics[] = {
        "monospace", "mono", "sans-serif", "sans serif", "sans", "serif",
        "cursive", "fantasy", "system-ui", "math", "emoji",
    };

    for (size_t i = 0; i < sizeof(generics) / sizeof(generics[0]); i++)
        if (strcasecmp(family, generics[i]) == 0)
            return 1;
    return 0;
}

static int resolve_family_alias(const char *family, char *result,
                                size_t result_size)
{
    FcPattern *pattern;
    FcPattern *matched;
    FcResult status;
    FcChar8 *resolved = NULL;
    int copied = -1;

    if (!family || !*family)
        return -1;
    if (family_is_installed(family)) {
        if (strlen(family) >= result_size)
            return -1;
        strcpy(result, family);
        return 0;
    }
    if (!family_is_generic(family))
        return -1;
    pattern = FcPatternCreate();
    if (!pattern)
        return -1;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    matched = FcFontMatch(NULL, pattern, &status);
    FcPatternDestroy(pattern);
    if (!matched)
        return -1;
    if (FcPatternGetString(matched, FC_FAMILY, 0, &resolved) == FcResultMatch &&
        resolved && strlen((const char *)resolved) < result_size) {
        strcpy(result, (const char *)resolved);
        copied = 0;
    }
    FcPatternDestroy(matched);
    return copied;
}

int ace_gfx_weight_from_css(int css_weight)
{
    if (css_weight <= 0)
        return 0;
    return FcWeightFromOpenType(css_weight);
}

static cairo_font_face_t *load_face(const char *family, int weight, int bold,
                                    int italic)
{
    FcPattern *pattern;
    FcPattern *matched;
    FcResult result;
    cairo_font_face_t *face;
    int wanted;

    if (weight <= 0)
        weight = FC_WEIGHT_REGULAR;
    /* Bold is bold whatever the base weight is: choosing Medium as the body
       face asks for a lighter *unemphasised* face, not a lighter bold one.
       A base already at or above bold keeps its own weight so that asking
       for a Black family does not quietly get thinned to bold. */
    wanted = bold ? (weight > FC_WEIGHT_BOLD ? weight : FC_WEIGHT_BOLD)
                  : weight;

    pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pattern, FC_SLANT,
                        italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddInteger(pattern, FC_WEIGHT, wanted);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    matched = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (!matched)
        return NULL;

    /*
     * FcFontMatch() always returns *something* -- fontconfig substitutes a
     * fallback family rather than failing. A real bold/italic face for the
     * requested family is confirmed by checking the matched pattern named
     * the family we asked for and the style we asked for, not by assuming
     * a match means what was asked for was found.
     */
    {
        FcChar8 *matched_family = NULL;
        int matched_slant = FC_SLANT_ROMAN;
        int matched_weight = FC_WEIGHT_REGULAR;

        FcPatternGetString(matched, FC_FAMILY, 0, &matched_family);
        FcPatternGetInteger(matched, FC_SLANT, 0, &matched_slant);
        FcPatternGetInteger(matched, FC_WEIGHT, 0, &matched_weight);

        if (!matched_family || strcasecmp((const char *)matched_family, family) != 0 ||
            (italic && matched_slant == FC_SLANT_ROMAN) ||
            (bold && matched_weight < FC_WEIGHT_BOLD) ||
            (!bold && matched_weight != wanted)) {
            FcPatternDestroy(matched);
            return NULL;
        }
    }

    face = cairo_ft_font_face_create_for_pattern(matched);
    FcPatternDestroy(matched);
    if (cairo_font_face_status(face) != CAIRO_STATUS_SUCCESS) {
        cairo_font_face_destroy(face);
        return NULL;
    }
    return face;
}

int ace_gfx_font_family_complete(const char *family, int weight)
{
    cairo_font_face_t *faces[4];
    char resolved[128];
    int i;
    int complete = 1;

    if (!family || resolve_family_alias(family, resolved, sizeof(resolved)) != 0)
        return 0;
    family = resolved;
    faces[ACE_GFX_STYLE_REGULAR] = load_face(family, weight, 0, 0);
    faces[ACE_GFX_STYLE_BOLD] = load_face(family, weight, 1, 0);
    faces[ACE_GFX_STYLE_ITALIC] = load_face(family, weight, 0, 1);
    faces[ACE_GFX_STYLE_BOLD_ITALIC] = load_face(family, weight, 1, 1);

    for (i = 0; i < 4; i++) {
        if (!faces[i])
            complete = 0;
        else
            cairo_font_face_destroy(faces[i]);
    }
    return complete;
}

/*
 * One scaled font per style, built once. cairo_set_scaled_font() then costs
 * a reference compare, where cairo_set_font_face()/cairo_set_font_size() per
 * call made cairo re-resolve the scaled font out of its global cache on
 * every run of text.
 */
static int create_scaled_fonts(struct ace_gfx_font_private *priv)
{
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_matrix_t font_matrix;
    cairo_matrix_t ctm;
    int i;
    int ok = 1;

    /*
     * Metric hinting is left on: it snaps the advance width to a whole
     * pixel, which is exactly the fixed cell pitch consoleclass.c lays the
     * grid out on, so hinted glyphs land on the same integer columns Text()
     * positions them at.
     */
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_matrix_init_scale(&font_matrix, priv->pixel_size, priv->pixel_size);
    cairo_matrix_init_identity(&ctm);

    for (i = 0; i < 4; i++) {
        priv->scaled[i] = cairo_scaled_font_create(priv->face[i], &font_matrix,
                                                   &ctm, options);
        if (!priv->scaled[i] ||
            cairo_scaled_font_status(priv->scaled[i]) != CAIRO_STATUS_SUCCESS)
            ok = 0;
    }
    cairo_font_options_destroy(options);
    return ok;
}

static int utf8_from_latin1(unsigned code, char *out)
{
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    out[0] = (char)(0xc0 | (code >> 6));
    out[1] = (char)(0x80 | (code & 0x3f));
    return 2;
}

static void ensure_glyph_cache(struct ace_gfx_font_private *priv, int style)
{
    unsigned code;

    if (priv->glyph_cached[style])
        return;
    priv->glyph_cached[style] = 1;

    for (code = ACE_GFX_GLYPH_LO; code <= ACE_GFX_GLYPH_HI; code++) {
        char utf8[4];
        int length = utf8_from_latin1(code, utf8);
        cairo_glyph_t *glyphs = NULL;
        int count = 0;

        if (cairo_scaled_font_text_to_glyphs(priv->scaled[style], 0.0, 0.0,
                                             utf8, length, &glyphs, &count,
                                             NULL, NULL, NULL)
                == CAIRO_STATUS_SUCCESS && count == 1)
            priv->glyph[style][code] = glyphs[0].index;
        cairo_glyph_free(glyphs);
    }
}

struct TextFont *ace_gfx_load_font(const struct ace_gfx_font_choice *choice,
                                   const char **reason_out)
{
    struct ace_gfx_font_private *priv;
    cairo_font_extents_t extents;
    cairo_text_extents_t cell_extents;
    char resolved[128];

    if (!choice || !choice->family || choice->pixel_size <= 0) {
        if (reason_out)
            *reason_out = "invalid font choice";
        return NULL;
    }

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        if (reason_out)
            *reason_out = "out of memory";
        return NULL;
    }

    /* Resolved for loading, but not recorded: the name the caller chose is
       what goes back into the config, so "Monospace" keeps meaning whatever
       this system calls its monospace font rather than freezing into the
       family it happens to be today. */
    if (resolve_family_alias(choice->family, resolved, sizeof(resolved)) != 0) {
        if (reason_out)
            *reason_out = "no font family of that name is installed";
        free(priv);
        return NULL;
    }
    priv->face[ACE_GFX_STYLE_REGULAR] = load_face(resolved, choice->weight, 0, 0);
    priv->face[ACE_GFX_STYLE_BOLD] = load_face(resolved, choice->weight, 1, 0);
    priv->face[ACE_GFX_STYLE_ITALIC] = load_face(resolved, choice->weight, 0, 1);
    priv->face[ACE_GFX_STYLE_BOLD_ITALIC] = load_face(resolved, choice->weight, 1, 1);

    if (!priv->face[ACE_GFX_STYLE_REGULAR] || !priv->face[ACE_GFX_STYLE_BOLD] ||
        !priv->face[ACE_GFX_STYLE_ITALIC] || !priv->face[ACE_GFX_STYLE_BOLD_ITALIC]) {
        if (reason_out)
            *reason_out = "family is missing a regular, bold, italic or "
                         "bold-italic face";
        ace_gfx_unload_font(&priv->tf);
        return NULL;
    }

    strncpy(priv->family, choice->family, sizeof(priv->family) - 1);
    priv->pixel_size = choice->pixel_size;

    if (!create_scaled_fonts(priv)) {
        if (reason_out)
            *reason_out = "font could not be scaled to the requested size";
        ace_gfx_unload_font(&priv->tf);
        return NULL;
    }

    /*
     * Measured, not guessed: tf_XSize/tf_YSize/tf_Baseline are what
     * consoleclass.c reads to set the fixed character-cell pitch
     * (cu_XRSize/cu_YRSize) the whole console grid is built on, so they have
     * to be the real advance width and line metrics of the chosen face at
     * the chosen size, not a nominal point size. Measuring through the same
     * scaled font Text() draws with keeps the declared cell and the rendered
     * glyph on identical metrics.
     */
    cairo_scaled_font_extents(priv->scaled[ACE_GFX_STYLE_REGULAR], &extents);
    cairo_scaled_font_text_extents(priv->scaled[ACE_GFX_STYLE_REGULAR], "M",
                                   &cell_extents);

    priv->tf.tf_Message.mn_Node.ln_Type = 0; /* NT_FONT, unused downstream */
    priv->tf.tf_Message.mn_Node.ln_Name = priv->family;
    priv->tf.tf_YSize = (UWORD)(extents.height + 0.5);
    priv->tf.tf_Style = 0;
    priv->tf.tf_Flags = 0;
    priv->tf.tf_XSize = (UWORD)(cell_extents.x_advance + 0.5);
    priv->tf.tf_Baseline = (UWORD)(extents.ascent + 0.5);
    priv->tf.tf_BoldSmear = 0; /* real faces are used instead of smearing */
    priv->tf.tf_Accessors = 0;
    priv->tf.tf_LoChar = 32;
    priv->tf.tf_HiChar = 255;
    priv->tf.tf_CharData = NULL; /* unread by this codepath; see file header */
    priv->tf.tf_Modulo = 0;
    priv->tf.tf_CharLoc = NULL;
    priv->tf.tf_CharSpace = NULL;
    priv->tf.tf_CharKern = NULL;

    if (priv->tf.tf_XSize == 0 || priv->tf.tf_YSize == 0) {
        if (reason_out)
            *reason_out = "font measured to a zero-sized cell";
        ace_gfx_unload_font(&priv->tf);
        return NULL;
    }

    return &priv->tf;
}

void ace_gfx_unload_font(struct TextFont *font)
{
    struct ace_gfx_font_private *priv;
    int i;

    if (!font)
        return;
    priv = (struct ace_gfx_font_private *)font;
    for (i = 0; i < 4; i++) {
        if (priv->scaled[i])
            cairo_scaled_font_destroy(priv->scaled[i]);
        if (priv->face[i])
            cairo_font_face_destroy(priv->face[i]);
    }
    free(priv);
}

static int style_index(UBYTE algo_style)
{
    int bold = (algo_style & FSF_BOLD) != 0;
    int italic = (algo_style & FSF_ITALIC) != 0;

    if (bold && italic)
        return ACE_GFX_STYLE_BOLD_ITALIC;
    if (bold)
        return ACE_GFX_STYLE_BOLD;
    if (italic)
        return ACE_GFX_STYLE_ITALIC;
    return ACE_GFX_STYLE_REGULAR;
}

/* -------------------------------------------------------------------------
 * RastPort / BitMap
 *
 * Nothing in the console codepath ACE compiles against this seam reads
 * struct BitMap's Planes[] -- confirmed by checking every call site in
 * rom/devs/console before writing this file -- so the BitMap exists only for
 * structural completeness (rp->BitMap is expected to be non-NULL) and the
 * real pixel storage lives on the RastPort's private context instead.
 *
 * struct RastPort reserves exactly one field for this: RP_Extra, present in
 * the real struct for private per-instance extension space. That is what it
 * is used for here, rather than repurposing a field with real meaning.
 * ---------------------------------------------------------------------- */

/*
 * The pixel buffer is addressed directly for every operation that moves or
 * fills whole spans -- RectFill, ScrollRaster, the COMPLEMENT cursor, and
 * the opaque cell background under text. Those are rectangle copies and
 * solid fills; routing them through cairo meant a pixman composite (and, in
 * ScrollRaster's case, a whole second surface) for work a memmove already
 * does. Cairo still draws every glyph, which is the part that needs it.
 *
 * Mixing the two is the documented cairo contract: flush before touching the
 * data, mark the touched rectangle dirty afterwards. raw_begin()/raw_end()
 * are that pair, and both are near-free on an image surface.
 */
/*
 * Extra rows allocated below the visible console so a full-width scroll can
 * be a change of viewing origin instead of a copy of the whole console. A
 * console scrolls by one text line at a time, so this buys headroom/line
 * scrolls before a real copy is needed, and the deeper the headroom the
 * rarer that copy. It is spent as a memory budget rather than a fixed row
 * count so that a wide window does not turn it into tens of megabytes, with
 * a floor that still covers a good many lines on the narrowest console.
 */
#define ACE_GFX_HEADROOM_BYTES (6u * 1024u * 1024u)
#define ACE_GFX_HEADROOM_MIN_ROWS 256
#define ACE_GFX_HEADROOM_MAX_ROWS 2048

static int scroll_headroom_rows(int width)
{
    size_t rows = ACE_GFX_HEADROOM_BYTES / ((size_t)width * sizeof(uint32_t));

    if (rows > ACE_GFX_HEADROOM_MAX_ROWS)
        rows = ACE_GFX_HEADROOM_MAX_ROWS;
    if (rows < ACE_GFX_HEADROOM_MIN_ROWS)
        rows = ACE_GFX_HEADROOM_MIN_ROWS;
    return (int)rows;
}

struct ace_gfx_rp_private {
    cairo_surface_t *surface;
    cairo_t *cr;
    uint32_t *base_pixels;
    int stride_px;
    /* The allocation is at least as large as the console in both axes; a
     * resize that still fits inside it reuses it rather than reallocating. */
    int alloc_width;
    int alloc_height;
    /* Row of the allocation that the console's row 0 currently lives on. */
    int origin_y;
    int width;
    int height;
    uint32_t palette[ACE_GFX_PEN_COUNT];
    int cell_width;
    int cell_height;
    int cell_columns;
    int cell_rows;
    struct ace_gfx_cell *cells;
    int cursor_column;
    int cursor_row;
    int cursor_inverted;
    int render_deferred;
    /* Union of everything drawn since the last ace_gfx_take_damage(), as an
     * inclusive rectangle. Empty when damage_x1 < damage_x0. */
    int damage_x0;
    int damage_y0;
    int damage_x1;
    int damage_y1;
};

static void rgb_components(uint32_t rgb, double *r, double *g, double *b)
{
    *r = ((rgb >> 16) & 0xff) / 255.0;
    *g = ((rgb >> 8) & 0xff) / 255.0;
    *b = (rgb & 0xff) / 255.0;
}

static uint32_t pen_rgb(struct ace_gfx_rp_private *priv, ULONG pen)
{
    if (pen >= ACE_GFX_PEN_COUNT)
        return 0;
    return priv->palette[pen];
}

/* CAIRO_FORMAT_RGB24 stores one 32-bit xRGB word per pixel. */
static uint32_t pen_pixel(struct ace_gfx_rp_private *priv, ULONG pen)
{
    return 0xff000000u | pen_rgb(priv, pen);
}

static struct ace_gfx_cell *cell_at(struct ace_gfx_rp_private *priv,
                                    int column, int row)
{
    if (!priv->cells || column < 0 || column >= priv->cell_columns ||
        row < 0 || row >= priv->cell_rows)
        return NULL;
    return &priv->cells[(size_t)row * priv->cell_columns + column];
}

static void cell_set_blank(struct ace_gfx_cell *cell, UBYTE background_pen,
                           UBYTE foreground_pen, UBYTE algo_style)
{
    cell->character = ' ';
    cell->algo_style = algo_style;
    cell->opaque = 1;
    cell->valid = 1;
    cell->foreground_pen = foreground_pen;
    cell->background_pen = background_pen;
}

static int cell_cache_resize(struct ace_gfx_rp_private *priv, int width,
                             int height, int cell_width, int cell_height,
                             UBYTE foreground_pen, UBYTE background_pen)
{
    struct ace_gfx_cell *cells;
    int columns;
    int rows;
    int row;
    int column;

    if (cell_width <= 0 || cell_height <= 0)
        return -1;
    columns = (width + cell_width - 1) / cell_width;
    rows = (height + cell_height - 1) / cell_height;
    cells = calloc((size_t)columns * rows, sizeof(*cells));
    if (!cells)
        return -1;

    for (row = 0; row < rows; row++)
        for (column = 0; column < columns; column++)
            cell_set_blank(&cells[(size_t)row * columns + column],
                           background_pen, foreground_pen, FS_NORMAL);

    if (priv->cells) {
        int copy_columns = priv->cell_columns < columns
                               ? priv->cell_columns : columns;
        int copy_rows = priv->cell_rows < rows ? priv->cell_rows : rows;

        if (priv->cell_width == cell_width &&
            priv->cell_height == cell_height) {
            for (row = 0; row < copy_rows; row++)
                memcpy(&cells[(size_t)row * columns],
                       &priv->cells[(size_t)row * priv->cell_columns],
                       (size_t)copy_columns * sizeof(*cells));
        }
        free(priv->cells);
    }

    priv->cells = cells;
    priv->cell_width = cell_width;
    priv->cell_height = cell_height;
    priv->cell_columns = columns;
    priv->cell_rows = rows;
    return 0;
}

static void cell_record(struct ace_gfx_rp_private *priv, int column, int row,
                        UBYTE character, UBYTE algo_style, UBYTE opaque,
                        UBYTE foreground_pen, UBYTE background_pen)
{
    struct ace_gfx_cell *cell = cell_at(priv, column, row);

    if (!cell)
        return;
    /* JAM1 draws only the glyph.  Its BPen says nothing about the pixels
       already in this cell, so retaining it would make a later cursor
       redraw invent a background that was never displayed. */
    if (!opaque && cell->valid)
        background_pen = cell->background_pen;
    cell->character = character;
    cell->algo_style = algo_style;
    cell->opaque = opaque;
    cell->valid = 1;
    cell->foreground_pen = foreground_pen;
    cell->background_pen = background_pen;
    if (priv->cursor_inverted && priv->cursor_column == column &&
        priv->cursor_row == row)
        priv->cursor_inverted = 0;
}

static void cell_invalidate_box(struct ace_gfx_rp_private *priv, int x0,
                                int y0, int x1, int y1)
{
    int first_column;
    int first_row;
    int last_column;
    int last_row;
    int row;
    int column;

    if (!priv->cells || priv->cell_width <= 0 || priv->cell_height <= 0)
        return;
    first_column = x0 / priv->cell_width;
    first_row = y0 / priv->cell_height;
    last_column = x1 / priv->cell_width;
    last_row = y1 / priv->cell_height;
    if (priv->cursor_inverted &&
        priv->cursor_column >= first_column &&
        priv->cursor_column <= last_column && priv->cursor_row >= first_row &&
        priv->cursor_row <= last_row)
        priv->cursor_inverted = 0;
    for (row = first_row; row <= last_row; row++) {
        for (column = first_column; column <= last_column; column++) {
            struct ace_gfx_cell *cell = cell_at(priv, column, row);

            if (cell)
                cell->valid = 0;
        }
    }
}

static void cell_fill_box(struct ace_gfx_rp_private *priv, int x0, int y0,
                          int x1, int y1, UBYTE background_pen,
                          UBYTE algo_style)
{
    int first_column;
    int first_row;
    int last_column;
    int last_row;
    int row;
    int column;

    if (!priv->cells || priv->cell_width <= 0 || priv->cell_height <= 0)
        return;
    first_column = (x0 + priv->cell_width - 1) / priv->cell_width;
    first_row = (y0 + priv->cell_height - 1) / priv->cell_height;
    last_column = (x1 + 1) / priv->cell_width - 1;
    last_row = (y1 + 1) / priv->cell_height - 1;
    cell_invalidate_box(priv, x0, y0, x1, y1);
    for (row = first_row; row <= last_row; row++) {
        for (column = first_column; column <= last_column; column++) {
            struct ace_gfx_cell *cell = cell_at(priv, column, row);

            if (cell)
                cell_set_blank(cell, background_pen, cell->foreground_pen,
                               algo_style);
        }
    }
}

/* Mirror a cell-aligned ScrollRaster() in the logical buffer. Console
 * applications commonly scroll a text-grid rectangle that excludes the
 * window's fractional right/bottom margins, so requiring the box to equal
 * the entire pixel surface would discard perfectly recoverable cell state. */
static int cell_scroll_box(struct ace_gfx_rp_private *priv, int dx, int dy,
                           int x0, int y0, int x1, int y1,
                           UBYTE background_pen, UBYTE algo_style)
{
    struct ace_gfx_cell *saved;
    int first_column;
    int first_row;
    int columns;
    int rows;
    int shift_columns;
    int shift_rows;
    int row;
    int column;

    if (!priv->cells || priv->cell_width <= 0 || priv->cell_height <= 0 ||
        x0 % priv->cell_width != 0 || y0 % priv->cell_height != 0 ||
        (x1 + 1) % priv->cell_width != 0 ||
        (y1 + 1) % priv->cell_height != 0 ||
        dx % priv->cell_width != 0 || dy % priv->cell_height != 0)
        return -1;
    first_column = x0 / priv->cell_width;
    first_row = y0 / priv->cell_height;
    columns = (x1 + 1) / priv->cell_width - first_column;
    rows = (y1 + 1) / priv->cell_height - first_row;
    if (columns <= 0 || rows <= 0 ||
        first_column < 0 || first_row < 0 ||
        first_column + columns > priv->cell_columns ||
        first_row + rows > priv->cell_rows)
        return -1;

    saved = malloc((size_t)columns * rows * sizeof(*saved));
    if (!saved)
        return -1;
    for (row = 0; row < rows; row++)
        memcpy(&saved[(size_t)row * columns],
               cell_at(priv, first_column, first_row + row),
               (size_t)columns * sizeof(*saved));

    priv->cursor_inverted = 0;
    shift_columns = dx / priv->cell_width;
    shift_rows = dy / priv->cell_height;
    for (row = 0; row < rows; row++) {
        for (column = 0; column < columns; column++) {
            int source_column = column + shift_columns;
            int source_row = row + shift_rows;
            struct ace_gfx_cell *destination =
                cell_at(priv, first_column + column, first_row + row);

            if (source_column >= 0 && source_column < columns &&
                source_row >= 0 && source_row < rows)
                *destination = saved[(size_t)source_row * columns +
                                     source_column];
            else
                cell_set_blank(destination, background_pen, 1, algo_style);
        }
    }
    free(saved);
    return 0;
}

static uint32_t *row_ptr(struct ace_gfx_rp_private *priv, int y)
{
    return priv->base_pixels +
           (size_t)(priv->origin_y + y) * priv->stride_px;
}

/* Moves the viewing window inside the allocation and keeps the cairo
 * context's user space pinned to the console's own coordinates. */
static void set_origin(struct ace_gfx_rp_private *priv, int origin_y)
{
    priv->origin_y = origin_y;
    cairo_identity_matrix(priv->cr);
    cairo_translate(priv->cr, 0.0, origin_y);
}

static void damage_clear(struct ace_gfx_rp_private *priv)
{
    priv->damage_x0 = 0;
    priv->damage_y0 = 0;
    priv->damage_x1 = -1;
    priv->damage_y1 = -1;
}

static void damage_add(struct ace_gfx_rp_private *priv, int x0, int y0,
                       int x1, int y1)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > priv->width - 1)
        x1 = priv->width - 1;
    if (y1 > priv->height - 1)
        y1 = priv->height - 1;
    if (x0 > x1 || y0 > y1)
        return;

    if (priv->damage_x1 < priv->damage_x0) {
        priv->damage_x0 = x0;
        priv->damage_y0 = y0;
        priv->damage_x1 = x1;
        priv->damage_y1 = y1;
        return;
    }
    if (x0 < priv->damage_x0)
        priv->damage_x0 = x0;
    if (y0 < priv->damage_y0)
        priv->damage_y0 = y0;
    if (x1 > priv->damage_x1)
        priv->damage_x1 = x1;
    if (y1 > priv->damage_y1)
        priv->damage_y1 = y1;
}

static void damage_all(struct ace_gfx_rp_private *priv)
{
    damage_clear(priv);
    damage_add(priv, 0, 0, priv->width - 1, priv->height - 1);
}

static void raw_begin(struct ace_gfx_rp_private *priv)
{
    cairo_surface_flush(priv->surface);
}

static void raw_end(struct ace_gfx_rp_private *priv, int x0, int y0,
                    int x1, int y1)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > priv->width - 1)
        x1 = priv->width - 1;
    if (y1 > priv->height - 1)
        y1 = priv->height - 1;
    if (x0 > x1 || y0 > y1)
        return;
    cairo_surface_mark_dirty_rectangle(priv->surface, x0, priv->origin_y + y0,
                                       x1 - x0 + 1, y1 - y0 + 1);
    damage_add(priv, x0, y0, x1, y1);
}

/*
 * Solid fill of an inclusive rectangle, clipped to the surface. The first
 * row is written word by word and the rest are memcpy'd from it, which hands
 * the bulk of a full-screen clear to the C library's vectorised copy instead
 * of a scalar store loop.
 */
static void fill_rect_raw(struct ace_gfx_rp_private *priv, int x0, int y0,
                          int x1, int y1, uint32_t pixel)
{
    uint32_t *first;
    int width;
    int x;
    int y;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > priv->width - 1)
        x1 = priv->width - 1;
    if (y1 > priv->height - 1)
        y1 = priv->height - 1;
    if (x0 > x1 || y0 > y1)
        return;

    width = x1 - x0 + 1;
    first = row_ptr(priv, y0) + x0;
    for (x = 0; x < width; x++)
        first[x] = pixel;
    for (y = y0 + 1; y <= y1; y++)
        memcpy(row_ptr(priv, y) + x0, first, (size_t)width * sizeof(uint32_t));
}

/*
 * Slack added to the width when the console outgrows its allocation, so that
 * a drag which widens the window a pixel at a time reallocates a handful of
 * times rather than on every step. The height needs none of its own: the
 * scroll headroom below the console already is that slack.
 */
#define ACE_GFX_GROW_SLACK 128

/*
 * Allocates the backing surface for a width x height console -- wider and
 * taller than the console itself, for growth slack and scroll headroom --
 * and caches its data pointer and stride. On success the caller owns
 * priv->surface/priv->cr.
 */
static int surface_bind(struct ace_gfx_rp_private *priv, int width, int height)
{
    cairo_surface_t *surface;
    cairo_t *cr;
    int alloc_width = width + ACE_GFX_GROW_SLACK;
    int alloc_height;
    int stride;

    alloc_height = height + scroll_headroom_rows(alloc_width);
    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, alloc_width,
                                         alloc_height);
    cr = cairo_create(surface);
    cairo_surface_flush(surface);
    stride = cairo_image_surface_get_stride(surface);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_status(cr) != CAIRO_STATUS_SUCCESS ||
        !cairo_image_surface_get_data(surface) || stride <= 0 ||
        stride % (int)sizeof(uint32_t) != 0) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return -1;
    }

    priv->surface = surface;
    priv->cr = cr;
    priv->base_pixels = (uint32_t *)cairo_image_surface_get_data(surface);
    priv->stride_px = stride / (int)sizeof(uint32_t);
    priv->alloc_width = alloc_width;
    priv->alloc_height = alloc_height;
    priv->origin_y = 0;
    priv->width = width;
    priv->height = height;
    damage_all(priv);
    return 0;
}

struct RastPort *ace_gfx_create_rastport(int width, int height,
                                         struct TextFont *font,
                                         const uint32_t rgb[ACE_GFX_PEN_COUNT])
{
    struct RastPort *rp;
    struct BitMap *bm;
    struct ace_gfx_rp_private *priv;
    int i;

    if (width <= 0 || height <= 0 || !font)
        return NULL;

    rp = calloc(1, sizeof(*rp));
    bm = calloc(1, sizeof(*bm));
    priv = calloc(1, sizeof(*priv));
    if (!rp || !bm || !priv) {
        free(rp);
        free(bm);
        free(priv);
        return NULL;
    }

    if (surface_bind(priv, width, height) != 0) {
        free(rp);
        free(bm);
        free(priv);
        return NULL;
    }
    for (i = 0; i < ACE_GFX_PEN_COUNT; i++)
        priv->palette[i] = rgb ? rgb[i] : (i == 0 ? 0x000000u : 0xffffffu);

    bm->BytesPerRow = (UWORD)((width + 15) & ~15) / 8;
    bm->Rows = (UWORD)height;
    bm->Flags = 0;
    bm->Depth = 24;

    rp->BitMap = bm;
    rp->Font = font;
    rp->FgPen = 1;
    rp->BgPen = 0;
    rp->DrawMode = JAM2;
    rp->cp_x = 0;
    rp->cp_y = 0;
    rp->AlgoStyle = FS_NORMAL;
    rp->TxHeight = font->tf_YSize;
    rp->TxWidth = font->tf_XSize;
    rp->TxBaseline = font->tf_Baseline;
    rp->RP_Extra = priv;

    if (cell_cache_resize(priv, width, height, font->tf_XSize,
                          font->tf_YSize, rp->FgPen, rp->BgPen) != 0) {
        cairo_destroy(priv->cr);
        cairo_surface_destroy(priv->surface);
        free(priv);
        free(bm);
        free(rp);
        return NULL;
    }

    /* Establish the surface in the background pen, as a fresh screen is. */
    raw_begin(priv);
    fill_rect_raw(priv, 0, 0, width - 1, height - 1, pen_pixel(priv, rp->BgPen));
    raw_end(priv, 0, 0, width - 1, height - 1);

    return rp;
}

void ace_gfx_destroy_rastport(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv;

    if (!rp)
        return;
    priv = rp->RP_Extra;
    if (priv) {
        free(priv->cells);
        cairo_destroy(priv->cr);
        cairo_surface_destroy(priv->surface);
        free(priv);
    }
    free(rp->BitMap);
    free(rp);
}

int ace_gfx_resize_rastport(struct RastPort *rp, int width, int height)
{
    struct ace_gfx_rp_private *priv;
    struct ace_gfx_rp_private next;
    int keep_width;
    int keep_height;
    int y;

    if (!rp || width <= 0 || height <= 0 || width > 65535 || height > 65535)
        return -1;
    priv = rp->RP_Extra;
    if (!priv)
        return -1;
    if (priv->width == width && priv->height == height)
        return 0;

    keep_width = priv->width < width ? priv->width : width;
    keep_height = priv->height < height ? priv->height : height;

    /*
     * A live resize arrives as a stream of sizes, so the common case must
     * not allocate: as long as the new console still fits inside the
     * existing allocation, only the newly uncovered strip is painted and the
     * pixels already in place stay exactly where they are.
     */
    if (width <= priv->alloc_width && height <= priv->alloc_height) {
        int old_width = priv->width;
        int old_height = priv->height;

        cairo_surface_flush(priv->surface);
        if (priv->origin_y + height > priv->alloc_height) {
            /* Scrolling has pushed the viewing origin too far down for the
             * taller console to fit below it. Fold what is on screen back to
             * the top of the allocation rather than allocate again. */
            uint32_t *visible = row_ptr(priv, 0);

            memmove(priv->base_pixels, visible,
                    (size_t)keep_height * priv->stride_px * sizeof(uint32_t));
            set_origin(priv, 0);
        }
        /* The new extent has to be in place before the fills: fill_rect_raw()
         * clips to the console, and these are the cells it just gained. */
        priv->width = width;
        priv->height = height;
        if (width > old_width)
            fill_rect_raw(priv, old_width, 0, width - 1, keep_height - 1,
                          pen_pixel(priv, rp->BgPen));
        if (height > old_height)
            fill_rect_raw(priv, 0, old_height, width - 1, height - 1,
                          pen_pixel(priv, rp->BgPen));
        cairo_surface_mark_dirty(priv->surface);
        damage_all(priv);
        rp->BitMap->BytesPerRow = (UWORD)((width + 15) & ~15) / 8;
        rp->BitMap->Rows = (UWORD)height;
        if (cell_cache_resize(priv, width, height, rp->Font->tf_XSize,
                              rp->Font->tf_YSize, rp->FgPen, rp->BgPen) != 0)
            return -1;
        return 0;
    }

    next = *priv;
    if (surface_bind(&next, width, height) != 0)
        return -1;

    /*
     * Carry the old pixels over row by row and paint only the strip the
     * larger surface newly exposes.
     */

    cairo_surface_flush(priv->surface);
    for (y = 0; y < keep_height; y++)
        memcpy(row_ptr(&next, y), row_ptr(priv, y),
               (size_t)keep_width * sizeof(uint32_t));
    if (width > keep_width)
        fill_rect_raw(&next, keep_width, 0, width - 1, keep_height - 1,
                      pen_pixel(&next, rp->BgPen));
    if (height > keep_height)
        fill_rect_raw(&next, 0, keep_height, width - 1, height - 1,
                      pen_pixel(&next, rp->BgPen));
    cairo_surface_mark_dirty(next.surface);

    cairo_destroy(priv->cr);
    cairo_surface_destroy(priv->surface);
    *priv = next;
    rp->BitMap->BytesPerRow = (UWORD)((width + 15) & ~15) / 8;
    rp->BitMap->Rows = (UWORD)height;
    if (cell_cache_resize(priv, width, height, rp->Font->tf_XSize,
                          rp->Font->tf_YSize, rp->FgPen, rp->BgPen) != 0)
        return -1;
    return 0;
}

int ace_gfx_take_damage(struct RastPort *rp, int *x_out, int *y_out,
                        int *width_out, int *height_out)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    if (!priv || priv->damage_x1 < priv->damage_x0)
        return 0;
    if (x_out)
        *x_out = priv->damage_x0;
    if (y_out)
        *y_out = priv->damage_y0;
    if (width_out)
        *width_out = priv->damage_x1 - priv->damage_x0 + 1;
    if (height_out)
        *height_out = priv->damage_y1 - priv->damage_y0 + 1;
    damage_clear(priv);
    return 1;
}

void ace_gfx_damage_all(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    if (priv)
        damage_all(priv);
}

void ace_gfx_set_render_deferred(struct RastPort *rp, int deferred)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    if (priv)
        priv->render_deferred = deferred != 0;
}

void ace_gfx_set_pen_rgb(struct RastPort *rp, int pen, uint32_t rgb)
{
    struct ace_gfx_rp_private *priv;

    if (!rp || pen < 0 || pen >= ACE_GFX_PEN_COUNT)
        return;
    priv = rp->RP_Extra;
    priv->palette[pen] = rgb;
}

void ace_gfx_rastport_size(struct RastPort *rp, int *width_out, int *height_out)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    if (width_out)
        *width_out = priv ? priv->width : 0;
    if (height_out)
        *height_out = priv ? priv->height : 0;
}

cairo_surface_t *ace_gfx_rastport_surface(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    return priv ? priv->surface : NULL;
}

int ace_gfx_rastport_origin_y(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;

    return priv ? priv->origin_y : 0;
}

void ace_gfx_read_rgb(struct RastPort *rp, uint8_t *out, size_t out_capacity)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;
    int x, y;
    size_t needed;

    if (!priv || !out)
        return;
    needed = (size_t)priv->width * (size_t)priv->height * 3;
    if (out_capacity < needed)
        return;

    cairo_surface_flush(priv->surface);

    for (y = 0; y < priv->height; y++) {
        uint32_t *row = row_ptr(priv, y);

        for (x = 0; x < priv->width; x++) {
            uint32_t argb = row[x];
            uint8_t *dst = out + ((size_t)y * priv->width + x) * 3;

            dst[0] = (argb >> 16) & 0xff;
            dst[1] = (argb >> 8) & 0xff;
            dst[2] = argb & 0xff;
        }
    }
}

/* -------------------------------------------------------------------------
 * graphics.library
 *
 * Signatures and semantics below come from reading the real implementations
 * in rom/graphics, not from the AmigaOS documentation alone -- in
 * particular, ScrollRaster()'s vacated-strip fill color: stdconclass.c
 * always does SetAPen(rp, backgroundPen) immediately before calling
 * ScrollRaster(), which only makes sense if ScrollRaster() fills the
 * uncovered area with the current FgPen, so that is what this
 * implementation does.
 * ---------------------------------------------------------------------- */

void Move(struct RastPort *rp, WORD x, WORD y)
{
    if (!rp)
        return;
    rp->cp_x = x;
    rp->cp_y = y;
}

void SetAPen(struct RastPort *rp, ULONG pen)
{
    if (!rp)
        return;
    rp->FgPen = (BYTE)pen;
}

void SetBPen(struct RastPort *rp, ULONG pen)
{
    if (!rp)
        return;
    rp->BgPen = (BYTE)pen;
}

void SetDrMd(struct RastPort *rp, ULONG drawMode)
{
    if (!rp)
        return;
    rp->DrawMode = (BYTE)drawMode;
}

void SetABPenDrMd(struct RastPort *rp, ULONG apen, ULONG bpen, ULONG drawMode)
{
    if (!rp)
        return;
    rp->FgPen = (BYTE)apen;
    rp->BgPen = (BYTE)bpen;
    rp->DrawMode = (BYTE)drawMode;
}

ULONG SetSoftStyle(struct RastPort *rp, ULONG style, ULONG enable)
{
    ULONG old;

    if (!rp)
        return 0;
    old = rp->AlgoStyle;
    rp->AlgoStyle = (UBYTE)((rp->AlgoStyle & ~enable) | (style & enable));
    return old;
}

static uint32_t xor_rgb(uint32_t rgb)
{
    return rgb ^ 0x00ffffffu;
}

/* Redraw one logical cell through cairo, preserving the glyph's shape while
 * changing the two colours that produced it. This is the host equivalent of
 * applying the cursor operation to the cell's foreground and background
 * pens; it deliberately never XORs antialiased pixels. */
static void render_cell(struct ace_gfx_rp_private *priv, struct TextFont *font,
                        int column, int row, const struct ace_gfx_cell *cell,
                        UBYTE foreground_pen, UBYTE background_pen)
{
    struct ace_gfx_font_private *font_private;
    cairo_glyph_t glyph;
    int style;
    int x = column * priv->cell_width;
    int top = row * priv->cell_height;
    double r, g, b;

    if (priv->render_deferred || !cell || !cell->valid || !font ||
        x >= priv->width ||
        top >= priv->height)
        return;
    font_private = (struct ace_gfx_font_private *)font;
    style = style_index(cell->algo_style);
    ensure_glyph_cache(font_private, style);

    /* This is a logical cell redraw, not a replay of the original Text()
       call. Even a JAM1 character needs its remembered background restored
       before its antialiased glyph is painted, or cairo blends the new glyph
       with the old one and leaves a fringe. */
    fill_rect_raw(priv, x, top,
                  x + priv->cell_width - 1,
                  top + priv->cell_height - 1,
                  pen_pixel(priv, background_pen));
    cairo_surface_mark_dirty_rectangle(priv->surface, x,
                                       priv->origin_y + top,
                                       priv->cell_width,
                                       priv->cell_height);

    cairo_save(priv->cr);
    cairo_rectangle(priv->cr, x, top, priv->cell_width, priv->cell_height);
    cairo_clip(priv->cr);
    rgb_components(pen_rgb(priv, foreground_pen), &r, &g, &b);
    cairo_set_source_rgb(priv->cr, r, g, b);
    cairo_set_scaled_font(priv->cr, font_private->scaled[style]);
    if (cell->character >= ACE_GFX_GLYPH_LO &&
        font_private->glyph[style][cell->character] != 0) {
        glyph.index = font_private->glyph[style][cell->character];
        glyph.x = x;
        glyph.y = top + font->tf_Baseline;
        cairo_show_glyphs(priv->cr, &glyph, 1);
    }
    if (cell->algo_style & FSF_UNDERLINED) {
        cairo_move_to(priv->cr, x, top + font->tf_Baseline + 1.0);
        cairo_line_to(priv->cr, x + priv->cell_width,
                      top + font->tf_Baseline + 1.0);
        cairo_set_line_width(priv->cr, 1.0);
        cairo_stroke(priv->cr);
    }
    cairo_restore(priv->cr);
}

/*
 * The real AROS cursor calls SetDrMd(COMPLEMENT) and RectFill over exactly
 * one character cell. ACE cannot invert the resolved surface pixels because
 * cairo antialiasing has already mixed foreground and background into them.
 * When the rectangle is that cursor-shaped operation and the cell's logical
 * contents are known, redraw the character with both stored pen indices
 * XORed, then look those pens up in the active palette.
 * Generic COMPLEMENT rectangles retain the pixel fallback because they have
 * no logical character to redraw.
 */
static void apply_complement(struct RastPort *rp, int x0, int y0, int x1,
                             int y1)
{
    struct ace_gfx_rp_private *priv = rp->RP_Extra;
    uint32_t pen_pixels[ACE_GFX_PEN_COUNT];
    unsigned pen;
    int x, y;

    if (x1 - x0 + 1 == priv->cell_width &&
        y1 - y0 + 1 == priv->cell_height &&
        priv->cell_width > 0 && priv->cell_height > 0 &&
        x0 % priv->cell_width == 0 && y0 % priv->cell_height == 0) {
        struct ace_gfx_cell *cell = cell_at(priv,
                                            x0 / priv->cell_width,
                                            y0 / priv->cell_height);
        int was_inverted = priv->cursor_inverted &&
                           priv->cursor_column == x0 / priv->cell_width &&
                           priv->cursor_row == y0 / priv->cell_height;

        if (cell && cell->valid) {
            render_cell(priv, rp->Font, x0 / priv->cell_width,
                        y0 / priv->cell_height, cell,
                        was_inverted ? cell->foreground_pen
                                     : cell->foreground_pen ^ ACE_GFX_PEN_MASK,
                        was_inverted ? cell->background_pen
                                     : cell->background_pen ^ ACE_GFX_PEN_MASK);
            priv->cursor_column = x0 / priv->cell_width;
            priv->cursor_row = y0 / priv->cell_height;
            priv->cursor_inverted = !was_inverted;
            return;
        }
    }

    if (priv->render_deferred) {
        cell_invalidate_box(priv, x0, y0, x1, y1);
        return;
    }

    for (pen = 0; pen < ACE_GFX_PEN_COUNT; pen++)
        pen_pixels[pen] = pen_pixel(priv, pen);

    for (y = y0; y <= y1; y++) {
        uint32_t *row = row_ptr(priv, y);

        for (x = x0; x <= x1; x++) {
            for (pen = 0; pen < ACE_GFX_PEN_COUNT; pen++) {
                if (row[x] == pen_pixels[pen]) {
                    row[x] = pen_pixels[pen ^ ACE_GFX_PEN_MASK];
                    break;
                }
            }
            if (pen == ACE_GFX_PEN_COUNT)
                row[x] = 0xff000000u | xor_rgb(row[x] & 0x00ffffffu);
        }
    }
}

void ace_gfx_render_grid(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;
    int was_deferred;
    int row;
    int column;

    if (!priv || !rp->Font)
        return;
    was_deferred = priv->render_deferred;
    priv->render_deferred = 0;
    cairo_surface_flush(priv->surface);
    fill_rect_raw(priv, 0, 0, priv->width - 1, priv->height - 1,
                  pen_pixel(priv, rp->BgPen));
    cairo_surface_mark_dirty_rectangle(priv->surface, 0, priv->origin_y,
                                       priv->width, priv->height);
    for (row = 0; row < priv->cell_rows; row++) {
        for (column = 0; column < priv->cell_columns; column++) {
            const struct ace_gfx_cell *cell = cell_at(priv, column, row);
            UBYTE foreground;
            UBYTE background;

            if (!cell || !cell->valid)
                continue;
            foreground = cell->foreground_pen;
            background = cell->background_pen;
            if (priv->cursor_inverted && priv->cursor_column == column &&
                priv->cursor_row == row) {
                foreground ^= ACE_GFX_PEN_MASK;
                background ^= ACE_GFX_PEN_MASK;
            }
            render_cell(priv, rp->Font, column, row, cell,
                        foreground, background);
        }
    }
    damage_all(priv);
    priv->render_deferred = was_deferred;
}

static uint64_t fingerprint_bytes(uint64_t fingerprint, const void *data,
                                  size_t length)
{
    const unsigned char *bytes = data;

    while (length-- != 0) {
        fingerprint ^= *bytes++;
        fingerprint *= UINT64_C(1099511628211);
    }
    return fingerprint;
}

uint64_t ace_gfx_grid_fingerprint(struct RastPort *rp)
{
    struct ace_gfx_rp_private *priv = rp ? rp->RP_Extra : NULL;
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    size_t cell_count;

    if (!priv)
        return 0;
    cell_count = (size_t)priv->cell_columns * (size_t)priv->cell_rows;
    fingerprint = fingerprint_bytes(fingerprint, &priv->cell_columns,
                                    sizeof(priv->cell_columns));
    fingerprint = fingerprint_bytes(fingerprint, &priv->cell_rows,
                                    sizeof(priv->cell_rows));
    fingerprint = fingerprint_bytes(fingerprint, priv->palette,
                                    sizeof(priv->palette));
    fingerprint = fingerprint_bytes(fingerprint, &rp->BgPen,
                                    sizeof(rp->BgPen));
    fingerprint = fingerprint_bytes(fingerprint, priv->cells,
                                    cell_count * sizeof(*priv->cells));
    fingerprint = fingerprint_bytes(fingerprint, &priv->cursor_inverted,
                                    sizeof(priv->cursor_inverted));
    if (priv->cursor_inverted) {
        fingerprint = fingerprint_bytes(fingerprint, &priv->cursor_column,
                                        sizeof(priv->cursor_column));
        fingerprint = fingerprint_bytes(fingerprint, &priv->cursor_row,
                                        sizeof(priv->cursor_row));
    }
    return fingerprint;
}

/* Clips an inclusive box to the surface. Returns 0 when nothing is left. */
static int clip_box(struct ace_gfx_rp_private *priv, WORD xMin, WORD yMin,
                    WORD xMax, WORD yMax, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = xMin < 0 ? 0 : xMin;
    *y0 = yMin < 0 ? 0 : yMin;
    *x1 = xMax >= priv->width ? priv->width - 1 : xMax;
    *y1 = yMax >= priv->height ? priv->height - 1 : yMax;
    return *x0 <= *x1 && *y0 <= *y1;
}

void RectFill(struct RastPort *rp, WORD xMin, WORD yMin, WORD xMax, WORD yMax)
{
    struct ace_gfx_rp_private *priv;
    int x0, y0, x1, y1;

    if (!rp)
        return;
    priv = rp->RP_Extra;
    if (!clip_box(priv, xMin, yMin, xMax, yMax, &x0, &y0, &x1, &y1))
        return;

    if (priv->render_deferred) {
        if (rp->DrawMode & COMPLEMENT)
            apply_complement(rp, x0, y0, x1, y1);
        else
            cell_fill_box(priv, x0, y0, x1, y1, rp->FgPen,
                          rp->AlgoStyle);
        return;
    }

    raw_begin(priv);
    if (rp->DrawMode & COMPLEMENT)
        apply_complement(rp, x0, y0, x1, y1);
    else {
        fill_rect_raw(priv, x0, y0, x1, y1, pen_pixel(priv, rp->FgPen));
        cell_fill_box(priv, x0, y0, x1, y1, rp->FgPen,
                      rp->AlgoStyle);
    }
    raw_end(priv, x0, y0, x1, y1);
}

/*
 * A console spends nearly all of its scrolling on one shape: everything from
 * the top-left corner, moved up by one text line. That case is served by
 * moving the viewing origin down inside the over-allocated surface instead
 * of copying the console, which turns the per-line cost from a copy of the
 * whole window into a fill of the one uncovered line.
 *
 * The scroll box does not always reach the right and bottom edges of the
 * surface -- a window whose pixel size is not a whole number of cells leaves
 * a margin -- and moving the origin moves that margin too. That is only
 * indistinguishable from a real scroll if the margin already holds the
 * colour the scroll would fill with, so it is checked rather than assumed;
 * a margin holding anything else falls back to the copying path below.
 *
 * Returns 0 if the caller should take the general path instead.
 */
static int scroll_by_origin(struct ace_gfx_rp_private *priv, int dy,
                            int x1, int y1, uint32_t fill)
{
    int keep = priv->height - dy;
    int x;
    int y;

    for (x = x1 + 1; x < priv->width; x++)
        for (y = 0; y < priv->height; y++)
            if (row_ptr(priv, y)[x] != fill)
                return 0;
    for (y = y1 + 1; y < priv->height; y++)
        for (x = 0; x <= x1; x++)
            if (row_ptr(priv, y)[x] != fill)
                return 0;

    cairo_surface_flush(priv->surface);
    if (priv->origin_y + dy + priv->height > priv->alloc_height) {
        /* Headroom is used up: fold the surviving rows back to the top of
         * the allocation. Whole rows move together, so the padding at the
         * end of each row travels with them. */
        memmove(priv->base_pixels,
                priv->base_pixels +
                    (size_t)(priv->origin_y + dy) * priv->stride_px,
                (size_t)keep * priv->stride_px * sizeof(uint32_t));
        set_origin(priv, 0);
    } else {
        set_origin(priv, priv->origin_y + dy);
    }

    fill_rect_raw(priv, 0, keep, priv->width - 1, priv->height - 1, fill);
    cairo_surface_mark_dirty_rectangle(priv->surface, 0,
                                       priv->origin_y + keep, priv->width, dy);
    damage_all(priv);
    return 1;
}

static void update_cells_after_scroll(struct ace_gfx_rp_private *priv, int dx,
                                      int dy, int x0, int y0, int x1, int y1,
                                      UBYTE background_pen, UBYTE algo_style)
{
    if (cell_scroll_box(priv, dx, dy, x0, y0, x1, y1,
                        background_pen, algo_style) == 0)
        return;
    cell_invalidate_box(priv, x0, y0, x1, y1);
}

void ScrollRaster(struct RastPort *rp, WORD dx, WORD dy, WORD xMin, WORD yMin,
                  WORD xMax, WORD yMax)
{
    struct ace_gfx_rp_private *priv;
    uint32_t fill;
    int x0, y0, x1, y1;
    int box_w, box_h;
    int adx, ady;
    int copy_w, copy_h;
    int src_x, dst_x, src_y, dst_y;
    int i;

    if (!rp || (dx == 0 && dy == 0))
        return;
    priv = rp->RP_Extra;
    if (!clip_box(priv, xMin, yMin, xMax, yMax, &x0, &y0, &x1, &y1))
        return;
    box_w = x1 - x0 + 1;
    box_h = y1 - y0 + 1;
    fill = pen_pixel(priv, rp->FgPen);
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;

    if (priv->render_deferred) {
        update_cells_after_scroll(priv, dx, dy, x0, y0, x1, y1,
                                  rp->FgPen, rp->AlgoStyle);
        return;
    }

    /*
     * The Amiga sign convention: positive dx/dy moves the pixels left/up.
     * The surviving pixels are moved with a per-row memmove -- source and
     * destination overlap on every scroll a console does, and memmove is
     * defined for that -- and only the strip the scroll vacates is filled.
     * The vacated strip takes the current FgPen: stdconclass.c always does
     * SetAPen(rp, backgroundPen) immediately before calling ScrollRaster(),
     * which only makes sense if that is the colour used.
     */
    if (dx == 0 && dy > 0 && dy < priv->height && x0 == 0 && y0 == 0 &&
        scroll_by_origin(priv, dy, x1, y1, fill)) {
        update_cells_after_scroll(priv, dx, dy, x0, y0, x1, y1,
                                  rp->FgPen, rp->AlgoStyle);
        return;
    }

    raw_begin(priv);
    if (adx >= box_w || ady >= box_h) {
        fill_rect_raw(priv, x0, y0, x1, y1, fill);
        raw_end(priv, x0, y0, x1, y1);
        update_cells_after_scroll(priv, dx, dy, x0, y0, x1, y1,
                                  rp->FgPen, rp->AlgoStyle);
        return;
    }

    copy_w = box_w - adx;
    copy_h = box_h - ady;
    src_x = x0 + (dx > 0 ? adx : 0);
    dst_x = x0 + (dx > 0 ? 0 : adx);
    src_y = y0 + (dy > 0 ? ady : 0);
    dst_y = y0 + (dy > 0 ? 0 : ady);

    /* Copy away from the vacated edge so an unread source row is never
     * overwritten first. */
    if (dy > 0) {
        for (i = 0; i < copy_h; i++)
            memmove(row_ptr(priv, dst_y + i) + dst_x,
                    row_ptr(priv, src_y + i) + src_x,
                    (size_t)copy_w * sizeof(uint32_t));
    } else {
        for (i = copy_h - 1; i >= 0; i--)
            memmove(row_ptr(priv, dst_y + i) + dst_x,
                    row_ptr(priv, src_y + i) + src_x,
                    (size_t)copy_w * sizeof(uint32_t));
    }

    if (dy > 0)
        fill_rect_raw(priv, x0, dst_y + copy_h, x1, y1, fill);
    else if (dy < 0)
        fill_rect_raw(priv, x0, y0, x1, dst_y - 1, fill);
    if (dx > 0)
        fill_rect_raw(priv, dst_x + copy_w, dst_y, x1, dst_y + copy_h - 1,
                      fill);
    else if (dx < 0)
        fill_rect_raw(priv, x0, dst_y, dst_x - 1, dst_y + copy_h - 1, fill);

    raw_end(priv, x0, y0, x1, y1);
    update_cells_after_scroll(priv, dx, dy, x0, y0, x1, y1,
                              rp->FgPen, rp->AlgoStyle);
}

/*
 * Glyphs are handed to cairo in batches rather than one show_glyphs() call
 * for the whole run so the position array stays on the stack.
 */
#define ACE_GFX_GLYPH_BATCH 128

void Text(struct RastPort *rp, CONST_STRPTR string, ULONG count)
{
    struct ace_gfx_rp_private *priv;
    struct ace_gfx_font_private *font;
    cairo_glyph_t batch[ACE_GFX_GLYPH_BATCH];
    ULONG drawn;
    ULONG i;
    int style;
    int opaque;
    int pen_x = rp ? rp->cp_x : 0;
    int pen_y = rp ? rp->cp_y : 0;
    int cell_w = rp && rp->Font ? rp->Font->tf_XSize : 0;
    int cell_h = rp && rp->Font ? rp->Font->tf_YSize : 0;
    int baseline = rp && rp->Font ? rp->Font->tf_Baseline : 0;
    int top;
    int run_w;
    int fits;
    unsigned fg_pen;
    unsigned bg_pen;
    double r, g, b;

    if (!rp || !string || !rp->Font || cell_w <= 0 || cell_h <= 0 || count == 0)
        return;
    priv = rp->RP_Extra;
    style = style_index(rp->AlgoStyle);

    /*
     * INVERSVID swaps the two pens for the duration of the draw, and the
     * JAM2 bit is what makes the cell background opaque -- setabpen() in
     * stdconclass.c builds exactly those two flags out of the console's
     * CON_TXTFLAGS_REVERSED/CONCEALED state.
     */
    fg_pen = rp->FgPen;
    bg_pen = rp->BgPen;
    if (rp->DrawMode & INVERSVID) {
        unsigned swap = fg_pen;

        fg_pen = bg_pen;
        bg_pen = swap;
    }
    opaque = (rp->DrawMode & JAM2) != 0;

    /* Cells that fall off the right edge are not drawn, but the pen still
     * advances by the full count, as real graphics.library's does. */
    fits = pen_x >= priv->width ? 0 : (priv->width - pen_x) / cell_w;
    drawn = count < (ULONG)fits ? count : (ULONG)fits;
    if (drawn == 0) {
        rp->cp_x = (WORD)(pen_x + (int)count * cell_w);
        return;
    }
    top = pen_y - baseline;
    run_w = (int)drawn * cell_w;

    for (i = 0; i < drawn; i++) {
        int x = pen_x + (int)i * cell_w;
        unsigned char code = (unsigned char)string[i];

        if (x % cell_w == 0 && top % cell_h == 0)
            cell_record(priv, x / cell_w, top / cell_h, code,
                        rp->AlgoStyle, (UBYTE)opaque,
                        (UBYTE)fg_pen, (UBYTE)bg_pen);
    }
    if (priv->render_deferred) {
        rp->cp_x = (WORD)(pen_x + (int)count * cell_w);
        return;
    }

    font = (struct ace_gfx_font_private *)rp->Font;
    ensure_glyph_cache(font, style);

    if (opaque) {
        raw_begin(priv);
        fill_rect_raw(priv, pen_x, top, pen_x + run_w - 1, top + cell_h - 1,
                      0xff000000u | pen_rgb(priv, bg_pen));
        raw_end(priv, pen_x, top, pen_x + run_w - 1, top + cell_h - 1);
    }

    /*
     * Antialiased glyph rendering can paint a pixel or two past a glyph's
     * own advance width -- real font hinting/kerning, not a bug -- which
     * would otherwise bleed out of the run into a neighbouring character
     * cell. A monospace terminal grid should never let that happen, so the
     * whole run is drawn under a clip to its own cells.
     */
    cairo_save(priv->cr);
    cairo_rectangle(priv->cr, pen_x, top, run_w, cell_h);
    cairo_clip(priv->cr);
    rgb_components(pen_rgb(priv, fg_pen), &r, &g, &b);
    cairo_set_source_rgb(priv->cr, r, g, b);
    cairo_set_scaled_font(priv->cr, font->scaled[style]);

    i = 0;
    while (i < drawn) {
        int in_batch = 0;

        while (i < drawn && in_batch < ACE_GFX_GLYPH_BATCH) {
            unsigned char code = (unsigned char)string[i];

            /* Control codes never reach a cell; console.device turns them
             * into commands before Text() ever sees them. */
            if (code >= ACE_GFX_GLYPH_LO && font->glyph[style][code] != 0) {
                batch[in_batch].index = font->glyph[style][code];
                batch[in_batch].x = pen_x + (double)i * cell_w;
                batch[in_batch].y = pen_y;
                in_batch++;
            }
            i++;
        }
        if (in_batch != 0)
            cairo_show_glyphs(priv->cr, batch, in_batch);
    }

    if (rp->AlgoStyle & FSF_UNDERLINED) {
        cairo_move_to(priv->cr, pen_x, pen_y + 1.0);
        cairo_line_to(priv->cr, pen_x + run_w, pen_y + 1.0);
        cairo_set_line_width(priv->cr, 1.0);
        cairo_stroke(priv->cr);
    }
    cairo_restore(priv->cr);

    damage_add(priv, pen_x, top, pen_x + run_w - 1, top + cell_h - 1);
    rp->cp_x = (WORD)(pen_x + (int)count * cell_w);
}


/* -------------------------------------------------------------------------
 * Scratch raster (AllocRaster/FreeRaster/InitTmpRas)
 *
 * These back the character-cell cursor's use of rp->TmpRas in
 * stdconclass.c. Real graphics.library uses this scratch space when its own
 * Text() renders a proportional font into an off-screen buffer before
 * blitting it. ACE's Text() above draws straight to the surface and never
 * consults rp->TmpRas, so nothing here needs planar bitmap semantics -- only
 * to hand back a buffer of the size AROS's own RASSIZE() macro computes, and
 * not crash when stdconclass.c sets it up.
 * ---------------------------------------------------------------------- */

PLANEPTR AllocRaster(UWORD width, UWORD height)
{
    ULONG size = RASSIZE(width, height);

    return calloc(1, size ? size : 1);
}

void FreeRaster(PLANEPTR p, UWORD width, UWORD height)
{
    (void)width;
    (void)height;
    free(p);
}

struct TmpRas *InitTmpRas(struct TmpRas *tmpRas, PLANEPTR buffer, LONG size)
{
    if (!tmpRas)
        return NULL;
    tmpRas->RasPtr = buffer;
    tmpRas->Size = size;
    return tmpRas;
}

/* -------------------------------------------------------------------------
 * Intuition DrawInfo (GetScreenDrawInfo/FreeScreenDrawInfo)
 *
 * stdconclass.c's constructor reads dri_Pens[BACKGROUNDPEN] and
 * dri_Pens[TEXTPEN] to remap the console's initial pen 0/1 to whatever a
 * screen's color scheme prefers; a single screen's worth of DrawInfo is
 * this seam's whole Intuition surface, real Intuition Screen/Window
 * structs being used as-is otherwise.
 * ---------------------------------------------------------------------- */

struct DrawInfo *GetScreenDrawInfo(struct Screen *screen)
{
    struct DrawInfo *dri = calloc(1, sizeof(*dri));
    UWORD *pens;

    (void)screen;
    if (!dri)
        return NULL;
    pens = calloc(NUMDRIPENS, sizeof(UWORD));
    if (!pens) {
        free(dri);
        return NULL;
    }
    pens[BACKGROUNDPEN] = 0;
    pens[TEXTPEN] = 1;

    dri->dri_Version = 1;
    dri->dri_NumPens = NUMDRIPENS;
    dri->dri_Pens = pens;
    dri->dri_Font = NULL;
    dri->dri_Depth = 24;
    return dri;
}

void FreeScreenDrawInfo(struct Screen *screen, struct DrawInfo *drawInfo)
{
    (void)screen;
    if (!drawInfo)
        return;
    free(drawInfo->dri_Pens);
    free(drawInfo);
}

/* -------------------------------------------------------------------------
 * TaggedOpenLibrary/CloseLibrary
 *
 * TaggedOpenLibrary() is AROS's own config-generated shortcut for opening
 * one of a small set of system libraries by tag; the console classes only
 * ever ask for TAGGEDOPEN_GRAPHICS, so it hands back a nonzero token
 * standing in for this file's own presence rather than a real Library base.
 * ---------------------------------------------------------------------- */

static int graphics_library_open_count;

void *TaggedOpenLibrary(IPTR library)
{
    if (library != TAGGEDOPEN_GRAPHICS)
        return NULL;
    graphics_library_open_count++;
    return &graphics_library_open_count;
}

/* Takes struct Library * to match the real Exec prototype, which
   proto/exec.h now declares for every ACE object rather than only the
   graphics ones.  The pointer this file hands out is the open counter
   standing in for a Library base, so it is only ever compared, never
   dereferenced. */
void CloseLibrary(struct Library *library)
{
    if ((void *)library == &graphics_library_open_count &&
        graphics_library_open_count > 0)
        graphics_library_open_count--;
}

/* -------------------------------------------------------------------------
 * Remaining small Exec/Utility calls
 * ---------------------------------------------------------------------- */

void SetMem(APTR destination, ULONG length, UBYTE value)
{
    memset(destination, value, length);
}

void CopyMem(CONST_APTR source, APTR destination, ULONG length)
{
    memmove(destination, source, length);
}

/*
 * rom/utility/gettagdata.c's real body is exactly this pass-through to
 * LibGetTagData(), which is itself exactly "found ? found->ti_Data :
 * defaultVal" over LibFindTagItem(). ACE compiles LibFindTagItem() and
 * LibNextTagItem() as real AROS source (compiler/arossupport) for the part
 * with actual logic -- the TAG_MORE/TAG_IGNORE/TAG_SKIP/TAG_END list walk --
 * and only this trivial combining step is hand-written, to avoid pulling in
 * rom/utility/intern.h for a one-line pass-through. See proto/utility.h.
 */
IPTR GetTagData(Tag tagValue, IPTR defaultVal, const struct TagItem *tagList)
{
    struct TagItem *found = LibFindTagItem(tagValue, tagList);

    return found ? found->ti_Data : defaultVal;
}

/* -------------------------------------------------------------------------
 * NewRawDoFmt (utility.library-shaped, %d/%u/%s/%c subset)
 *
 * A hand-written exception to "compile the real AROS source": the real
 * implementation (rom/exec/rawdofmt.c) depends on exec.library's private
 * exec_intern.h, a shadow far larger than ace_boopsi_intern.h's for a
 * function that never touches a pixel -- stdconclass.c uses it only to
 * format two VT100-style replies (cursor position report, window size
 * report), both %d substitution into a caller buffer via
 * RAWFMTFUNC_STRING. This covers exactly the conversions AROS's own console
 * sources use.
 * ---------------------------------------------------------------------- */

STRPTR NewRawDoFmt(CONST_STRPTR formatString, VOID_FUNC putChProc,
                   APTR putChData, ...)
{
    va_list args;
    char *out = (char *)putChData;
    const char *f = (const char *)formatString;

    (void)putChProc; /* RAWFMTFUNC_STRING is the only mode this seam needs */

    va_start(args, putChData);
    while (*f) {
        if (*f == '%') {
            f++;
            switch (*f) {
            case 'd': {
                int v = va_arg(args, int);
                unsigned int u = v < 0 ? (unsigned int)(-v) : (unsigned int)v;
                char digits[16];
                int n = 0;

                if (v < 0)
                    *out++ = '-';
                do {
                    digits[n++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u && n < (int)sizeof(digits));
                while (n > 0)
                    *out++ = digits[--n];
                f++;
                break;
            }
            case 'u': {
                unsigned int u = va_arg(args, unsigned int);
                char digits[16];
                int n = 0;

                do {
                    digits[n++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u && n < (int)sizeof(digits));
                while (n > 0)
                    *out++ = digits[--n];
                f++;
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);

                while (s && *s)
                    *out++ = *s++;
                f++;
                break;
            }
            case 'c':
                *out++ = (char)va_arg(args, int);
                f++;
                break;
            case '%':
                *out++ = '%';
                f++;
                break;
            default:
                *out++ = '%';
                break;
            }
        } else {
            *out++ = *f++;
        }
    }
    va_end(args);
    *out = '\0';
    return (STRPTR)putChData;
}
