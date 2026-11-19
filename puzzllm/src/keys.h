/* keys.h - turn a decoded key press into the bytes a legacy terminal sends.
 *
 * The game reads the outer terminal through the Kitty keyboard protocol, so
 * it knows each key and the text it produced. The programs on the screens
 * (omp, bash) get the classic xterm encoding: UTF-8 text, C0 for Ctrl,
 * ESC-prefix for Alt, CSI/SS3 for cursor and function keys.
 */
#ifndef PUZZLLM_KEYS_H
#define PUZZLLM_KEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kitty functional key values (the PUA numbers kitty_keyboard.h uses;
 * keys.c checks them against that header in the game build). */
enum {
    KEY_ESCAPE = 0xe000, KEY_ENTER = 0xe001, KEY_TAB = 0xe002,
    KEY_BACKSPACE = 0xe003, KEY_INSERT = 0xe004, KEY_DELETE = 0xe005,
    KEY_LEFT = 0xe006, KEY_RIGHT = 0xe007, KEY_UP = 0xe008, KEY_DOWN = 0xe009,
    KEY_PAGE_UP = 0xe00a, KEY_PAGE_DOWN = 0xe00b, KEY_HOME = 0xe00c,
    KEY_END = 0xe00d, KEY_F1 = 0xe014, KEY_F12 = 0xe01f
};
enum { MOD_SHIFT = 1u << 0, MOD_ALT = 1u << 1, MOD_CTRL = 1u << 2 };
enum { KEY_TEXT_MAX = 16 };

typedef struct key_event {
    uint32_t key;                 /* unicode scalar or KEY_* */
    uint32_t mods;                /* MOD_* */
    uint32_t text[KEY_TEXT_MAX];  /* text the key produced, if any */
    int text_len;
    int action;                   /* 1 press, 2 repeat, 3 release */
} key_event;

/* Bytes for one key event; 0 for releases and keys with no encoding. */
size_t keys_encode(const key_event *ev, bool app_cursor, char *out, size_t cap);

/* Bracketed or plain paste of UTF-8 text. */
size_t keys_paste(const char *text, bool bracketed, char *out, size_t cap);

struct neuroterm;
size_t neuroterm_key(struct neuroterm *terminal, const key_event *event,
                     char *buf, size_t cap);

#endif
