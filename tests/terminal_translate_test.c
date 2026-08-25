#define _POSIX_C_SOURCE 200809L

#include "terminal_translate.h"

#include <stdio.h>
#include <string.h>

#define SINK_CAPACITY 8192

struct test_sink {
    char bytes[SINK_CAPACITY];
    size_t length;
    size_t limit;
};

static int failures;

static int test_sink_write(void *context, const void *bytes, size_t length)
{
    struct test_sink *sink = context;

    if (sink->length + length > sink->limit)
        return -1;
    memcpy(sink->bytes + sink->length, bytes, length);
    sink->length += length;
    return 0;
}

static size_t test_sink_space(void *context)
{
    struct test_sink *sink = context;

    return sink->limit - sink->length;
}

static void sink_init(struct test_sink *sink, size_t limit)
{
    memset(sink, 0, sizeof(*sink));
    sink->limit = limit > SINK_CAPACITY ? SINK_CAPACITY : limit;
}

static void report(const char *name, const struct test_sink *sink,
                   const char *expected)
{
    size_t index;

    fprintf(stderr, "%s\n  expected:", name);
    for (index = 0; expected[index]; index++)
        fprintf(stderr, " %02x", (unsigned char)expected[index]);
    fprintf(stderr, "\n  actual:  ");
    for (index = 0; index < sink->length; index++)
        fprintf(stderr, " %02x", (unsigned char)sink->bytes[index]);
    fprintf(stderr, "\n");
    failures++;
}

/* Feed one xterm stream a byte at a time, so a translation that depends on
 * a sequence arriving whole is caught here rather than in a live session. */
static void check_screen(const char *name, const char *input,
                         const char *expected)
{
    struct ace_xterm_to_amiga state;
    struct test_sink sink;
    struct ace_terminal_sink target;
    size_t index;

    sink_init(&sink, SINK_CAPACITY);
    target.write = test_sink_write;
    target.space = test_sink_space;
    target.context = &sink;
    ace_xterm_to_amiga_init(&state);
    for (index = 0; input[index]; index++) {
        unsigned char byte = (unsigned char)input[index];

        if (ace_xterm_to_amiga_feed(&state, &target, &byte, 1) != 1) {
            report(name, &sink, expected);
            return;
        }
    }
    (void)ace_xterm_to_amiga_flush(&state, &target);
    if (sink.length != strlen(expected) ||
        memcmp(sink.bytes, expected, sink.length) != 0)
        report(name, &sink, expected);
}

static void check_keyboard(const char *name, const char *input,
                           const char *expected)
{
    struct ace_amiga_to_xterm state;
    struct test_sink sink;
    struct ace_terminal_sink target;

    sink_init(&sink, SINK_CAPACITY);
    target.write = test_sink_write;
    target.space = test_sink_space;
    target.context = &sink;
    ace_amiga_to_xterm_init(&state);
    (void)ace_amiga_to_xterm_feed(&state, &target,
                                  (const unsigned char *)input, strlen(input));
    (void)ace_amiga_to_xterm_flush(&state, &target);
    if (sink.length != strlen(expected) ||
        memcmp(sink.bytes, expected, sink.length) != 0)
        report(name, &sink, expected);
}

/* An erase or repeat that cannot fit is left for the next call rather than
 * costing the session its output. */
static void check_backpressure(void)
{
    struct ace_xterm_to_amiga state;
    struct test_sink sink;
    struct ace_terminal_sink target;
    static const unsigned char input[] = "\033[40Xtail";
    size_t consumed;

    sink_init(&sink, ACE_TERMINAL_EMIT_MAX - 1);
    target.write = test_sink_write;
    target.space = test_sink_space;
    target.context = &sink;
    ace_xterm_to_amiga_init(&state);
    consumed = ace_xterm_to_amiga_feed(&state, &target, input,
                                       sizeof(input) - 1);
    if (consumed != 0 || sink.length != 0) {
        fprintf(stderr, "back pressure: consumed %zu into %zu bytes\n",
                consumed, sink.length);
        failures++;
        return;
    }
    sink.limit = SINK_CAPACITY;
    consumed = ace_xterm_to_amiga_feed(&state, &target, input,
                                       sizeof(input) - 1);
    if (consumed != sizeof(input) - 1 || sink.length != 40 + 4 + 4) {
        fprintf(stderr, "back pressure resume: consumed %zu into %zu bytes\n",
                consumed, sink.length);
        failures++;
    }
}

