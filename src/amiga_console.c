#define _GNU_SOURCE
#define _XOPEN_SOURCE 600

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <datatypes/textclass.h>
#include <libraries/iffparse.h>
#include <proto/iffparse.h>
#include <errno.h>
#include <limits.h>
#include <pango/pango.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aros_graphics_runtime.h"
#include "ace_appmenu_wayland.h"
#include "ace_requestor_gui.h"
#include "console_channel.h"
#include "console_dispatch.h"
#include "console_device_bridge.h"
#include "console_spec.h"

#define INPUT_MAX 4096

/*
 * The console window's pixel size. Real AROS console classes read
 * win->Width/Height at ConUnit construction (consoleclass.c's
 * console_new()) and again through M_Console_NewWindowSize when the window
 * changes. The GTK drawing area is the emulated AROS Window here.
 */
#define CONSOLE_WIDTH 900
#define CONSOLE_HEIGHT 576

/* Candidates are tried in order so the window still opens on a host without
 * the first choice installed. The active typeface can be changed from the
 * ACE Shell menu. */
static const char *const default_font_candidates[] = {
    "Liberation Mono", "DejaVu Sans Mono", "monospace", NULL
};
/* Font sizes are POINTS everywhere above the graphics runtime, because that
 * is what the GTK font chooser speaks and what every other terminal on the
 * desktop stores.  ace.conf's font-size is points too.
 *
 * The graphics runtime below is deliberately the other way round: an Amiga
 * struct TextFont measures tf_YSize in pixels -- topaz.font 8 is eight pixels
 * tall, not eight points -- so ace_gfx_font_choice.pixel_size is correctly
 * named and stays pixels.  This file is the boundary between the two, and
 * font_pixels() is the only crossing.
 *
 * These used to be the same number, which meant an 11 chosen in ACE rendered
 * at 11 pixels while an 11 chosen in any other terminal rendered at 11 points
 * -- 14.67 pixels at 96 dpi.  ACE drew every font at 75% of the requested
 * size, and the font chooser's own preview disagreed with the result.
 */
#define DEFAULT_FONT_POINTS 12

/* Pango's default resolution, used when nothing overrides it.  ACE cannot ask
 * GDK for the real one here: the console font is loaded before gtk_init(), so
 * that the cell size is known in time to size the window.  A screen with a
 * non-default Xft.dpi will therefore render ACE at 96 dpi while GTK apps
 * around it scale -- if that needs fixing, this is the one place to change.
 */
#define FONT_REFERENCE_DPI 96.0

static int font_pixels(int points)
{
    int pixels = (int)(points * FONT_REFERENCE_DPI / 72.0 + 0.5);

    return pixels > 0 ? pixels : 1;
}
#define CONFIG_GROUP "ACE Shell"
#define ACE_ICON_NAME "ace-shell"
#define CONFIG_FONT_FAMILY "font-family"
#define CONFIG_FONT_SIZE "font-size"
#define CONFIG_FONT_WEIGHT "font-weight"
#define CONFIG_PALETTE_PREFIX "palette-"

static const uint32_t default_palette[ACE_CONSOLE_PEN_COUNT] = {
    0x000000u, 0xffffffu, 0xff5555u, 0x55ff55u,
    0x5555ffu, 0xffff55u, 0xff55ffu, 0x55ffffu,
};

/*
 * Named palettes, so eight pens can be chosen rather than hand-mixed. The pen
 * order follows default_palette above: pen 0 is the background and pen 1 the
 * text -- the two stdconclass.c's penmap[] redirects to BACKGROUNDPEN and
 * TEXTPEN -- and pens 2-7 carry the six accents CSI 32m-37m reach.
 *
 * Pen 7 is worth choosing deliberately: it doubles as the cursor colour. The
 * console draws its cursor with a COMPLEMENT RectFill, and over the
 * background that inverts pen 0 to pen 0 ^ ACE_GFX_PEN_MASK, which is pen 7
 * on the three bitplanes ACE simulates.
 *
 * "Retro" is Commodore's own eight Workbench 3.1 defaults, kept in their own
 * order rather than remapped into the accent order the imported schemes use;
 * that ordering is the historical artifact, so bending it would be a lie.
 * The rest are transcribed from each project's published palette.
 */
struct palette_preset {
    const char *name;
    uint32_t colors[ACE_CONSOLE_PEN_COUNT];
};

static const struct palette_preset palette_presets[] = {
    /* AROS workbench/prefs/palette/prefs.c defaultcolor[], which carries
       Commodore's 12-bit registers ($AAA, $68B, $E44 ...) intact. */
    { "Retro",
      { 0xaaaaaau, 0x000000u, 0xffffffu, 0x6688bbu,
        0xee4444u, 0x55dd55u, 0x0044ddu, 0xee9900u } },
    { "ACE Default",
      { 0x000000u, 0xffffffu, 0xff5555u, 0x55ff55u,
        0x5555ffu, 0xffff55u, 0xff55ffu, 0x55ffffu } },
    { "Catppuccin Latte",
      { 0xeff1f5u, 0x4c4f69u, 0xd20f39u, 0x40a02bu,
        0x1e66f5u, 0xdf8e1du, 0x8839efu, 0x179299u } },
    { "Catppuccin Mocha",
      { 0x1e1e2eu, 0xcdd6f4u, 0xf38ba8u, 0xa6e3a1u,
        0x89b4fau, 0xf9e2afu, 0xcba6f7u, 0x94e2d5u } },
    /* Dracula names no plain blue; its purple stands in, as it does in the
       scheme's own terminal mappings. */
    { "Dracula",
      { 0x282a36u, 0xf8f8f2u, 0xff5555u, 0x50fa7bu,
        0xbd93f9u, 0xf1fa8cu, 0xff79c6u, 0x8be9fdu } },
    { "Gruvbox Dark",
      { 0x282828u, 0xebdbb2u, 0xfb4934u, 0xb8bb26u,
        0x83a598u, 0xfabd2fu, 0xd3869bu, 0x8ec07cu } },
    { "Nord",
      { 0x2e3440u, 0xd8dee9u, 0xbf616au, 0xa3be8cu,
        0x81a1c1u, 0xebcb8bu, 0xb48eadu, 0x88c0d0u } },
    { "Solarized Dark",
      { 0x002b36u, 0x839496u, 0xdc322fu, 0x859900u,
        0x268bd2u, 0xb58900u, 0xd33682u, 0x2aa198u } },
    { "Solarized Light",
      { 0xfdf6e3u, 0x657b83u, 0xdc322fu, 0x859900u,
        0x268bd2u, 0xb58900u, 0xd33682u, 0x2aa198u } },
    /* Official Tango defines no cyan; 0x06989a is the value GNOME Terminal
       added to complete the scheme, and is what "Tango" means for a
       terminal. */
    { "Tango",
      { 0x2e3436u, 0xd3d7cfu, 0xcc0000u, 0x4e9a06u,
        0x3465a4u, 0xc4a000u, 0x75507bu, 0x06989au } },
};

#define PALETTE_PRESET_COUNT \
    ((int)(sizeof(palette_presets) / sizeof(palette_presets[0])))

struct console_window {
    GtkWidget *window;
    GtkWidget *menu_bar;
    GtkWidget *drawing_area;
    int stream_fd;
    struct ace_console_channel channel;
    pid_t child_pid;
    pid_t controller_pid;
    char *font_family;
    int font_points;
    /* CSS/Pango weight: 400 Regular, 500 Medium.  0 means unset. */
    int font_weight;
    uint32_t palette[ACE_CONSOLE_PEN_COUNT];
    guint menu_probe_source;
    guint resize_source;
    guint damage_source;
    guint overload_frame_source;
    guint overload_settle_source;
    gboolean render_deferred;
    uint64_t presented_grid_fingerprint;
    int pending_width;
    int pending_height;
    int title_state;
    char title_buffer[1024];
    size_t title_length;
    char current_title[1024];
    gboolean menu_type_hint_restored;
    gboolean menu_wayland_live;
    gboolean fullscreen;
    gboolean selection_dragging;
    gboolean selection_valid;
    int selection_start_column;
    int selection_start_row;
    int selection_end_column;
    int selection_end_row;
    guint copy_status_source;
    char copy_status[128];
    GDBusConnection *menu_bus;
    GMenu *menu_model;
    GSimpleActionGroup *menu_actions;
    guint menu_model_export_id;
    guint menu_action_export_id;
    char *menu_object_path;

    /*
     * Real AROS console.device rendering state, behind an opaque bridge --
     * see console_device_bridge.h for why amiga_console.c cannot include
     * AROS's own headers directly (struct timeval and MAX/MIN collide with
     * glib's).
     */
    struct ace_console_device *device;
};

static void clear_selection(struct console_window *console);
static void copy_selection(struct console_window *console);
static void refresh_console_title(struct console_window *console);

static void update_channel_geometry(struct console_window *console,
                                    gboolean notify)
{
    int pixel_width;
    int pixel_height;
    int cell_width;
    int cell_height;

    ace_console_device_size(console->device, &pixel_width, &pixel_height);
    if (ace_console_device_cell_size(console->device, &cell_width,
                                     &cell_height) != 0 ||
        cell_width <= 0 || cell_height <= 0)
        return;
    if (notify)
        ace_console_channel_notify_resize(&console->channel,
                                          pixel_height / cell_height,
                                          pixel_width / cell_width);
    else
        ace_console_channel_set_geometry(&console->channel,
                                         pixel_height / cell_height,
                                         pixel_width / cell_width);
}

static char *config_path(void)
{
    const char *home = g_get_home_dir();

    if (!home || !*home)
        return NULL;
    return g_build_filename(home, ".config", "ace.conf", NULL);
}

