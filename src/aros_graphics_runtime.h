#ifndef ACE_AROS_GRAPHICS_RUNTIME_H
#define ACE_AROS_GRAPHICS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include <cairo/cairo.h>

/*
 * Host seam for graphics.library, the RastPort/BitMap/TextFont surface the
 * real AROS console classes in rom/devs/console draw through. Unlike the
 * BOOPSI seam, this one is genuinely ACE's to write: graphics.library is the
 * hardware boundary itself, the point where AmigaOS drawing calls become
 * pixels on a real display. There is no upstream AROS source to compile here
 * that does not lead to a driver (HIDD) stack ACE does not want.
 *
 * The console classes only ever reach a RastPort's pixels through ten calls
 * (Move, Text, SetAPen, SetBPen, SetDrMd, SetABPenDrMd, RectFill,
 * ScrollRaster, SetSoftStyle, plus the AllocRaster/FreeRaster/InitTmpRas
 * scratch-buffer trio used only by the character-cell cursor). Everything
 * this file exposes beyond those calls is for constructing the RastPort,
 * BitMap and TextFont a real ConUnit expects to find.
 */

struct RastPort;
struct BitMap;
struct TextFont;

/*
 * A host font choice: a fontconfig family name and a pixel size. ACE
 * validates on load that the family has real regular, bold, italic and
 * bold-italic faces -- SetSoftStyle() requests are rendered with the actual
 * face, not synthesized the way real Amiga hardware smears/shears a single
 * bitmap face, since a host compositor can just ask for the real one.
 * Underline is drawn as a rule beneath the baseline either way: it is a
 * soft-style flag on real hardware too, not a font glyph.
 */
struct ace_gfx_font_choice {
    const char *family;
    int pixel_size;
    /* Base weight, on fontconfig's scale (FC_WEIGHT_*).  Zero means
       FC_WEIGHT_REGULAR.  A family like FiraCode Nerd Font ships Light,
       Regular, Retina, Medium and SemiBold faces that Pango reports as faces
       *within* one family, not as families of their own, so a family name
       alone cannot say which of them was asked for.  Bold stays bold
       regardless: this picks which face is the unemphasised one. */
    int weight;
};

/*
 * Loads and validates a font choice, returning a real struct TextFont whose
 * tf_XSize/tf_YSize/tf_Baseline are measured from the host face. This is
 * what a caller attaches to a window's RastPort as rp->Font before console
 * classes read cu_XRSize/cu_YRSize from it, per AROS's own
 * "For now one should use only non-proportional fonts" contract in
 * rom/devs/console/consoleclass.c.
 *
 * Returns NULL if the family cannot be found, or is missing any of the four
 * required style faces. *reason_out, if non-NULL, receives a static string
 * describing why.
 */
struct TextFont *ace_gfx_load_font(const struct ace_gfx_font_choice *choice,
                                   const char **reason_out);
void ace_gfx_unload_font(struct TextFont *font);

/* Whether a family has real base/bold/italic/bold-italic faces at the given
   weight.  Pass 0 for the ordinary regular weight. */
int ace_gfx_font_family_complete(const char *family, int weight);

/* Maps a CSS/OpenType weight number -- Pango's scale, where 400 is Regular
   and 500 Medium -- onto fontconfig's, where those are 80 and 100.  Returns
   0 for the default weight so callers can pass it straight through. */
int ace_gfx_weight_from_css(int css_weight);

#define ACE_GFX_PEN_COUNT 8
/* The bitplane depth behind that pen count, as the mask a COMPLEMENT draw
 * inverts a pen index against. Three planes give pens 0-7 and a mask of 7. */
#define ACE_GFX_PEN_MASK (ACE_GFX_PEN_COUNT - 1)

/*
 * Creates a RastPort over a freshly allocated width x height BitMap, with the
 * given font attached and the palette initialised from rgb (ACE_GFX_PEN_COUNT
 * entries, 0xRRGGBB each). Pen N always starts mapped to palette entry N, the
 * same identity stdconclass.c's own constructor establishes before a
 * screen's DrawInfo remaps pens 0/1 to background/text.
 */
struct RastPort *ace_gfx_create_rastport(int width, int height,
                                         struct TextFont *font,
                                         const uint32_t rgb[ACE_GFX_PEN_COUNT]);
void ace_gfx_destroy_rastport(struct RastPort *rp);

/* Changes the pixel surface dimensions while preserving the existing
 * RastPort, font, parser state, and visible pixels. */
int ace_gfx_resize_rastport(struct RastPort *rp, int width, int height);

/*
 * The bounding box of everything drawn since the last call, so the window
 * owner can repaint just that area instead of the whole console. Returns 0
 * and leaves the outputs untouched when nothing has been drawn; returns 1
 * and resets the accumulator otherwise.
 */
int ace_gfx_take_damage(struct RastPort *rp, int *x_out, int *y_out,
                        int *width_out, int *height_out);

/* Marks the whole surface as needing repaint, e.g. after a resize. */
void ace_gfx_damage_all(struct RastPort *rp);

/* High-output path: keep the logical character grid current while
 * suppressing pixel work, and rebuild complete pixel snapshots from it when
 * the GUI has an opportunity to present. */
void ace_gfx_set_render_deferred(struct RastPort *rp, int deferred);
void ace_gfx_render_grid(struct RastPort *rp);
uint64_t ace_gfx_grid_fingerprint(struct RastPort *rp);

/* Replaces one pen's RGB entry, e.g. after a screen DrawInfo remap. */
void ace_gfx_set_pen_rgb(struct RastPort *rp, int pen, uint32_t rgb);

/*
 * Copies the current pixels out as tightly packed 8-bit RGB, row-major,
 * width*height*3 bytes. This is the host's read side of the seam: whatever
 * owns the actual window (Wayland/GTK, or a test) pulls frames out this way
 * rather than reaching into the RastPort's private rendering state.
 */
void ace_gfx_read_rgb(struct RastPort *rp, uint8_t *out,
                      size_t out_capacity);
void ace_gfx_rastport_size(struct RastPort *rp, int *width_out,
                           int *height_out);

/*
 * The RastPort's own pixel surface, for a caller that already owns a cairo_t
 * of its own (the live GTK window) to blit directly instead of round-
 * tripping through ace_gfx_read_rgb()'s byte array. Owned by the RastPort;
 * do not destroy it, and do not hold it past ace_gfx_destroy_rastport().
 *
 * The surface is taller than the console: spare rows below the visible area
 * let a scroll move the viewing origin instead of copying every pixel. A
 * caller blitting it must therefore take the console's rows from
 * ace_gfx_rastport_origin_y() downwards, not from the top of the surface,
 * and must not assume the surface's height is the console's height.
 */
cairo_surface_t *ace_gfx_rastport_surface(struct RastPort *rp);
int ace_gfx_rastport_origin_y(struct RastPort *rp);

#endif
