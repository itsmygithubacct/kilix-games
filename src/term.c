/* Raw terminal input and asynchronous Kitty graphics presentation. */
#include "kilix_pong.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

static volatile sig_atomic_t winch_pending;
static struct termios original_termios;
static volatile sig_atomic_t raw_active;
static volatile int shutdown_claimed;
static bool release_capability_seen;

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;
static bool clear_pending;
static char origin_sequence[32] = "\x1b[H";

static void on_winch(int signal_number)
{
    (void)signal_number;
    winch_pending = 1;
}

static void write_all(const char *data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(STDOUT_FILENO, data, length);
        if (written <= 0) return;
        data += written;
        length -= (size_t)written;
    }
}

static void write_string(const char *text)
{
    write_all(text, strlen(text));
}

static bool read_byte_timeout(unsigned char *out, int milliseconds)
{
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000
    };
    int selected = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
    return selected > 0 && read(STDIN_FILENO, out, 1) == 1;
}

static bool measure_geometry(int *out_width, int *out_height)
{
    struct winsize size;
    if (!out_width || !out_height || ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
        return false;

    int columns = size.ws_col > 0 ? size.ws_col : 80;
    int rows = size.ws_row > 0 ? size.ws_row : 24;
    int cell_width = size.ws_xpixel > 0 ? size.ws_xpixel / columns : 9;
    int cell_height = size.ws_ypixel > 0 ? size.ws_ypixel / rows : 18;
    if (cell_width <= 0) cell_width = 9;
    if (cell_height <= 0) cell_height = 18;

    int image_rows = rows > 2 ? rows - 1 : rows;
    int width = columns * cell_width;
    int height = image_rows * cell_height;
    if (width < 320 || height < 180) return false;
    if (width > 1440) width = 1440;
    if (height > 900) height = 900;
    width -= width % cell_width;
    height -= height % cell_height;
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) return false;

    *out_width = width;
    *out_height = height;

    int occupied_columns = (width + cell_width - 1) / cell_width;
    int occupied_rows = (height + cell_height - 1) / cell_height;
    int column = 1 + (columns - occupied_columns) / 2;
    int row = 1 + (image_rows - occupied_rows) / 2;
    if (column < 1) column = 1;
    if (row < 1) row = 1;
    (void)snprintf(origin_sequence, sizeof origin_sequence,
                   "\x1b[%d;%dH", row, column);
    return true;
}

static bool kitty_graphics_probe(void)
{
    /* Pair the graphics query with a device-attributes query so terminals that
       reject the APC still give us a bounded reply to consume. */
    write_string("\x1b_Gi=91,a=q,t=d,f=24,s=1,v=1;AAAA\x1b\\\x1b[c");
    char response[512] = {0};
    size_t used = 0;
    bool graphics_seen = false;
    while (used + 1 < sizeof response) {
        unsigned char byte;
        if (!read_byte_timeout(&byte, 400)) break;
        response[used++] = (char)byte;
        response[used] = '\0';
        if (strstr(response, "\x1b_Gi=91")) graphics_seen = true;
        if (byte == 'c' && strstr(response, "\x1b[?")) break;
    }
    return graphics_seen;
}

bool term_init(int *out_width, int *out_height)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    if (!measure_geometry(out_width, out_height)) return false;
    if (tcgetattr(STDIN_FILENO, &original_termios) != 0) return false;

    struct termios raw = original_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

    raw_active = true;
    shutdown_claimed = 0;
    release_capability_seen = false;
    winch_pending = 0;

    if (!getenv("KILIX_PONG_SKIP_PROBE") && !kitty_graphics_probe()) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        raw_active = false;
        fprintf(stderr,
                "kilix-pong: terminal did not answer the Kitty graphics query\n"
                "try Kilix, Kitty, Ghostty, WezTerm, or recent Konsole\n"
                "(KILIX_PONG_SKIP_PROBE=1 bypasses this check)\n");
        return false;
    }

    (void)signal(SIGWINCH, on_winch);
    /* Flags 1|2|4|8: disambiguate, event types, alternate keys, all keys. */
    write_string("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[>15u");
    return true;
}

bool term_check_resize(int *out_width, int *out_height)
{
    if (!winch_pending) return false;
    winch_pending = 0;
    pthread_mutex_lock(&frame_lock);
    bool measured = measure_geometry(out_width, out_height);
    if (measured) clear_pending = true;
    pthread_mutex_unlock(&frame_lock);
    return measured;
}

/* ------------------------------- input ---------------------------------- */