static gboolean parse_rgb(const char *text, uint32_t *rgb)
{
    char *end;
    guint64 value;

    if (!text || strlen(text) != 6)
        return FALSE;
    value = g_ascii_strtoull(text, &end, 16);
    if (end != text + 6 || value > 0xffffffu)
        return FALSE;
    *rgb = (uint32_t)value;
    return TRUE;
}

static void load_config(struct console_window *console)
{
    GKeyFile *key_file;
    GError *error = NULL;
    char *path;
    char *family;
    int size;
    int i;

    path = config_path();
    if (!path)
        return;
    key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            fprintf(stderr, "ace-console: cannot read %s: %s\n", path,
                    error->message);
        g_clear_error(&error);
        g_key_file_unref(key_file);
        g_free(path);
        return;
    }

    {
        int weight = g_key_file_get_integer(key_file, CONFIG_GROUP,
                                            CONFIG_FONT_WEIGHT, &error);

        if (!error && weight >= 100 && weight <= 1000)
            console->font_weight = weight;
        g_clear_error(&error);
    }

    family = g_key_file_get_string(key_file, CONFIG_GROUP,
                                    CONFIG_FONT_FAMILY, &error);
    if (family && *family && strlen(family) < 128) {
        /* Keep the family even when the saved weight is not available in it:
           a font the user chose at the wrong weight beats falling back to a
           different typeface entirely.  Dropping the weight here also stops a
           stale one following the family around after a font is reinstalled
           without its Medium face. */
        if (!ace_gfx_font_family_complete(
                family, ace_gfx_weight_from_css(console->font_weight)) &&
            ace_gfx_font_family_complete(family, 0))
            console->font_weight = 0;
        if (ace_gfx_font_family_complete(
                family, ace_gfx_weight_from_css(console->font_weight))) {
            g_free(console->font_family);
            console->font_family = family;
            family = NULL;
        }
    }
    g_free(family);
    g_clear_error(&error);

    size = g_key_file_get_integer(key_file, CONFIG_GROUP, CONFIG_FONT_SIZE,
                                  &error);
    if (!error && size > 0 && size <= 512)
        console->font_points = size;
    g_clear_error(&error);

    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        char key[32];
        char *text;
        uint32_t rgb;

        snprintf(key, sizeof(key), "%s%d", CONFIG_PALETTE_PREFIX, i);
        text = g_key_file_get_string(key_file, CONFIG_GROUP, key, &error);
        if (text && parse_rgb(text, &rgb))
            console->palette[i] = rgb;
        g_free(text);
        g_clear_error(&error);
    }
    g_key_file_unref(key_file);
    g_free(path);
}

static void save_config(struct console_window *console)
{
    GKeyFile *key_file;
    GError *error = NULL;
    char *path;
    char *directory;
    char *data;
    gsize length;
    int i;

    path = config_path();
    if (!path)
        return;
    directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        fprintf(stderr, "ace-console: cannot create %s: %s\n", directory,
                g_strerror(errno));
        g_free(directory);
        g_free(path);
        return;
    }
    key_file = g_key_file_new();
    g_key_file_set_string(key_file, CONFIG_GROUP, CONFIG_FONT_FAMILY,
                          console->font_family);
    g_key_file_set_integer(key_file, CONFIG_GROUP, CONFIG_FONT_SIZE,
                           console->font_points);
    g_key_file_set_integer(key_file, CONFIG_GROUP, CONFIG_FONT_WEIGHT,
                           console->font_weight);
    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        char key[32];
        char value[7];

        snprintf(key, sizeof(key), "%s%d", CONFIG_PALETTE_PREFIX, i);
        snprintf(value, sizeof(value), "%06x", console->palette[i]);
        g_key_file_set_string(key_file, CONFIG_GROUP, key, value);
    }
    data = g_key_file_to_data(key_file, &length, &error);
    if (!data || !g_file_set_contents(path, data, (gssize)length, &error)) {
        fprintf(stderr, "ace-console: cannot write %s: %s\n", path,
                error ? error->message : "unknown error");
    }
    g_clear_error(&error);
    g_free(data);
    g_key_file_unref(key_file);
    g_free(directory);
    g_free(path);
}

static gboolean font_is_monospace(const PangoFontFamily *family,
                                  const PangoFontFace *face, gpointer data)
{
    (void)face;
    (void)data;
    return pango_font_family_is_monospace((PangoFontFamily *)family);
}

static void show_error(struct console_window *console, const char *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(console->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static size_t build_font_candidates(const char *preferred,
                                    const char *const *defaults,
                                    const char **out, size_t capacity)
{
    size_t count = 0;
    int i;

    if (preferred && *preferred && count + 1 < capacity)
        out[count++] = preferred;
    for (i = 0; defaults[i] && count + 1 < capacity; i++) {
        if (preferred && strcmp(preferred, defaults[i]) == 0)
            continue;
        out[count++] = defaults[i];
    }
    out[count] = NULL;
    return count;
}

static void choose_font(GtkWidget *widget, gpointer data)
{
    struct console_window *console = data;
    GtkWidget *dialog;
    GtkFontChooser *chooser;
    PangoFontDescription *description;
    const char *family;
    char *initial_font;
    int point_size;
    int weight;

    (void)widget;
    dialog = gtk_font_chooser_dialog_new("Choose Typeface",
                                         GTK_WINDOW(console->window));
    gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_UTILITY);
    chooser = GTK_FONT_CHOOSER(dialog);
    gtk_font_chooser_set_filter_func(chooser, font_is_monospace, NULL, NULL);
    /* Built as a Pango description so the dialog reopens on the face the user
       actually chose.  Weight is a Pango enum value in that syntax, so it is
       written as a number rather than a name. */
    initial_font = g_strdup_printf("%s weight=%d %d", console->font_family,
                                   console->font_weight > 0
                                       ? console->font_weight
                                       : PANGO_WEIGHT_NORMAL,
                                   console->font_points);
    gtk_font_chooser_set_font(chooser, initial_font);
    g_free(initial_font);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(dialog);
        return;
    }
    /* The whole description, not gtk_font_chooser_get_font_family(): that
       returns the PangoFontFamily and so throws the face away.  Pango groups
       FiraCode Nerd Font's Light/Regular/Retina/Medium/SemiBold as faces
       inside one family, so choosing "FiraCode Nerd Font Medium" yields
       family "FiraCode Nerd Font" and a Medium weight -- keeping only the
       family silently selected Regular, and the choice appeared not to
       stick no matter how many times it was made. */
    description = gtk_font_chooser_get_font_desc(chooser);
    family = description ? pango_font_description_get_family(description) : NULL;
    /* GtkFontChooser reports points, so this is the size the user picked --
       the device wants pixels and gets them from font_pixels(). */
    point_size = description
                     ? pango_font_description_get_size(description) / PANGO_SCALE
                     : 0;
    weight = description ? (int)pango_font_description_get_weight(description)
                         : PANGO_WEIGHT_NORMAL;
    if (!family || point_size <= 0 ||
        ace_console_device_set_font(console->device, family,
                                    font_pixels(point_size),
                                    ace_gfx_weight_from_css(weight)) != 0) {
        if (description)
            pango_font_description_free(description);
        gtk_widget_destroy(dialog);
        show_error(console, "That typeface could not be loaded by ACE Shell.");
        return;
    }
    g_free(console->font_family);
    console->font_family = g_strdup(family);
    console->font_points = point_size;
    console->font_weight = weight;
    pango_font_description_free(description);
    save_config(console);
    refresh_console_title(console);
    gtk_widget_queue_draw(console->drawing_area);
    gtk_widget_destroy(dialog);
}

static GdkRGBA palette_rgba(uint32_t rgb)
{
    GdkRGBA color = {
        ((rgb >> 16) & 0xff) / 255.0,
        ((rgb >> 8) & 0xff) / 255.0,
        (rgb & 0xff) / 255.0,
        1.0,
    };
    return color;
}

static uint32_t palette_rgb(const GdkRGBA *color)
{
    guint red = (guint)(CLAMP(color->red, 0.0, 1.0) * 255.0 + 0.5);
    guint green = (guint)(CLAMP(color->green, 0.0, 1.0) * 255.0 + 0.5);
    guint blue = (guint)(CLAMP(color->blue, 0.0, 1.0) * 255.0 + 0.5);

    return (red << 16) | (green << 8) | blue;
}

/*
 * The palette dialog is a fixed list of named schemes over an editable set of
 * eight pens. Picking a scheme greys the pens out -- they then only report
 * what was picked -- and the trailing "Custom" row hands them back.
 */
struct palette_dialog {
    GtkWidget *buttons[ACE_CONSOLE_PEN_COUNT];
    GtkWidget *custom_frame;
    GtkWidget *custom_swatch;
    /* The user's own eight pens, held across excursions into the presets so
       that browsing the list never costs them their colours. */
    uint32_t custom[ACE_CONSOLE_PEN_COUNT];
    int selected; /* index into palette_presets, or -1 for Custom */
};

/* Draws a row's eight pens as one strip, so the list can be read by colour
   rather than by name alone. */
static gboolean draw_palette_swatch(GtkWidget *widget, cairo_t *cr,
                                    gpointer data)
{
    const uint32_t *colors = data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    int i;

    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        int x0 = width * i / ACE_CONSOLE_PEN_COUNT;
        int x1 = width * (i + 1) / ACE_CONSOLE_PEN_COUNT;
        GdkRGBA color = palette_rgba(colors[i]);

        gdk_cairo_set_source_rgba(cr, &color);
        cairo_rectangle(cr, x0, 0, x1 - x0, height);
        cairo_fill(cr);
    }
    return TRUE;
}