int main(void)
{
    /* Colour. SGR 30+n names a pen, not a colour: pen 0 is the window
       background and pen 1 the text colour, so ANSI red is pen 2. */
    check_screen("ansi colours", "\033[31mred\033[32mgreen\033[0m",
                 "\23332mred\23333mgreen\2330m");
    check_screen("ansi background", "\033[44;37mx", "\23344;31mx");
    check_screen("default colours", "\033[39;49mx", "\23339;49mx");
    check_screen("empty sgr is a reset", "\033[mx", "\2330mx");
    /* Bright foregrounds have no pen, so they arrive as bold; bright black
       would select the window background and becomes the text pen. */
    check_screen("bright foreground", "\033[91mx\033[31my",
                 "\23332;1mx\23332;22my");
    check_screen("bright black", "\033[90mx", "\23331;1mx");
    check_screen("bold survives a colour change", "\033[1;34mx",
                 "\23334;1mx");
    /* 256-colour and truecolour reduce to the nearest of the eight pens. */
    check_screen("indexed colour", "\033[38;5;196mx", "\23332mx");
    check_screen("indexed background", "\033[48;5;23mx", "\23347mx");
    check_screen("grey ramp", "\033[38;5;250mx", "\23331mx");
    check_screen("truecolour", "\033[38;2;0;160;0mx", "\23333mx");
    check_screen("t.416 truecolour", "\033[38:2::0:0:170mx", "\23334mx");
    check_screen("unsupported renditions drop", "\033[5;9;53mx", "x");

    /* Cursor placement. The console fills an omitted CUP parameter with the
       current position where xterm uses the home corner. */
    check_screen("home", "\033[H", "\2331;1H");
    check_screen("row only", "\033[5H", "\2335;1H");
    check_screen("full cup", "\033[12;40H", "\23312;40H");
    check_screen("column address", "\033[9G", "\r\2338C");
    check_screen("first column", "\033[1G", "\r");
    check_screen("row address", "\033[14d", "\23314H");
    check_screen("zero counts move one", "\033[0C", "\2331C");

    /* Erase. The console ignores the parameter of its own erase commands,
       so only the forms it really implements may reach it. */
    check_screen("clear screen", "\033[2J", "\2331;1H\233J");
    check_screen("erase below", "\033[J", "\233J");
    check_screen("erase above drops", "\033[1J", "");
    check_screen("erase to end of line", "\033[K", "\233K");
    check_screen("erase whole line", "\033[2K", "\r\233K");
    check_screen("erase characters", "\033[4X", "    \2334D");
    /* Insert and delete character ignore their count on the Amiga. */
    check_screen("delete characters", "\033[3P", "\233P\233P\233P");
    check_screen("insert characters", "\033[2@", "\233@\233@");
    check_screen("repeat", "-\033[3b", "----");

    /* Modes. Only the three the console can honour are translated. */
    check_screen("cursor off", "\033[?25l", "\2330 p");
    check_screen("cursor on", "\033[?25h", "\2331 p");
    check_screen("autowrap off", "\033[?7l", "\233>7l");
    check_screen("alternate screen", "\033[?1049hx", "\2331;1H\233Jx");
    check_screen("mouse tracking drops", "\033[?1006;1000h", "");
    check_screen("scrolling region drops", "\033[1;28r", "");
    check_screen("window manipulation drops", "\033[22;0;0t", "");
    check_screen("save cursor drops", "\033[s\033[u", "");
    check_screen("cursor style drops", "\033[2 q", "");
    check_screen("charset drops", "\033(Bprompt", "prompt");
    check_screen("keypad mode drops", "\033=x", "x");
    check_screen("reverse index", "\033M", "\215");
    check_screen("window title drops", "\033]0;host\007text", "text");

    /* Text. ACE draws each console byte as Latin-1. */
    check_screen("latin-1", "caf\303\251", "caf\351");
    check_screen("sort marker", "CPU%\342\226\275MEM%", "CPU%vMEM%");
    check_screen("line drawing", "\342\224\202\342\224\200\342\224\274",
                 "|-+");
    check_screen("ellipsis", "wait\342\200\246", "wait...");
    check_screen("unrenderable", "\346\274\242", "?");
    /* U+009B would arrive as a console CSI introducer. */
    check_screen("c1 is not text", "\302\233x", "?x");
    /* A program already speaking to the console keeps its own sequences. */
    check_screen("native csi passes", "target\23399~output",
                 "target\23399~output");
    check_screen("partial sequence flushes", "incomplete\033[",
                 "incomplete\033[");

    check_keyboard("cursor keys", "\233A\233 A", "\033[A\033[1;2D");
    check_keyboard("backspace and delete", "\b\177", "\177\033[3~");

    check_backpressure();

    if (failures) {
        fprintf(stderr, "%d terminal translation checks failed\n", failures);
        return 1;
    }
    printf("terminal translation passes\n");
    return 0;
}