static bool decimal_range(const char *text, size_t begin, size_t end, int *out)
{
    unsigned value = 0;
    if (!out || begin >= end) return false;
    for (size_t index = begin; index < end; index++) {
        unsigned char byte = (unsigned char)text[index];
        if (byte < '0' || byte > '9') return false;
        unsigned digit = (unsigned)(byte - '0');
        if (value > ((unsigned)INT_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out = (int)value;
    return true;
}

static bool parse_modifiers_action(const char *text, size_t begin, size_t end,
                                   int *mods, KeyAction *action,
                                   bool *explicit_action)
{
    if (!mods || !action || !explicit_action) return false;
    *mods = 0;
    *action = KEY_ACTION_PRESS;
    *explicit_action = false;
    if (begin >= end) return true;

    size_t colon = end;
    for (size_t index = begin; index < end; index++) {
        if (text[index] == ':') {
            colon = index;
            break;
        }
    }

    int encoded_modifiers;
    if (!decimal_range(text, begin, colon, &encoded_modifiers) ||
        encoded_modifiers < 1 || encoded_modifiers > 256)
        return false;
    *mods = encoded_modifiers - 1;

    if (colon < end) {
        int encoded_action;
        if (!decimal_range(text, colon + 1, end, &encoded_action) ||
            encoded_action < KEY_ACTION_PRESS ||
            encoded_action > KEY_ACTION_RELEASE)
            return false;
        *action = (KeyAction)encoded_action;
        *explicit_action = true;
    }
    return true;
}

static int normalize_key(int key)
{
    switch (key) {
    case 13: return KEY_ENTER;
    case 9: return KEY_TAB;
    case 27: return KEY_ESC;
    case 127: return KEY_BACKSPACE;
    /* Kitty's functional-key code points when a terminal elects CSI-u for
       arrows instead of the traditional A/B/C/D final bytes. */
    case 57352: return KEY_UP;
    case 57353: return KEY_DOWN;
    case 57354: return KEY_RIGHT;
    case 57355: return KEY_LEFT;
    default: return key;
    }
}

static int legacy_final_key(unsigned char final)
{
    switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'Z': return KEY_TAB;
    default: return 0;
    }
}

static bool parse_csi_u(const char *parameters, size_t length, KeyEvent *event)
{
    size_t first_semicolon = length;
    size_t second_semicolon = length;
    for (size_t index = 0; index < length; index++) {
        if (parameters[index] == ';') {
            if (first_semicolon == length) first_semicolon = index;
            else { second_semicolon = index; break; }
        }
    }

    size_t first_colon = first_semicolon;
    for (size_t index = 0; index < first_semicolon; index++) {
        if (parameters[index] == ':') { first_colon = index; break; }
    }

    int key;
    if (!decimal_range(parameters, 0, first_colon, &key) || key > 0x10ffff)
        return false;

    int modifiers = 0;
    KeyAction action = KEY_ACTION_PRESS;
    bool explicit_action = false;
    if (first_semicolon < length &&
        !parse_modifiers_action(parameters, first_semicolon + 1,
                                second_semicolon, &modifiers, &action,
                                &explicit_action))
        return false;

    event->key = normalize_key(key);
    event->mods = modifiers;
    event->action = action;
    if (explicit_action) release_capability_seen = true;
    return true;
}

static bool parse_legacy_csi(const char *parameters, size_t length,
                             unsigned char final, KeyEvent *event)
{
    int key = legacy_final_key(final);
    if (!key) return false;

    size_t semicolon = length;
    for (size_t index = 0; index < length; index++) {
        if (parameters[index] == ';') { semicolon = index; break; }
    }

    int modifiers = final == 'Z' ? KEY_MOD_SHIFT : 0;
    KeyAction action = KEY_ACTION_PRESS;
    bool explicit_action = false;
    if (semicolon < length &&
        !parse_modifiers_action(parameters, semicolon + 1, length,
                                &modifiers, &action, &explicit_action))
        return false;

    event->key = key;
    event->mods = modifiers;
    event->action = action;
    if (explicit_action) release_capability_seen = true;
    return true;
}

static bool parse_csi(KeyEvent *event)
{
    char parameters[96];
    size_t length = 0;
    bool overflow = false;
    unsigned char final = 0;

    for (;;) {
        unsigned char byte;
        if (!read_byte_timeout(&byte, 25)) return false;
        if (byte >= 0x40 && byte <= 0x7e) {
            final = byte;
            break;
        }
        if (length < sizeof parameters) parameters[length++] = (char)byte;
        else overflow = true;
    }
    if (overflow) return false;
    if (final == 'u') return parse_csi_u(parameters, length, event);
    return parse_legacy_csi(parameters, length, final, event);
}

bool term_poll_event(KeyEvent *out)
{
    if (!out) return false;
    unsigned char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1) return false;

    *out = (KeyEvent){ .action = KEY_ACTION_PRESS };
    if (byte == 3) {
        out->key = 'c';
        out->mods = KEY_MOD_CTRL;
        return true;
    }
    if (byte == '\r' || byte == '\n') { out->key = KEY_ENTER; return true; }
    if (byte == 127 || byte == 8) { out->key = KEY_BACKSPACE; return true; }
    if (byte == '\t') { out->key = KEY_TAB; return true; }
    if (byte != 0x1b) { out->key = byte; return true; }

    unsigned char next;
    if (!read_byte_timeout(&next, 25)) { out->key = KEY_ESC; return true; }
    if (next == '[') return parse_csi(out);
    if (next == 'O') {
        unsigned char final;
        if (!read_byte_timeout(&final, 25)) return false;
        int key = legacy_final_key(final);
        if (!key) return false;
        out->key = key;
        return true;
    }

    /* A legacy Alt chord is ESC followed by the literal key. */
    out->key = normalize_key(next);
    out->mods = KEY_MOD_ALT;
    return true;
}

bool term_has_key_release(void)
{
    return release_capability_seen;
}

/* ---------------------------- frame presenter --------------------------- */

typedef struct {
    uint8_t *rgb;
    uint8_t *compressed;
    char *base64;
    char *output;
    size_t rgb_capacity;
    size_t compressed_capacity;
    size_t base64_capacity;
    size_t output_capacity;
} EncoderBuffers;

static EncoderBuffers encoder;
static pthread_t presenter_thread;
static uint8_t *pending_buffer;
static uint8_t *encode_buffer;
static size_t pending_capacity;
static size_t encode_capacity;
static int frame_width;
static int frame_height;
static bool frame_pending;
static bool presenter_running;

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *input, size_t length, char *output)
{
    size_t input_index = 0;
    size_t output_index = 0;
    while (input_index + 2 < length) {
        uint32_t value = (uint32_t)input[input_index] << 16 |
                         (uint32_t)input[input_index + 1] << 8 |
                         input[input_index + 2];
        output[output_index++] = base64_alphabet[(value >> 18) & 63];
        output[output_index++] = base64_alphabet[(value >> 12) & 63];
        output[output_index++] = base64_alphabet[(value >> 6) & 63];
        output[output_index++] = base64_alphabet[value & 63];
        input_index += 3;
    }
    if (input_index < length) {
        uint32_t value = (uint32_t)input[input_index] << 16;
        if (input_index + 1 < length)
            value |= (uint32_t)input[input_index + 1] << 8;
        output[output_index++] = base64_alphabet[(value >> 18) & 63];
        output[output_index++] = base64_alphabet[(value >> 12) & 63];
        output[output_index++] = input_index + 1 < length
                               ? base64_alphabet[(value >> 6) & 63] : '=';
        output[output_index++] = '=';
    }
    return output_index;
}