static GtkWidget *palette_list_row(const char *name, const uint32_t *colors,
                                   int index)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *swatch = gtk_drawing_area_new();

    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(swatch, 136, 16);
    gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
    g_signal_connect(swatch, "draw", G_CALLBACK(draw_palette_swatch),
                     (gpointer)colors);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), swatch, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data(G_OBJECT(row), "preset-index", GINT_TO_POINTER(index));
    g_object_set_data(G_OBJECT(row), "swatch", swatch);
    return row;
}

/* GtkColorButton emits color-set only for user edits, never for the
   programmatic set_rgba() below, so the user's colours survive a tour of the
   presets without needing to be snapshotted on the way out. */
static void palette_custom_edited(GtkColorButton *button, gpointer data)
{
    struct palette_dialog *dialog = data;
    int i;

    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        GdkRGBA color;

        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog->buttons[i]),
                                   &color);
        dialog->custom[i] = palette_rgb(&color);
    }
    (void)button;
    gtk_widget_queue_draw(dialog->custom_swatch);
}

static void palette_row_selected(GtkListBox *list, GtkListBoxRow *row,
                                 gpointer data)
{
    struct palette_dialog *dialog = data;
    const uint32_t *colors;
    int index;
    int i;

    (void)list;
    if (!row)
        return;
    index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "preset-index"));
    dialog->selected = index;
    colors = index >= 0 ? palette_presets[index].colors : dialog->custom;

    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        GdkRGBA color = palette_rgba(colors[i]);

        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog->buttons[i]),
                                   &color);
    }
    gtk_widget_set_sensitive(dialog->custom_frame, index < 0);
}

static void choose_palette(GtkWidget *widget, gpointer data)
{
    struct console_window *console = data;
    struct palette_dialog state;
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *scroller;
    GtkWidget *list;
    GtkWidget *grid;
    GtkWidget *custom_row;
    uint32_t palette[ACE_CONSOLE_PEN_COUNT];
    int i;

    (void)widget;
    memcpy(state.custom, console->palette, sizeof(state.custom));
    state.selected = -1;
    for (i = 0; i < PALETTE_PRESET_COUNT; i++) {
        if (memcmp(palette_presets[i].colors, console->palette,
                   sizeof(state.custom)) == 0) {
            state.selected = i;
            break;
        }
    }

    dialog = gtk_dialog_new_with_buttons(
        "Choose Palette", GTK_WINDOW(console->window), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Apply", GTK_RESPONSE_OK, NULL);
    gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 520);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 12);

    /* The list is sized to stand on its own rather than to fit its contents:
       it has to be visible without hunting, and scroll rather than grow as
       schemes are added. */
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller),
                                               220);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller),
                                        GTK_SHADOW_IN);
    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_box_pack_start(GTK_BOX(content), scroller, TRUE, TRUE, 0);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    state.custom_frame = gtk_frame_new("Custom colors");
    gtk_container_add(GTK_CONTAINER(state.custom_frame), grid);
    gtk_box_pack_start(GTK_BOX(content), state.custom_frame, FALSE, FALSE, 0);

    /* Two rows of four pens; each pen is a label over its button, so the two
       pen rows occupy four grid rows. */
    for (i = 0; i < ACE_CONSOLE_PEN_COUNT; i++) {
        char label_text[32];
        GtkWidget *label;
        GdkRGBA color = palette_rgba(state.custom[i]);
        int column = i % 4;
        int base = (i / 4) * 2;

        snprintf(label_text, sizeof(label_text), "Pen %d%s", i,
                 i == 0 ? " (bg)" : i == 1 ? " (text)" :
                 i == 7 ? " (cursor)" : "");
        label = gtk_label_new(label_text);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        state.buttons[i] = gtk_color_button_new_with_rgba(&color);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(state.buttons[i]),
                                        FALSE);
        gtk_widget_set_hexpand(state.buttons[i], TRUE);
        g_signal_connect(state.buttons[i], "color-set",
                         G_CALLBACK(palette_custom_edited), &state);
        gtk_grid_attach(GTK_GRID(grid), label, column, base, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), state.buttons[i], column, base + 1,
                        1, 1);
    }

    for (i = 0; i < PALETTE_PRESET_COUNT; i++)
        gtk_container_add(GTK_CONTAINER(list),
                          palette_list_row(palette_presets[i].name,
                                           palette_presets[i].colors, i));
    custom_row = palette_list_row("Custom", state.custom, -1);
    state.custom_swatch = g_object_get_data(G_OBJECT(custom_row), "swatch");
    gtk_container_add(GTK_CONTAINER(list), custom_row);

    g_signal_connect(list, "row-selected", G_CALLBACK(palette_row_selected),
                     &state);
    gtk_list_box_select_row(
        GTK_LIST_BOX(list),
        state.selected >= 0
            ? gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), state.selected)
            : GTK_LIST_BOX_ROW(custom_row));
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        memcpy(palette,
               state.selected >= 0 ? palette_presets[state.selected].colors
                                   : state.custom,
               sizeof(palette));
        if (ace_console_device_set_palette(console->device, palette) != 0) {
            gtk_widget_destroy(dialog);
            show_error(console, "The palette could not be applied to ACE Shell.");
            return;
        }
        memcpy(console->palette, palette, sizeof(console->palette));
        save_config(console);
        refresh_console_title(console);
        gtk_widget_queue_draw(console->drawing_area);
    }
    gtk_widget_destroy(dialog);
}

static void activate_dbus_menu_action(GSimpleAction *action,
                                      GVariant *parameter, gpointer data)
{
    struct console_window *console = data;
    const char *name = g_action_get_name(G_ACTION(action));

    (void)parameter;
    if (strcmp(name, "typeface") == 0)
        choose_font(NULL, console);
    else if (strcmp(name, "palette") == 0)
        choose_palette(NULL, console);
    else if (strcmp(name, "quit") == 0)
        gtk_widget_destroy(console->window);
}

static void add_dbus_menu_action(struct console_window *console,
                                 const char *name)
{
    GSimpleAction *action = g_simple_action_new(name, NULL);

    g_signal_connect(action, "activate", G_CALLBACK(activate_dbus_menu_action),
                     console);
    g_action_map_add_action(G_ACTION_MAP(console->menu_actions),
                            G_ACTION(action));
    g_object_unref(action);
}

