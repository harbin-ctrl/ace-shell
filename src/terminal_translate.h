#ifndef ACE_TERMINAL_TRANSLATE_H
#define ACE_TERMINAL_TRANSLATE_H

#include <stddef.h>

/*
 * Linux and the Amiga console agree on almost nothing, so ACE keeps both
 * vocabularies in one place:
 *
 *   Linux child --- xterm ---> ace_xterm_to_amiga --- CSI ---> ACE console
 *   ACE console --- CSI   ---> ace_amiga_to_xterm --- xterm -> Linux child
 *
 * The screen direction is the harder one. xterm introduces sequences with
 * ESC [, colours with 256-entry SGR, and text in UTF-8; console.device
 * introduces sequences with the C1 byte 0x9B, colours by naming one of
 * eight pens, and text in Latin-1. Its parser (AROS rom/devs/console) also
 * prints anything it does not recognise, so a sequence that cannot be
 * translated must be consumed here rather than forwarded.
 */

/* An in-progress escape sequence. The longest xterm sequence ACE has to
 * hold is a truecolour SGR such as ESC [ 0;1;38;2;255;255;255 m. */
#define ACE_TERMINAL_SEQUENCE_MAX 64

/* console.device has no erase-character, repeat, or counted insert/delete
 * command, so those are replayed as literal cells and one short sequence
 * can become most of a line. This is the room a sink must have free before
 * it is fed another byte. */
#define ACE_TERMINAL_REPEAT_MAX 255
#define ACE_TERMINAL_EMIT_MAX 1024

/*
 * DECCKM. xterm-256color names the SS3 form -- ESC O A -- as its arrow key
 * capability and turns this mode on with the same smkx that a full-screen
 * program sends when it takes the keyboard, so a program in that state does
 * not recognise the CSI form. The mode is announced on the screen side and
 * answered on the keyboard side, which is why both directions live here.
 */
enum ace_cursor_key_mode {
    ACE_CURSOR_KEYS_NORMAL = 0,
    ACE_CURSOR_KEYS_APPLICATION,
};

/*
 * Where translated bytes go. The screen direction expands, so it asks how
 * much room is left rather than discovering a full sink halfway through a
 * sequence it can no longer take back.
 */
struct ace_terminal_sink {
    int (*write)(void *context, const void *bytes, size_t length);
    size_t (*space)(void *context);
    void *context;
};

/* ACE console keyboard -> xterm. */
struct ace_amiga_to_xterm {
    unsigned char sequence[ACE_TERMINAL_SEQUENCE_MAX];
    size_t sequence_length;
    int utf8_continuations;
    int resize_pending;
    enum ace_cursor_key_mode cursor_keys;
};

/* Linux program output -> ACE console. */
struct ace_xterm_to_amiga {
    unsigned char sequence[ACE_TERMINAL_SEQUENCE_MAX];
    size_t sequence_length;
    int kind;
    unsigned char utf8[4];
    size_t utf8_length;
    size_t utf8_needed;
    /* Repeat (CSI b) names no character of its own. */
    unsigned char last_graphic;
    /* The console has one bold flag but xterm has two ways to ask for it:
     * SGR 1, and the eight bright colours the Amiga palette cannot hold. */
    int bold_requested;
    int bright_foreground;
    int bold_emitted;
    enum ace_cursor_key_mode cursor_keys;
};

void ace_amiga_to_xterm_init(struct ace_amiga_to_xterm *state);
int ace_amiga_to_xterm_feed(struct ace_amiga_to_xterm *state,
                            const struct ace_terminal_sink *sink,
                            const unsigned char *bytes, size_t length);
int ace_amiga_to_xterm_flush(struct ace_amiga_to_xterm *state,
                             const struct ace_terminal_sink *sink);
void ace_amiga_to_xterm_set_cursor_keys(struct ace_amiga_to_xterm *state,
                                        enum ace_cursor_key_mode mode);

void ace_xterm_to_amiga_init(struct ace_xterm_to_amiga *state);
/* Returns how many input bytes were translated. A short result means the
 * sink fell below ACE_TERMINAL_EMIT_MAX; feed the rest once it has drained. */
size_t ace_xterm_to_amiga_feed(struct ace_xterm_to_amiga *state,
                               const struct ace_terminal_sink *sink,
                               const unsigned char *bytes, size_t length);
int ace_xterm_to_amiga_flush(struct ace_xterm_to_amiga *state,
                             const struct ace_terminal_sink *sink);
/* What the program on the screen side last asked the keyboard to send. */
enum ace_cursor_key_mode ace_xterm_to_amiga_cursor_keys(
    const struct ace_xterm_to_amiga *state);

#endif