static bool grow_bytes(uint8_t **buffer, size_t *capacity, size_t required)
{
    if (required <= *capacity) return true;
    uint8_t *grown = realloc(*buffer, required);
    if (!grown) return false;
    *buffer = grown;
    *capacity = required;
    return true;
}

static bool grow_chars(char **buffer, size_t *capacity, size_t required)
{
    if (required <= *capacity) return true;
    char *grown = realloc(*buffer, required);
    if (!grown) return false;
    *buffer = grown;
    *capacity = required;
    return true;
}

static void encode_and_write(const uint8_t *rgba, int width, int height,
                             const char *origin, bool clear_first)
{
    if (!rgba || width <= 0 || height <= 0) return;
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 4) return;
    size_t raw_length = pixels * 3;
    if (!grow_bytes(&encoder.rgb, &encoder.rgb_capacity, raw_length)) return;

    for (size_t index = 0; index < pixels; index++) {
        encoder.rgb[index * 3] = rgba[index * 4];
        encoder.rgb[index * 3 + 1] = rgba[index * 4 + 1];
        encoder.rgb[index * 3 + 2] = rgba[index * 4 + 2];
    }

    size_t compressed_bound = compressBound(raw_length);
    if (!grow_bytes(&encoder.compressed, &encoder.compressed_capacity,
                    compressed_bound))
        return;
    size_t base64_required = ((compressed_bound + 2) / 3) * 4 + 8;
    if (!grow_chars(&encoder.base64, &encoder.base64_capacity, base64_required))
        return;

    uLongf compressed_length = (uLongf)encoder.compressed_capacity;
    if (compress2(encoder.compressed, &compressed_length, encoder.rgb,
                  raw_length, 1) != Z_OK)
        return;
    size_t base64_length = base64_encode(encoder.compressed,
                                         (size_t)compressed_length,
                                         encoder.base64);
    size_t output_required = base64_length +
                             (base64_length / 4096 + 2) * 80 + 256;
    if (!grow_chars(&encoder.output, &encoder.output_capacity, output_required))
        return;

    static int shown_id = 2;
    int new_id = shown_id == 1 ? 2 : 1;
    char *cursor = encoder.output;
    cursor += sprintf(cursor, "\x1b[?2026h%s%s",
                      clear_first ? "\x1b[2J" : "", origin);
    size_t offset = 0;
    bool first = true;
    while (offset < base64_length) {
        size_t chunk = base64_length - offset;
        if (chunk > 4096) chunk = 4096;
        int more = offset + chunk < base64_length;
        if (first) {
            cursor += sprintf(cursor,
                              "\x1b_Ga=T,f=24,i=%d,q=2,o=z,s=%d,v=%d,m=%d;",
                              new_id, width, height, more);
            first = false;
        } else {
            cursor += sprintf(cursor, "\x1b_Gm=%d;", more);
        }
        memcpy(cursor, encoder.base64 + offset, chunk);
        cursor += chunk;
        *cursor++ = '\x1b';
        *cursor++ = '\\';
        offset += chunk;
    }
    cursor += sprintf(cursor,
                      "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\\x1b[?2026l", shown_id);
    shown_id = new_id;
    write_all(encoder.output, (size_t)(cursor - encoder.output));
}