static int export_dbus_menu(struct console_window *console)
{
    GMenu *submenu;
    GError *error = NULL;

    console->menu_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!console->menu_bus) {
        fprintf(stderr, "ace-console: cannot connect to session D-Bus: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }
    console->menu_model = g_menu_new();
    submenu = g_menu_new();
    g_menu_append(submenu, "Typeface…", "app.typeface");
    g_menu_append(submenu, "Palette…", "app.palette");
    g_menu_append(submenu, "Quit", "app.quit");
    g_menu_append_submenu(console->menu_model, "ACE Shell",
                          G_MENU_MODEL(submenu));
    g_object_unref(submenu);

    console->menu_actions = g_simple_action_group_new();
    add_dbus_menu_action(console, "typeface");
    add_dbus_menu_action(console, "palette");
    add_dbus_menu_action(console, "quit");
    console->menu_object_path = g_strdup(
        "/org/appmenu/gtk/window/menus/menubar/ace_shell");
    console->menu_model_export_id = g_dbus_connection_export_menu_model(
        console->menu_bus, console->menu_object_path,
        G_MENU_MODEL(console->menu_model), &error);
    if (!console->menu_model_export_id) {
        fprintf(stderr, "ace-console: cannot export menu on D-Bus: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }
    console->menu_action_export_id = g_dbus_connection_export_action_group(
        console->menu_bus, console->menu_object_path,
        G_ACTION_GROUP(console->menu_actions), &error);
    if (!console->menu_action_export_id) {
        fprintf(stderr, "ace-console: cannot export menu actions on D-Bus: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        g_dbus_connection_unexport_menu_model(console->menu_bus,
                                              console->menu_model_export_id);
        console->menu_model_export_id = 0;
        return -1;
    }
    return 0;
}

static void unexport_dbus_menu(struct console_window *console)
{
    if (console->menu_bus) {
        if (console->menu_action_export_id)
            g_dbus_connection_unexport_action_group(
                console->menu_bus, console->menu_action_export_id);
        if (console->menu_model_export_id)
            g_dbus_connection_unexport_menu_model(
                console->menu_bus, console->menu_model_export_id);
    }
    console->menu_action_export_id = 0;
    console->menu_model_export_id = 0;
    g_clear_object(&console->menu_actions);
    g_clear_object(&console->menu_model);
    g_clear_object(&console->menu_bus);
    g_clear_pointer(&console->menu_object_path, g_free);
}

static GtkWidget *build_menu(struct console_window *console)
{
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *root_item = gtk_menu_item_new_with_mnemonic("_ACE Shell");
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *font_item = gtk_menu_item_new_with_mnemonic("_Typeface…");
    GtkWidget *palette_item = gtk_menu_item_new_with_mnemonic("_Palette…");
    GtkWidget *quit_item = gtk_menu_item_new_with_mnemonic("_Quit");

    g_signal_connect(font_item, "activate", G_CALLBACK(choose_font), console);
    g_signal_connect(palette_item, "activate", G_CALLBACK(choose_palette), console);
    g_signal_connect_swapped(quit_item, "activate",
                             G_CALLBACK(gtk_widget_destroy), console->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), font_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), palette_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(root_item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), root_item);
    return bar;
}

static void prepare_appmenu_environment(void)
{
    const char *modules = getenv("GTK_MODULES");
    GString *filtered = g_string_new(NULL);

    if (modules && *modules) {
        char **names = g_strsplit(modules, ":", -1);
        int i;

        for (i = 0; names[i]; i++) {
            if (!names[i][0] || strcmp(names[i], "appmenu-gtk-module") == 0)
                continue;
            if (filtered->len)
                g_string_append_c(filtered, ':');
            g_string_append(filtered, names[i]);
        }
        g_strfreev(names);
    }

    /* ACE exports its own GMenu/GAction pair and advertises that one address
     * over org_kde_kwin_appmenu. Loading appmenu-gtk-module as well creates a
     * second model/action group for the same surface. The compositor stores a
     * single address per view, so the two advertisements race and can leave
     * the displayed menu backed by the module's stale GTK action state. Keep
     * unrelated GTK modules, but make ACE's menu the only provider. */
    setenv("GTK_MODULES", filtered->str, 1);
    g_string_free(filtered, TRUE);
}

static gboolean update_menu_visibility(gpointer data)
{
    struct console_window *console = data;
    GtkSettings *settings;
    gboolean shell_shows_menubar = FALSE;
    GParamSpec *property;

    settings = gtk_widget_get_settings(console->window);
    property = g_object_class_find_property(G_OBJECT_GET_CLASS(settings),
                                             "gtk-shell-shows-menubar");
    if (property)
        g_object_get(settings, "gtk-shell-shows-menubar",
                     &shell_shows_menubar, NULL);

    /* Keep this live. The compositor can appear after the window, and a
     * compositor restart invalidates the previous Wayland advertisement. */
    console->menu_wayland_live = ace_appmenu_wayland_advertise(
        GTK_WINDOW(console->window), console->menu_bus,
        console->menu_object_path);
    if (console->menu_wayland_live || shell_shows_menubar)
        gtk_widget_hide(console->menu_bar);
    else
        gtk_widget_show(console->menu_bar);

    if (gtk_widget_get_mapped(console->window) &&
        !console->menu_type_hint_restored) {
        /* The installed appmenu module only inspects NORMAL/DIALOG windows
         * during its pre-realize hook. UTILITY avoids its premature private
         * GDK call; by this timer the real window exists, so restore the
         * normal ACE Shell type hint. */
        gtk_window_set_type_hint(GTK_WINDOW(console->window),
                                 GDK_WINDOW_TYPE_HINT_NORMAL);
        console->menu_type_hint_restored = TRUE;
    }
    return G_SOURCE_CONTINUE;
}

/*
 * Repaint only the rectangle the console actually drew into. A console spends
 * most of its time changing one line or one cell, and asking GTK to redraw
 * the whole window for that means handing the compositor a full window's
 * worth of pixels on every frame.
 */
static gboolean flush_console_damage(gpointer data)
{
    struct console_window *console = data;
    int x;
    int y;
    int width;
    int height;

    console->damage_source = 0;
    /* Output continues through the live device while a frozen historical
     * surface is displayed. Keep its damage pending until a key returns to
     * the live view. */
    if (ace_console_device_scrollback_active(console->device))
        return G_SOURCE_REMOVE;
    if (ace_console_device_take_damage(console->device, &x, &y, &width,
                                       &height))
        gtk_widget_queue_draw_area(console->drawing_area, x, y, width, height);
    return G_SOURCE_REMOVE;
}

static void queue_console_damage(struct console_window *console)
{
    /* Parsing and the backing surface stay current for every byte, but only
     * present the newest surface once per frame.  Fast output can therefore
     * cross several complete pages without making GTK paint each scroll. */
    console->damage_source = ace_console_dispatch_schedule_frame(
        console->damage_source, flush_console_damage, console);
}

static void draw_text_overlay(struct console_window *console, cairo_t *cr,
                              int width, int height, const char *text,
                              gboolean bottom)
{
    PangoLayout *layout;
    PangoFontDescription *font;
    GdkRGBA background = palette_rgba(console->palette[1]);
    GdkRGBA foreground = palette_rgba(console->palette[0]);
    int text_width;
    int text_height;
    int overlay_height;
    int y;

    layout = pango_cairo_create_layout(cr);
    font = pango_font_description_new();
    pango_font_description_set_family(font,
                                      console->font_family
                                          ? console->font_family
                                          : "monospace");
    pango_font_description_set_absolute_size(
        font, font_pixels(console->font_points) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, MAX(1, width - 12) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    overlay_height = text_height + 8;
    y = bottom ? height - overlay_height : 0;
    if (y < 0)
        y = 0;

    gdk_cairo_set_source_rgba(cr, &background);
    cairo_rectangle(cr, 0, y, width, overlay_height);
    cairo_fill(cr);
    gdk_cairo_set_source_rgba(cr, &foreground);
    cairo_move_to(cr, 6, y + 4);
    pango_cairo_show_layout(cr, layout);

    (void)text_width;
    pango_font_description_free(font);
    g_object_unref(layout);
}

static void draw_selection(struct console_window *console, cairo_t *cr,
                           int width, int height)
{
    GdkRGBA color = palette_rgba(console->palette[7]);
    int cell_width;
    int cell_height;
    int columns;
    int rows;
    int start_column;
    int start_row;
    int end_column;
    int end_row;

    if (!console->selection_valid ||
        !ace_console_device_scrollback_active(console->device) ||
        ace_console_device_cell_size(console->device, &cell_width,
                                     &cell_height) != 0)
        return;
    columns = width / cell_width;
    rows = height / cell_height;
    if (columns < 1 || rows < 1)
        return;

    start_column = console->selection_start_column;
    start_row = console->selection_start_row;
    end_column = console->selection_end_column;
    end_row = console->selection_end_row;
    if (start_row > end_row ||
        (start_row == end_row && start_column > end_column)) {
        int temporary = start_column;

        start_column = end_column;
        end_column = temporary;
        temporary = start_row;
        start_row = end_row;
        end_row = temporary;
    }
    if (start_column < 0)
        start_column = 0;
    if (end_column >= columns)
        end_column = columns - 1;
    if (start_row < 0)
        start_row = 0;
    if (end_row >= rows)
        end_row = rows - 1;
    if (start_row > end_row || start_column > end_column)
        return;

    color.alpha = 0.45;
    gdk_cairo_set_source_rgba(cr, &color);
    for (int row = start_row; row <= end_row; row++) {
        int first = row == start_row ? start_column : 0;
        int last = row == end_row ? end_column : columns - 1;

        cairo_rectangle(cr, first * cell_width, row * cell_height,
                        (last - first + 1) * cell_width, cell_height);
    }
    cairo_fill(cr);
}

static void draw_scrollback_frame(struct console_window *console, cairo_t *cr,
                                  int width, int height)
{
    GdkRGBA color = palette_rgba(console->palette[2]);
    const double line_width = 2.0;

    if (width <= (int)line_width || height <= (int)line_width)
        return;
    /* Keep the indicator thinner than a character cell and inside the
     * viewport, so it identifies the whole console without hiding text. */
    color.alpha = 0.5;
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_set_line_width(cr, line_width);
    cairo_rectangle(cr, line_width / 2.0, line_width / 2.0,
                    width - line_width, height - line_width);
    cairo_stroke(cr);
}

static gboolean draw_console(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    struct console_window *console = data;
    int scrollback_active =
        ace_console_device_scrollback_active(console->device);
    cairo_surface_t *surface = scrollback_active
                                   ? ace_console_device_scrollback_surface(
                                         console->device)
                                   : ace_console_device_surface(console->device);
    GdkRGBA background = palette_rgba(console->palette[0]);
    GtkAllocation allocation;
    int origin_y = scrollback_active
                       ? ace_console_device_scrollback_origin_y(console->device)
                       : ace_console_device_origin_y(console->device);
    int width;
    int height;

    if (!surface)
        return FALSE;
    ace_console_device_size(console->device, &width, &height);
    gtk_widget_get_allocation(widget, &allocation);

    /* A resize enlarges the widget before the console has caught up with it.
     * Paint the shortfall in the background pen so the gap reads as empty
     * console rather than as whatever the compositor last had there. */
    if (width < allocation.width || height < allocation.height) {
        cairo_set_source_rgb(cr, background.red, background.green,
                             background.blue);
        if (width < allocation.width)
            cairo_rectangle(cr, width, 0, allocation.width - width,
                            allocation.height);
        if (height < allocation.height)
            cairo_rectangle(cr, 0, height, width, allocation.height - height);
        cairo_fill(cr);
    }

    /*
     * The surface carries growth slack and scroll headroom around the
     * console, so the console's own rows start at origin_y and the blit is
     * clipped to the console's extent.
     */
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_set_source_surface(cr, surface, 0, -origin_y);
    cairo_paint(cr);
    cairo_restore(cr);
    draw_selection(console, cr, width, height);
    if (console->copy_status[0] != '\0')
        draw_text_overlay(console, cr, width, height, console->copy_status,
                          TRUE);
    if (scrollback_active)
        draw_scrollback_frame(console, cr, width, height);
    return FALSE;
}

static gboolean apply_pending_resize(gpointer data)
{
    struct console_window *console = data;
    int width = console->pending_width;
    int height = console->pending_height;

    console->resize_source = 0;
    if (width <= 0 || height <= 0)
        return G_SOURCE_REMOVE;
    if (ace_console_device_resize(console->device, width, height) != 0) {
        fprintf(stderr, "ace-console: failed to resize console.device to %dx%d\n",
                width, height);
        return G_SOURCE_REMOVE;
    }
    update_channel_geometry(console, TRUE);
    ace_console_device_notify_resize(console->device);
    refresh_console_title(console);
    gtk_widget_queue_draw(console->drawing_area);
    return G_SOURCE_REMOVE;
}

static void refresh_console_title(struct console_window *console)
{
    const char *base = console->current_title[0] != '\0'
                           ? console->current_title
                           : "ACE Shell";
    char title[sizeof(console->current_title) + 96];

    if (ace_console_device_scrollback_active(console->device)) {
        snprintf(title, sizeof(title), "%s — SCROLLBACK (%d lines back)", base,
                 ace_console_device_scrollback_lines(console->device));
        gtk_window_set_title(GTK_WINDOW(console->window), title);
    } else {
        gtk_window_set_title(GTK_WINDOW(console->window), base);
    }
}

static void set_console_title(struct console_window *console)
{
    console->title_buffer[console->title_length] = '\0';
    g_strlcpy(console->current_title, console->title_buffer,
              sizeof(console->current_title));
    refresh_console_title(console);
}

static void output_console_byte(struct console_window *console,
                                unsigned char byte, unsigned char *ordinary,
                                size_t *ordinary_length)
{
    if (*ordinary_length == 8192) {
        ace_console_device_write(console->device, ordinary, *ordinary_length);
        *ordinary_length = 0;
    }
    ordinary[(*ordinary_length)++] = byte;
}

/* Strip ACE's private OSC 2 title messages before the bytes reach the AROS
 * console parser. All other bytes, including ANSI/CSI sequences, are passed
 * through unchanged. The state is kept on the window because a socket read
 * can split a control sequence at any byte. */
static void output_console(struct console_window *console,
                           const unsigned char *data, size_t length)
{
    unsigned char ordinary[8192];
    size_t ordinary_length = 0;

    while (length-- != 0) {
        unsigned char byte = *data++;

        switch (console->title_state) {
        case 0:
            if (byte == 0x1b)
                console->title_state = 1;
            else
                output_console_byte(console, byte, ordinary,
                                    &ordinary_length);
            break;
        case 1:
            if (byte == ']') {
                console->title_state = 2;
            } else {
                output_console_byte(console, 0x1b, ordinary,
                                    &ordinary_length);
                output_console_byte(console, byte, ordinary,
                                    &ordinary_length);
                console->title_state = 0;
            }
            break;
        case 2:
            if (byte == '2') {
                console->title_state = 3;
            } else {
                output_console_byte(console, 0x1b, ordinary,
                                    &ordinary_length);
                output_console_byte(console, ']', ordinary, &ordinary_length);
                output_console_byte(console, byte, ordinary,
                                    &ordinary_length);
                console->title_state = 0;
            }
            break;
        case 3:
            if (byte == ';') {
                console->title_length = 0;
                console->title_state = 4;
            } else {
                output_console_byte(console, 0x1b, ordinary,
                                    &ordinary_length);
                output_console_byte(console, ']', ordinary, &ordinary_length);
                output_console_byte(console, '2', ordinary, &ordinary_length);
                output_console_byte(console, byte, ordinary,
                                    &ordinary_length);
                console->title_state = 0;
            }
            break;
        default:
            if (byte == '\a') {
                set_console_title(console);
                console->title_state = 0;
            } else if (byte == 0x1b ||
                       console->title_length + 1 >=
                           sizeof(console->title_buffer)) {
                /* Invalid or overlong titles are rendered literally instead
                 * of swallowing the rest of the program's output. */
                output_console_byte(console, 0x1b, ordinary,
                                    &ordinary_length);
                output_console_byte(console, ']', ordinary, &ordinary_length);
                output_console_byte(console, '2', ordinary, &ordinary_length);
                output_console_byte(console, ';', ordinary, &ordinary_length);
                for (size_t index = 0; index < console->title_length; index++)
                    output_console_byte(console,
                                        (unsigned char)console->title_buffer[index],
                                        ordinary, &ordinary_length);
                output_console_byte(console, byte, ordinary, &ordinary_length);
                console->title_state = 0;
            } else {
                console->title_buffer[console->title_length++] = (char)byte;
            }
            break;
        }
    }
    if (ordinary_length != 0)
        ace_console_device_write(console->device, ordinary, ordinary_length);
}

static void drawing_area_size_allocate(GtkWidget *widget,
                                       GtkAllocation *allocation,
                                       gpointer data)
{
    struct console_window *console = data;

    if (allocation->width <= 0 || allocation->height <= 0)
        return;
    console->pending_width = allocation->width;
    console->pending_height = allocation->height;
    if (console->resize_source != 0)
        g_source_remove(console->resize_source);
    /* A live resize delivers a stream of allocations, and each one puts the
     * console's whole character grid through AROS's geometry path. Coalesce
     * them to about one frame so the console tracks the drag closely without
     * running that path several times per frame; draw_console() paints the
     * background into whatever the console has not caught up with yet. */
    console->resize_source = g_timeout_add(16, apply_pending_resize, console);
    (void)widget;
}

static int send_input(struct console_window *console, const void *data,
                      size_t length)
{
    return ace_console_channel_send(&console->channel, data, length);
}

static void clear_selection(struct console_window *console)
{
    console->selection_dragging = FALSE;
    console->selection_valid = FALSE;
}

static gboolean clear_copy_status(gpointer data)
{
    struct console_window *console = data;

    console->copy_status_source = 0;
    console->copy_status[0] = '\0';
    gtk_widget_queue_draw(console->drawing_area);
    return G_SOURCE_REMOVE;
}

static void show_copy_status(struct console_window *console,
                             const char *text, size_t length)
{
    size_t characters = length;

    if (g_utf8_validate(text, (gssize)length, NULL))
        characters = (size_t)g_utf8_strlen(text, (gssize)length);
    if (console->copy_status_source != 0)
        g_source_remove(console->copy_status_source);
    snprintf(console->copy_status, sizeof(console->copy_status),
             "%zu characters copied to buffer", characters);
    console->copy_status_source = g_timeout_add(2500, clear_copy_status,
                                                console);
    gtk_widget_queue_draw(console->drawing_area);
}

static int write_clipboard_text(const char *text, size_t length)
{
    struct IFFHandle *iff = NULL;
    struct ClipboardHandle *clipboard = NULL;
    int result = -1;

    if (!text || length > (size_t)LONG_MAX)
        return -1;
    iff = AllocIFF();
    if (!iff)
        goto done;
    clipboard = OpenClipboard(0);
    if (!clipboard)
        goto done;
    iff->iff_Stream = (IPTR)clipboard;
    InitIFFasClip(iff);
    if (OpenIFF(iff, IFFF_WRITE) != 0 ||
        PushChunk(iff, ID_FTXT, ID_FORM, IFFSIZE_UNKNOWN) != 0 ||
        PushChunk(iff, 0, ID_CHRS, IFFSIZE_UNKNOWN) != 0 ||
        WriteChunkBytes(iff, (APTR)text, (LONG)length) != (LONG)length ||
        PopChunk(iff) != 0 || PopChunk(iff) != 0)
        goto close_iff;
    result = 0;

close_iff:
    CloseIFF(iff);
done:
    if (clipboard)
        CloseClipboard(clipboard);
    if (iff)
        FreeIFF(iff);
    return result;
}

static char *read_clipboard_text(size_t *length_out)
{
    struct IFFHandle *iff = NULL;
    struct ClipboardHandle *clipboard = NULL;
    struct ContextNode *chunk;
    LONG stops[] = {ID_FTXT, ID_CHRS};
    char *text = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int found = 0;
    int success = 0;

    if (length_out)
        *length_out = 0;
    iff = AllocIFF();
    if (!iff)
        goto done;
    clipboard = OpenClipboard(0);
    if (!clipboard)
        goto done;
    iff->iff_Stream = (IPTR)clipboard;
    InitIFFasClip(iff);
    if (OpenIFF(iff, IFFF_READ) != 0 ||
        StopChunk(iff, ID_FTXT, ID_CHRS) != 0)
        goto close_iff;
    for (;;) {
        LONG error = ParseIFF(iff, IFFPARSE_SCAN);

        if (error == IFFERR_EOF)
            break;
        if (error == IFFERR_EOC)
            continue;
        if (error != 0)
            goto close_iff;
        chunk = CurrentChunk(iff);
        if (!chunk || chunk->cn_Type != stops[0] ||
            chunk->cn_ID != stops[1])
            continue;
        found = 1;
        while (chunk->cn_Size > 0) {
            unsigned char buffer[4096];
            size_t remaining = (size_t)chunk->cn_Size;
            size_t wanted = remaining < sizeof(buffer) ? remaining :
                            sizeof(buffer);
            LONG actual = ReadChunkBytes(iff, buffer, (LONG)wanted);

            if (actual != (LONG)wanted ||
                length > SIZE_MAX - (size_t)actual - 1)
                goto close_iff;
            if (length + (size_t)actual + 1 > capacity) {
                size_t new_capacity = capacity ? capacity : 256;
                char *grown;

                while (new_capacity < length + (size_t)actual + 1) {
                    if (new_capacity > SIZE_MAX / 2)
                        goto close_iff;
                    new_capacity *= 2;
                }
                /* Not onto text itself: realloc() returning NULL leaves the
                   old block allocated, and assigning over the only pointer
                   to it is how it would be lost. */
                grown = realloc(text, new_capacity);
                if (!grown)
                    goto close_iff;
                text = grown;
                capacity = new_capacity;
            }
            memcpy(text + length, buffer, (size_t)actual);
            length += (size_t)actual;
            text[length] = '\0';
            chunk->cn_Size -= actual;
        }
    }
    if (!found)
        goto close_iff;
    if (!text) {
        text = malloc(1);
        if (!text)
            goto close_iff;
        text[0] = '\0';
    }
    if (length_out)
        *length_out = length;
    success = 1;

close_iff:
    CloseIFF(iff);
done:
    if (clipboard)
        CloseClipboard(clipboard);
    if (iff)
        FreeIFF(iff);
    if (!success) {
        free(text);
        text = NULL;
        if (length_out)
            *length_out = 0;
    }
    return text;
}

static void put_clipboard(struct console_window *console, char *text,
                          size_t length)
{
    if (!text)
        return;
    if (write_clipboard_text(text, length) == 0)
        show_copy_status(console, text, length);
    free(text);
}

static void copy_all(struct console_window *console)
{
    size_t length = 0;

    put_clipboard(console,
                  ace_console_device_copy_all(console->device, &length),
                  length);
}

static void copy_selection(struct console_window *console)
{
    size_t length = 0;

    if (!console->selection_valid)
        return;
    put_clipboard(
        console,
        ace_console_device_copy_selection(
            console->device, console->selection_start_column,
            console->selection_start_row, console->selection_end_column,
            console->selection_end_row, &length),
        length);
}

static void paste_clipboard(struct console_window *console)
{
    size_t length = 0;
    char *text = read_clipboard_text(&length);

    if (!text)
        return;
    (void)send_input(console, text, length);
    free(text);
}

static void leave_scrollback(struct console_window *console)
{
    clear_selection(console);
    if (!ace_console_device_scrollback_active(console->device))
        return;
    ace_console_device_clear_scrollback(console->device);
    refresh_console_title(console);
    gtk_widget_queue_draw(console->drawing_area);
}

static void toggle_fullscreen(struct console_window *console)
{
    if (console->fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(console->window));
        console->fullscreen = FALSE;
    } else {
        gtk_window_fullscreen(GTK_WINDOW(console->window));
        console->fullscreen = TRUE;
    }
}

/*
 * The console input codes an Amiga program expects for the keys that are not
 * characters. These are the console.device raw sequences, the same ones the
 * Amiga terminal description in an unmodified program's own key table is
 * written against -- Vim's builtin_amiga[] in term.c, for instance. The
 * shifted arrows and the 101-key block matter as soon as a full-screen
 * program is running: without them those keys reach the program as nothing
 * at all.
 */
struct console_key {
    guint keyval;
    guint modifiers;
    const char *sequence;
};

#define KEY_ANY_MODIFIERS ((guint)-1)

static const struct console_key console_keys[] = {
    { GDK_KEY_Return,        KEY_ANY_MODIFIERS, "\n" },
    { GDK_KEY_KP_Enter,      KEY_ANY_MODIFIERS, "\n" },
    { GDK_KEY_BackSpace,     KEY_ANY_MODIFIERS, "\b" },
    { GDK_KEY_Delete,        KEY_ANY_MODIFIERS, "\177" },
    { GDK_KEY_Escape,        KEY_ANY_MODIFIERS, "\033" },
    { GDK_KEY_Tab,           0,                 "\t" },
    { GDK_KEY_Tab,           GDK_SHIFT_MASK,    "\233Z" },
    { GDK_KEY_ISO_Left_Tab,  KEY_ANY_MODIFIERS, "\233Z" },
    { GDK_KEY_Up,            GDK_SHIFT_MASK,    "\233T" },
    { GDK_KEY_Down,          GDK_SHIFT_MASK,    "\233S" },
    { GDK_KEY_Left,          GDK_SHIFT_MASK,    "\233 A" },
    { GDK_KEY_Right,         GDK_SHIFT_MASK,    "\233 @" },
    { GDK_KEY_Up,            KEY_ANY_MODIFIERS, "\233A" },
    { GDK_KEY_Down,          KEY_ANY_MODIFIERS, "\233B" },
    { GDK_KEY_Right,         KEY_ANY_MODIFIERS, "\233C" },
    { GDK_KEY_Left,          KEY_ANY_MODIFIERS, "\233D" },
    { GDK_KEY_Insert,        GDK_SHIFT_MASK,    "\23350~" },
    { GDK_KEY_Home,          GDK_SHIFT_MASK,    "\23354~" },
    { GDK_KEY_End,           GDK_SHIFT_MASK,    "\23355~" },
    { GDK_KEY_Insert,        KEY_ANY_MODIFIERS, "\23340~" },
    { GDK_KEY_Page_Up,       KEY_ANY_MODIFIERS, "\23341~" },
    { GDK_KEY_Page_Down,     KEY_ANY_MODIFIERS, "\23342~" },
    { GDK_KEY_Home,          KEY_ANY_MODIFIERS, "\23344~" },
    { GDK_KEY_End,           KEY_ANY_MODIFIERS, "\23345~" },
    { GDK_KEY_Help,          KEY_ANY_MODIFIERS, "\233?~" },
    { GDK_KEY_F1,            GDK_SHIFT_MASK,    "\23310~" },
    { GDK_KEY_F2,            GDK_SHIFT_MASK,    "\23311~" },
    { GDK_KEY_F3,            GDK_SHIFT_MASK,    "\23312~" },
    { GDK_KEY_F4,            GDK_SHIFT_MASK,    "\23313~" },
    { GDK_KEY_F5,            GDK_SHIFT_MASK,    "\23314~" },
    { GDK_KEY_F6,            GDK_SHIFT_MASK,    "\23315~" },
    { GDK_KEY_F7,            GDK_SHIFT_MASK,    "\23316~" },
    { GDK_KEY_F8,            GDK_SHIFT_MASK,    "\23317~" },
    { GDK_KEY_F9,            GDK_SHIFT_MASK,    "\23318~" },
    { GDK_KEY_F10,           GDK_SHIFT_MASK,    "\23319~" },
    { GDK_KEY_F1,            KEY_ANY_MODIFIERS, "\2330~" },
    { GDK_KEY_F2,            KEY_ANY_MODIFIERS, "\2331~" },
    { GDK_KEY_F3,            KEY_ANY_MODIFIERS, "\2332~" },
    { GDK_KEY_F4,            KEY_ANY_MODIFIERS, "\2333~" },
    { GDK_KEY_F5,            KEY_ANY_MODIFIERS, "\2334~" },
    { GDK_KEY_F6,            KEY_ANY_MODIFIERS, "\2335~" },
    { GDK_KEY_F7,            KEY_ANY_MODIFIERS, "\2336~" },
    { GDK_KEY_F8,            KEY_ANY_MODIFIERS, "\2337~" },
    { GDK_KEY_F9,            KEY_ANY_MODIFIERS, "\2338~" },
    { GDK_KEY_F10,           KEY_ANY_MODIFIERS, "\2339~" },
};

/*
 * Ctrl with a key produces the control character that key names, which is
 * how a program reads Ctrl-C, and equally how it reads the Ctrl-W, Ctrl-R
 * and Ctrl-V an editor is driven with. Only Ctrl-C and Ctrl-D used to reach
 * the shell, and every other Ctrl chord was swallowed here.
 */
static int control_character(guint key)
{
    if (key >= GDK_KEY_a && key <= GDK_KEY_z)
        return (int)(key - GDK_KEY_a) + 1;
    if (key >= GDK_KEY_A && key <= GDK_KEY_Z)
        return (int)(key - GDK_KEY_A) + 1;
    switch (key) {
    case GDK_KEY_at:
    case GDK_KEY_space:
    case GDK_KEY_2:
        return 0;
    case GDK_KEY_bracketleft:
    case GDK_KEY_3:
        return 033;
    case GDK_KEY_backslash:
    case GDK_KEY_4:
        return 034;
    case GDK_KEY_bracketright:
    case GDK_KEY_5:
        return 035;
    case GDK_KEY_asciicircum:
    case GDK_KEY_6:
        return 036;
    case GDK_KEY_underscore:
    case GDK_KEY_minus:
    case GDK_KEY_7:
        return 037;
    case GDK_KEY_question:
        return 0177;
    default:
        return -1;
    }
}

static gboolean key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct console_window *console = data;
    guint key = event->keyval;
    guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    char utf8[8];
    gunichar unicode;

    (void)widget;
    if ((modifiers & GDK_CONTROL_MASK) &&
        (key == GDK_KEY_c || key == GDK_KEY_C) &&
        ace_console_device_scrollback_active(console->device)) {
        if (console->selection_valid)
            copy_selection(console);
        else
            copy_all(console);
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) &&
        (key == GDK_KEY_v || key == GDK_KEY_V)) {
        leave_scrollback(console);
        paste_clipboard(console);
        return TRUE;
    }
    if (key == GDK_KEY_F12 && modifiers == 0) {
        copy_all(console);
        leave_scrollback(console);
        return TRUE;
    }
    leave_scrollback(console);
    if (key == GDK_KEY_F11 && modifiers == 0) {
        toggle_fullscreen(console);
        return TRUE;
    }
    for (size_t index = 0; index < G_N_ELEMENTS(console_keys); index++) {
        const struct console_key *entry = &console_keys[index];

        if (entry->keyval != key)
            continue;
        if (entry->modifiers != KEY_ANY_MODIFIERS &&
            entry->modifiers != modifiers)
            continue;
        (void)send_input(console, entry->sequence, strlen(entry->sequence));
        return TRUE;
    }
    if (modifiers & GDK_CONTROL_MASK) {
        int character = control_character(key);

        if (character >= 0) {
            if (character == 3) {
                /* Ctrl-C is an AmigaDOS break request, not a byte to leave
                   queued behind a running foreground command.  The shell
                   forwards SIGUSR1 to that command, whose ACE runtime turns
                   it into SIGBREAKF_CTRL_C. */
                if (console->controller_pid > 0)
                    (void)ace_console_dispatch_control(
                        console->controller_pid, character);
                return TRUE;
            }
            if (character == 4) {
                /* Ctrl-D is the shell's script-break request.  It is not
                   input for the foreground command: Shell.c observes the
                   resulting SIGBREAKF_CTRL_D after that command completes. */
                if (console->controller_pid > 0)
                    (void)ace_console_dispatch_control(
                        console->controller_pid, character);
                return TRUE;
            }
            if (character == 5 || character == 6) {
                /* Ctrl-E and Ctrl-F are application-defined Amiga break
                   signals.  Send them to the shell, which brokers them to
                   its foreground task (or retains them for itself). */
                if (console->controller_pid > 0)
                    (void)ace_console_dispatch_control(
                        console->controller_pid, character);
                return TRUE;
            }
            char byte = (char)character;

            (void)send_input(console, &byte, 1);
        }
        return TRUE;
    }

    unicode = gdk_keyval_to_unicode(key);
    if (unicode >= 0x20 && unicode != 0x7f &&
        g_unichar_validate(unicode) &&
        g_unichar_to_utf8(unicode, utf8) < (gint)sizeof(utf8)) {
        int length = g_unichar_to_utf8(unicode, utf8);
        (void)send_input(console, utf8, (size_t)length);
        return TRUE;
    }
    return FALSE;
}

#define SCROLLBACK_LINES_PER_WHEEL 3

static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *event,
                             gpointer data)
{
    struct console_window *console = data;
    int direction = 0;
    int current;
    int requested;

    (void)widget;
    if (event->direction == GDK_SCROLL_UP ||
        (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0.0))
        direction = 1;
    else if (event->direction == GDK_SCROLL_DOWN ||
             (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0.0))
        direction = -1;
    if (direction == 0)
        return TRUE;

    clear_selection(console);
    if (!ace_console_device_scrollback_active(console->device)) {
        if (direction < 0)
            return TRUE;
        (void)ace_console_device_set_scrollback(console->device, 0);
        refresh_console_title(console);
        gtk_widget_queue_draw(console->drawing_area);
        return TRUE;
    }
    current = ace_console_device_scrollback_lines(console->device);
    requested = current + direction * SCROLLBACK_LINES_PER_WHEEL;
    if (requested <= 0)
        ace_console_device_clear_scrollback(console->device);
    else
        (void)ace_console_device_set_scrollback(console->device, requested);
    refresh_console_title(console);
    gtk_widget_queue_draw(console->drawing_area);
    return TRUE;
}

static void pointer_cell(struct console_window *console, gdouble x, gdouble y,
                         int *column_out, int *row_out)
{
    int cell_width;
    int cell_height;
    int columns;
    int rows;

    if (ace_console_device_cell_size(console->device, &cell_width,
                                     &cell_height) != 0) {
        *column_out = 0;
        *row_out = 0;
        return;
    }
    columns = gtk_widget_get_allocated_width(console->drawing_area) / cell_width;
    rows = gtk_widget_get_allocated_height(console->drawing_area) / cell_height;
    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;
    *column_out = x < 0 ? 0 : (int)(x / cell_width);
    *row_out = y < 0 ? 0 : (int)(y / cell_height);
    if (*column_out >= columns)
        *column_out = columns - 1;
    if (*row_out >= rows)
        *row_out = rows - 1;
}