static void *presenter_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&frame_lock);
        while (!frame_pending && presenter_running)
            pthread_cond_wait(&frame_cond, &frame_lock);
        if (!presenter_running) {
            pthread_mutex_unlock(&frame_lock);
            break;
        }

        uint8_t *swap_buffer = pending_buffer;
        pending_buffer = encode_buffer;
        encode_buffer = swap_buffer;
        size_t swap_capacity = pending_capacity;
        pending_capacity = encode_capacity;
        encode_capacity = swap_capacity;
        int width = frame_width;
        int height = frame_height;
        char origin[sizeof origin_sequence];
        (void)snprintf(origin, sizeof origin, "%s", origin_sequence);
        bool clear = clear_pending;
        clear_pending = false;
        frame_pending = false;
        pthread_mutex_unlock(&frame_lock);

        encode_and_write(encode_buffer, width, height, origin, clear);
    }
    return NULL;
}

void term_present(const uint8_t *rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0) return;
    size_t needed = (size_t)width * (size_t)height * 4;
    pthread_mutex_lock(&frame_lock);
    if (!presenter_running) {
        presenter_running = true;
        if (pthread_create(&presenter_thread, NULL, presenter_main, NULL) != 0) {
            presenter_running = false;
            char origin[sizeof origin_sequence];
            (void)snprintf(origin, sizeof origin, "%s", origin_sequence);
            pthread_mutex_unlock(&frame_lock);
            encode_and_write(rgba, width, height, origin, false);
            return;
        }
    }
    if (!grow_bytes(&pending_buffer, &pending_capacity, needed)) {
        pthread_mutex_unlock(&frame_lock);
        return;
    }
    memcpy(pending_buffer, rgba, needed);
    frame_width = width;
    frame_height = height;
    frame_pending = true;
    pthread_cond_signal(&frame_cond);
    pthread_mutex_unlock(&frame_lock);
}

static void presenter_stop(void)
{
    pthread_mutex_lock(&frame_lock);
    if (!presenter_running) {
        pthread_mutex_unlock(&frame_lock);
        return;
    }
    presenter_running = false;
    frame_pending = false;
    pthread_cond_signal(&frame_cond);
    pthread_mutex_unlock(&frame_lock);
    (void)pthread_join(presenter_thread, NULL);
}

static bool claim_shutdown(void)
{
    if (!raw_active) return false;
    return !__sync_lock_test_and_set(&shutdown_claimed, 1);
}

static void restore_terminal(void)
{
    /* The leading ST closes a presenter APC if a fatal signal interrupted it. */
    write_string("\x1b\\\x1b_Ga=d,d=A,q=2\x1b\\\x1b[<u"
                 "\x1b[?25h\x1b[?1049l");
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    raw_active = false;
}

static void free_presenter_buffers(void)
{
    free(pending_buffer);
    free(encode_buffer);
    free(encoder.rgb);
    free(encoder.compressed);
    free(encoder.base64);
    free(encoder.output);
    pending_buffer = NULL;
    encode_buffer = NULL;
    pending_capacity = 0;
    encode_capacity = 0;
    encoder = (EncoderBuffers){0};
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    presenter_stop();
    restore_terminal();
    free_presenter_buffers();
}

void term_emergency_restore(void)
{
    /* Signal path deliberately takes no locks and never joins a thread. */
    if (claim_shutdown()) restore_terminal();
}