static gboolean button_press(GtkWidget *widget, GdkEventButton *event,
                             gpointer data)
{
    struct console_window *console = data;

    if (event->button != 1 ||
        !ace_console_device_scrollback_active(console->device))
        return FALSE;
    pointer_cell(console, event->x, event->y,
                 &console->selection_start_column,
                 &console->selection_start_row);
    console->selection_end_column = console->selection_start_column;
    console->selection_end_row = console->selection_start_row;
    console->selection_dragging = TRUE;
    console->selection_valid = FALSE;
    gtk_widget_grab_focus(widget);
    return TRUE;
}

static gboolean motion_notify(GtkWidget *widget, GdkEventMotion *event,
                              gpointer data)
{
    struct console_window *console = data;
    int column;
    int row;

    if (!console->selection_dragging)
        return FALSE;
    pointer_cell(console, event->x, event->y, &column, &row);
    console->selection_end_column = column;
    console->selection_end_row = row;
    console->selection_valid =
        column != console->selection_start_column ||
        row != console->selection_start_row;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean button_release(GtkWidget *widget, GdkEventButton *event,
                               gpointer data)
{
    struct console_window *console = data;
    int column;
    int row;

    if (event->button != 1 || !console->selection_dragging)
        return FALSE;
    pointer_cell(console, event->x, event->y, &column, &row);
    console->selection_end_column = column;
    console->selection_end_row = row;
    console->selection_valid =
        column != console->selection_start_column ||
        row != console->selection_start_row;
    console->selection_dragging = FALSE;
    if (console->selection_valid)
        copy_selection(console);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

/*
 * How much shell output one main-loop turn will absorb. Draining the socket
 * in one pass is what keeps a burst of output from costing a console write
 * and a repaint per 4KB the socket happened to deliver, but an unbounded
 * drain would let a program that prints without pause hold the main loop --
 * and with it the keyboard and the window -- for as long as it kept
 * printing. At this size the loop comes back for air several times a second
 * even under a flood.
 */
#define OUTPUT_DRAIN_MAX (16 * 1024)
/* A minimum spacing between snapshot opportunities, not a promised frame
 * rate. Low-priority output parsing and GTK events remain free to run first. */
#define OVERLOAD_QUIET_MILLISECONDS 16

static int queued_output_bytes(struct console_window *console)
{
    int pending = 0;

    if (console->stream_fd < 0 ||
        ioctl(console->stream_fd, FIONREAD, &pending) != 0)
        return 0;
    return pending > 0 ? pending : 0;
}

static gboolean finish_overload_frame(gpointer data)
{
    struct console_window *console = data;

    console->overload_settle_source = 0;
    if (queued_output_bytes(console) != 0) {
        console->overload_settle_source = g_timeout_add_full(
            G_PRIORITY_LOW, OVERLOAD_QUIET_MILLISECONDS,
            finish_overload_frame, console, NULL);
        return G_SOURCE_REMOVE;
    }
    if (console->overload_frame_source != 0) {
        g_source_remove(console->overload_frame_source);
        console->overload_frame_source = 0;
    }
    if (console->presented_grid_fingerprint !=
        ace_console_device_grid_fingerprint(console->device)) {
        ace_console_device_render_grid(console->device);
        console->presented_grid_fingerprint =
            ace_console_device_grid_fingerprint(console->device);
        (void)flush_console_damage(console);
    }
    console->render_deferred = FALSE;
    ace_console_device_set_render_deferred(console->device, FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean present_overload_frame(gpointer data)
{
    struct console_window *console = data;

    if (!console->render_deferred) {
        console->overload_frame_source = 0;
        return G_SOURCE_REMOVE;
    }
    if (console->presented_grid_fingerprint !=
        ace_console_device_grid_fingerprint(console->device)) {
        ace_console_device_render_grid(console->device);
        console->presented_grid_fingerprint =
            ace_console_device_grid_fingerprint(console->device);
        (void)flush_console_damage(console);
    }
    return G_SOURCE_CONTINUE;
}

static void arm_overload_settle(struct console_window *console)
{
    if (console->overload_settle_source != 0)
        g_source_remove(console->overload_settle_source);
    console->overload_settle_source = g_timeout_add_full(
        G_PRIORITY_LOW, OVERLOAD_QUIET_MILLISECONDS,
        finish_overload_frame, console, NULL);
}

static void begin_overload_if_needed(struct console_window *console)
{
    size_t capacity;

    if (console->render_deferred)
        return;
    capacity = ace_console_device_grid_capacity(console->device);
    if (capacity == 0 || (size_t)queued_output_bytes(console) <= capacity)
        return;
    console->render_deferred = TRUE;
    console->presented_grid_fingerprint =
        ace_console_device_grid_fingerprint(console->device);
    ace_console_device_set_render_deferred(console->device, TRUE);
    if (console->damage_source != 0) {
        g_source_remove(console->damage_source);
        console->damage_source = 0;
    }
    if (console->overload_frame_source == 0)
        console->overload_frame_source = g_timeout_add_full(
            G_PRIORITY_LOW, OVERLOAD_QUIET_MILLISECONDS,
            present_overload_frame, console, NULL);
}

static gboolean read_console(GIOChannel *channel, GIOCondition condition,
                             gpointer data)
{
    struct console_window *console = data;
    char buffer[8192];
    size_t drained = 0;
    int closed = 0;

    (void)channel;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        if (console->window)
            gtk_widget_destroy(console->window);
        return G_SOURCE_REMOVE;
    }

    begin_overload_if_needed(console);

    while (drained < OUTPUT_DRAIN_MAX) {
        ssize_t length = ace_console_channel_receive(&console->channel,
                                                     buffer, sizeof(buffer));

        if (length > 0) {
            /*
             * The real entry point console.c's beginio()/CMD_WRITE would
             * call. ACE's rendering path never goes through DoIO()/BeginIO()
             * -- see HANDOFF.md -- so this calls the real ANSI/CSI parser
             * directly with the same arguments beginio() would have passed
             * it.
             */
            output_console(console, (const unsigned char *)buffer,
                           (size_t)length);
            drained += (size_t)length;
            continue;
        }
        if (length == 0) {
            closed = 1;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        closed = 1;
        break;
    }

    if (drained != 0) {
        if (console->render_deferred)
            arm_overload_settle(console);
        else
            queue_console_damage(console);
    }
    if (closed) {
        if (console->window)
            gtk_widget_destroy(console->window);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void console_destroy(GtkWidget *widget, gpointer data)
{
    struct console_window *console = data;
    int status;

    (void)widget;
    ace_requestor_gui_stop();
    if (console->menu_probe_source != 0) {
        g_source_remove(console->menu_probe_source);
        console->menu_probe_source = 0;
    }
    if (console->resize_source != 0) {
        g_source_remove(console->resize_source);
        console->resize_source = 0;
    }
    if (console->damage_source != 0) {
        g_source_remove(console->damage_source);
        console->damage_source = 0;
    }
    if (console->overload_settle_source != 0) {
        g_source_remove(console->overload_settle_source);
        console->overload_settle_source = 0;
    }
    if (console->overload_frame_source != 0) {
        g_source_remove(console->overload_frame_source);
        console->overload_frame_source = 0;
    }
    if (console->copy_status_source != 0) {
        g_source_remove(console->copy_status_source);
        console->copy_status_source = 0;
    }
    ace_appmenu_wayland_forget();
    if (console->child_pid > 0) {
        /* Closing the console window is what closing a console window means
         * for everything running under it, not just for the shell: the
         * commands the shell started have lost their console too. The child
         * is a process group leader (see main()), so this reaches a
         * full-screen program that the shell is currently waiting on --
         * which would otherwise be left reading a console that can never
         * produce another byte. */
        if (kill(-console->child_pid, SIGHUP) != 0)
            (void)kill(console->child_pid, SIGHUP);
        (void)waitpid(console->child_pid, &status, 0);
    }
    if (console->stream_fd >= 0)
        close(console->stream_fd);
    ace_console_channel_close(&console->channel);
    gtk_main_quit();
}

static int executable_directory(const char *argv0, char *directory,
                                size_t directory_size)
{
    char path[PATH_MAX];
    char *slash;

    if (!realpath(argv0, path))
        return -1;
    slash = strrchr(path, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (strlen(path) >= directory_size)
        return -1;
    strcpy(directory, path);
    return 0;
}

int main(int argc, char **argv)
{
    struct console_window console;
    GtkWidget *window;
    GtkWidget *box;
    GtkWidget *menu;
    char directory[PATH_MAX];
    char shell_path[PATH_MAX];
    const char *session;
    const char *specification = NULL;
    const char *font_candidates[5];
    int sockets[2];
    int external_fd = -1;
    struct ace_console_spec window_spec;
    int gtk_argc = 1;
    char **gtk_argv = (char *[]){ argv[0], NULL };
    int i;

    memset(&console, 0, sizeof(console));
    console.stream_fd = -1;
    console.child_pid = -1;
    console.controller_pid = -1;
    console.font_points = DEFAULT_FONT_POINTS;
    g_strlcpy(console.current_title, "ACE Shell",
              sizeof(console.current_title));
    memcpy(console.palette, default_palette, sizeof(console.palette));
    memset(&window_spec, 0, sizeof(window_spec));
    if (argc < 3 || strcmp(argv[1], "--session") != 0) {
        fprintf(stderr, "usage: %s --session SESSION [--fd FD --spec CON:... ]\n",
                argv[0]);
        return 20;
    }
    session = argv[2];
    setenv("ACE_SESSION", session, 1);
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--fd") == 0 && i + 1 < argc) {
            char *end;
            long value;

            errno = 0;
            value = strtol(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' ||
                value < 0 || value > INT_MAX) {
                fprintf(stderr, "ace-console: invalid --fd\n");
                return 20;
            }
            external_fd = (int)value;
        } else if (strcmp(argv[i], "--spec") == 0 && i + 1 < argc) {
            specification = argv[++i];
            if (ace_console_spec_parse(specification, &window_spec) != 0) {
                fprintf(stderr, "ace-console: invalid CON: specification\n");
                return 20;
            }
        } else {
            fprintf(stderr, "ace-console: unknown argument: %s\n", argv[i]);
            return 20;
        }
    }
    if ((external_fd >= 0) != (specification != NULL)) {
        fprintf(stderr, "ace-console: --fd and --spec must be paired\n");
        return 20;
    }
    if (external_fd >= 0)
        console.current_title[0] = '\0';
    if (window_spec.title[0] != '\0')
        g_strlcpy(console.current_title, window_spec.title,
                  sizeof(console.current_title));
    console.font_family = g_strdup(default_font_candidates[0]);
    for (i = 0; default_font_candidates[i]; i++) {
        if (ace_gfx_font_family_complete(default_font_candidates[i], 0)) {
            g_free(console.font_family);
            console.font_family = g_strdup(default_font_candidates[i]);
            break;
        }
    }
    load_config(&console);
    build_font_candidates(console.font_family, default_font_candidates,
                          font_candidates, G_N_ELEMENTS(font_candidates));
    console.device = ace_console_device_open(
        window_spec.has_size ? window_spec.width : CONSOLE_WIDTH,
        window_spec.has_size ? window_spec.height : CONSOLE_HEIGHT,
                                             font_candidates,
                                             font_pixels(console.font_points),
                                             ace_gfx_weight_from_css(
                                                 console.font_weight));
    if (!console.device) {
        fprintf(stderr, "ace-console: failed to set up console.device\n");
        return 20;
    }
    if (ace_console_device_set_palette(console.device, console.palette) != 0) {
        fprintf(stderr, "ace-console: failed to apply saved palette\n");
        ace_console_device_close(console.device);
        return 20;
    }
    ace_console_channel_init(&console.channel, -1);
    update_channel_geometry(&console, FALSE);
    /* ACE runs as a native Wayland console, even when DISPLAY is also set.
     * ACE owns the exported menu/action pair; GtkMenuBar remains the local
     * fallback when no appmenu compositor is available. */
    setenv("GDK_BACKEND", "wayland", 1);
    prepare_appmenu_environment();
    if (external_fd >= 0) {
        struct ucred peer;
        socklen_t peer_size = sizeof(peer);

        console.stream_fd = external_fd;
        console.child_pid = -1;
        if (getsockopt(external_fd, SOL_SOCKET, SO_PEERCRED, &peer,
                       &peer_size) == 0 && peer_size == sizeof(peer))
            console.controller_pid = peer.pid;
    } else {
        if (executable_directory(argv[0], directory, sizeof(directory)) != 0)
            return 20;
        if (snprintf(shell_path, sizeof(shell_path), "%s/ace-user-shell", directory) >=
            (int)sizeof(shell_path))
            return 20;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            return 20;
        console.child_pid = fork();
        if (console.child_pid < 0)
            return 20;
        if (console.child_pid == 0) {
            close(sockets[0]);
            /* Everything the shell runs shares one process group, so the console
             * can hang up on all of it at once when the window closes. */
            (void)setpgid(0, 0);
            if (dup2(sockets[1], STDIN_FILENO) < 0 ||
                dup2(sockets[1], STDOUT_FILENO) < 0 ||
                dup2(sockets[1], STDERR_FILENO) < 0)
                _exit(20);
            if (sockets[1] > STDERR_FILENO)
                close(sockets[1]);
            setenv("ACE_SESSION", session, 1);
            setenv("ACE_CONSOLE_INTERACTIVE", "1", 1);
            /* This is an Amiga console.device stream, not the host terminal
             * inherited by the GUI launcher. Unchanged Amiga programs use the
             * standard TERM value to select their console backend; in particular
             * Vim's __AROS__ size query is enabled only for TERM=amiga. */
            setenv("TERM", "amiga", 1);
            execl(shell_path, shell_path, (char *)NULL);
            _exit(20);
        }
        close(sockets[1]);
        console.stream_fd = sockets[0];
        console.controller_pid = console.child_pid;
    }
    ace_console_channel_set_fd(&console.channel, console.stream_fd);
    ace_console_device_set_input_fd(console.device, console.stream_fd);

    /* Keep the Wayland app_id, X11 class, launcher desktop file, and
     * icon-theme lookup under the same identity so the panel groups the
     * running console with ACE Shell and displays its application icon. */
    g_set_prgname(ACE_ICON_NAME);
    g_set_application_name("ACE Shell");
    gdk_set_program_class(ACE_ICON_NAME);
    gtk_init(&gtk_argc, &gtk_argv);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    console.window = window;
    refresh_console_title(&console);
    gtk_window_set_icon_name(GTK_WINDOW(window), ACE_ICON_NAME);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_default_size(
        GTK_WINDOW(window),
        window_spec.has_size ? window_spec.width : CONSOLE_WIDTH,
        window_spec.has_size ? window_spec.height : CONSOLE_HEIGHT);
    g_signal_connect(window, "destroy", G_CALLBACK(console_destroy), &console);
    console.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(console.drawing_area, TRUE);
    gtk_widget_add_events(console.drawing_area,
                          GDK_KEY_PRESS_MASK | GDK_SCROLL_MASK |
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK);
    g_signal_connect(console.drawing_area, "draw", G_CALLBACK(draw_console), &console);
    g_signal_connect(console.drawing_area, "key-press-event", G_CALLBACK(key_press), &console);
    g_signal_connect(console.drawing_area, "scroll-event", G_CALLBACK(scroll_event),
                     &console);
    g_signal_connect(console.drawing_area, "button-press-event",
                     G_CALLBACK(button_press), &console);
    g_signal_connect(console.drawing_area, "motion-notify-event",
                     G_CALLBACK(motion_notify), &console);
    g_signal_connect(console.drawing_area, "button-release-event",
                     G_CALLBACK(button_release), &console);
    g_signal_connect(console.drawing_area, "size-allocate",
                     G_CALLBACK(drawing_area_size_allocate), &console);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    menu = build_menu(&console);
    console.menu_bar = menu;
    gtk_box_pack_start(GTK_BOX(box), menu, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), console.drawing_area, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    (void)export_dbus_menu(&console);
    gtk_widget_show_all(window);
    if (window_spec.has_position)
        gtk_window_move(GTK_WINDOW(window), window_spec.x, window_spec.y);
    gtk_widget_grab_focus(console.drawing_area);
    (void)ace_requestor_gui_start(session, window);
    console.menu_probe_source = g_timeout_add(250, update_menu_visibility,
                                              &console);

    if (!ace_console_dispatch_add_output_watch(console.stream_fd,
                                               read_console, &console)) {
        fprintf(stderr, "ace-console: cannot watch console output\n");
        gtk_widget_destroy(window);
    }
    gtk_main();
    ace_appmenu_wayland_forget();
    unexport_dbus_menu(&console);
    ace_console_device_close(console.device);
    g_free(console.font_family);
    return 0;
}
