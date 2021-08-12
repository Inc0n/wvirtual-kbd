/*
 * Copyright © 2019 Josef Gajdusek
 * Copyright © 2020 Daniel De Graaf
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
https://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html#ss1.2

 */

#define _GNU_SOURCE
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

#define error(...) {                     \
        fprintf(stderr, __VA_ARGS__);            \
        fprintf(stderr, "\n");                   \
        exit(EXIT_FAILURE);                      \
}

#define SHIFT 0x100

#define DEBUG 1

#ifdef DEBUG
#define debug_log(...) {                         \
        fprintf(stderr, __VA_ARGS__);            \
        fprintf(stderr, "\n");                   \
}
#else
#define debug_log(...)
#endif

enum Modifier {
    MOD_NONE = 0x0,
    MOD_SHIFT = 0x1,
    MOD_CONTROL = 0x4,
    MOD_ALT = 0x8,
    MOD_SUPER = 0x40
};

struct wkey {
    unsigned int key;
    enum Modifier mod;
};

// global variables

static struct wl_seat *Seat = NULL;
static struct zwp_virtual_keyboard_manager_v1 *Keyboard_manager = NULL;

// maps

static const struct { const char *name; uint32_t mod; } Mod_names[] = {
    {"S", MOD_SHIFT},
    /* {"capslock", CAPSLOCK}, */
    {"C", MOD_CONTROL},
    {"H", MOD_SUPER},
    /* {"win", MOD_SUPER}, */
    {"M", MOD_ALT},
    /* {"altgr", ALTGR}, */
};

enum Modifier name_to_mod(const char *name)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(Mod_names); i++) {
		if (!strcasecmp(Mod_names[i].name, name)) {
			return Mod_names[i].mod;
		}
	}
	return MOD_NONE;
}

// from the keyboard map, values already subtracted 8 for zwp use
static const uint32_t Charmap[256] = {
    [0x1b] = 1, // esc
    ['1'] = 2, ['!'] = SHIFT | 2,
    ['2'] = 3, ['@'] = SHIFT | 3,
    ['3'] = 4, ['#'] = SHIFT | 4,
    ['4'] = 5, ['$'] = SHIFT | 5,
    ['5'] = 6, ['%'] = SHIFT | 6,
    ['6'] = 7, ['^'] = SHIFT | 7,
    ['7'] = 8, ['&'] = SHIFT | 8,
    ['8'] = 9, ['*'] = SHIFT | 9,
    ['9'] = 10, ['('] = SHIFT | 10,
    ['0'] = 11, [')'] = SHIFT | 11,
    ['-'] = 12, ['_'] = SHIFT | 12,
    ['='] = 13, ['+'] = SHIFT | 13,
    [0x08] = 14,
    ['\t'] = 15,
    ['q'] = 16, ['Q'] = SHIFT | 16,
    ['w'] = 17, ['W'] = SHIFT | 17,
    ['e'] = 18, ['E'] = SHIFT | 18,
    ['r'] = 19, ['R'] = SHIFT | 19,
    ['t'] = 20, ['T'] = SHIFT | 20,
    ['y'] = 21, ['Y'] = SHIFT | 21,
    ['u'] = 22, ['U'] = SHIFT | 22,
    ['i'] = 23, ['I'] = SHIFT | 23,
    ['o'] = 24, ['O'] = SHIFT | 24,
    ['p'] = 25, ['P'] = SHIFT | 25,
    ['['] = 26, ['{'] = SHIFT | 26,
    [']'] = 27, ['}'] = SHIFT | 27,
    ['\n'] = 28,
    // LCtrl = 29,
    ['a'] = 30, ['A'] = SHIFT | 30,
    ['s'] = 31, ['S'] = SHIFT | 31,
    ['d'] = 32, ['D'] = SHIFT | 32,
    ['f'] = 33, ['F'] = SHIFT | 33,
    ['g'] = 34, ['G'] = SHIFT | 34,
    ['h'] = 35, ['H'] = SHIFT | 35,
    ['j'] = 36, ['J'] = SHIFT | 36,
    ['k'] = 37, ['K'] = SHIFT | 37,
    ['l'] = 38, ['L'] = SHIFT | 38,
    [';'] = 39, [':'] = SHIFT | 39,
    ['\''] = 40, ['"'] = SHIFT | 40,
    ['`'] = 41, ['~'] = SHIFT | 41,
    // LShift = 42,
    ['\\'] = 43, ['|'] = SHIFT | 43,
    ['z'] = 44, ['Z'] = SHIFT | 44,
    ['x'] = 45, ['X'] = SHIFT | 45,
    ['c'] = 46, ['C'] = SHIFT | 46,
    ['v'] = 47, ['V'] = SHIFT | 47,
    ['b'] = 48, ['B'] = SHIFT | 48,
    ['n'] = 49, ['N'] = SHIFT | 49,
    ['m'] = 50, ['M'] = SHIFT | 50,
    [','] = 51, ['<'] = SHIFT | 51,
    ['.'] = 52, ['>'] = SHIFT | 52,
    ['/'] = 53, ['?'] = SHIFT | 53,
    // RShift = 54,
    // kpmu = 55,
    // LAlt = 56,
    [' '] = 57,
    // caps = 58,
    // F1 = 59
    // F10 = 68
    // F11 = 87
    // F12 = 88
};

// from the keyboard map, values already subtracted 8 for zwp use
static const struct { const char *name; uint32_t code; } Keymap[] = {
    // [0x1b] = 1, // esc
    {"esc", 1},
    // LCtrl = 29,
    // LShift = 42,
    // RShift = 54,
    // kpmu = 55,
    // LAlt = 56,
    // caps = 58,
    {"F1", 59},
    {"F10", 68},
    {"F11", 87},
    {"F12", 88},
};

uint32_t keyname_to_keycode(const char *name) {
	for (unsigned int i = 0; i < ARRAY_SIZE(Keymap); i++) {
		if (!strcasecmp(Keymap[i].name, name)) {
			return Keymap[i].code;
		}
	}
	return 0;
}

//

static int timestamp(void) {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    int ms = 1000 * tp.tv_sec + tp.tv_nsec / 1000000;
    return ms;
}

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version) {
    if (!strcmp(interface, wl_seat_interface.name)) {
        Seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
    }
    else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        Keyboard_manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name) {
    // Who cares?
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void setup_keymap(struct zwp_virtual_keyboard_v1 *keyboard) {
    int fd = memfd_create("keymap", FD_CLOEXEC);
    struct xkb_rule_names rule_names = {
        .model = "pc104",
        .layout = "",
    };
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, &rule_names, 0);
    struct xkb_state *state = xkb_state_new(keymap);
    char *keymap_string =
        xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(keymap_string);
    write(fd, keymap_string, size);
    free(keymap_string);

    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                   size);
    close(fd);
}

static void do_type(struct zwp_virtual_keyboard_v1 *keyboard,
                    struct wkey key)
{
    zwp_virtual_keyboard_v1_modifiers(keyboard, key.mod, 0, 0, 0);

    if (key.key) {
        zwp_virtual_keyboard_v1_key(keyboard, timestamp(), key.key,
                                    WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard, timestamp(), key.key,
                                    WL_KEYBOARD_KEY_STATE_RELEASED);
    }

    if (key.mod != MOD_NONE) {
        zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
    }
}

// DONE <2021-08-11 WED>: Parse string kbd representation to our internal
// uint32_t representation
/*
  kbd examples:
  C-M-s, control-meta-s
  C-S-s, control-SHIFT-s
*/
static struct wkey parse_kbd(char *kbd) {
    // a copy to retain for error messages
    const char *copy = kbd;
    const char delim[] = "-";

    char *ptr = strtok(kbd, delim);
    struct wkey key = { .key = 0, .mod = MOD_NONE };

    while (ptr != NULL) {

        char *next = strtok(NULL, delim);
        if (next == NULL) {
            /* error("kbd parsing error: %s", copy); */
            /* const char c = ptr[0]; */
            key.key = (ptr[1] == '\0')
                ? Charmap[(uint8_t)ptr[0]]
                : keyname_to_keycode(ptr);
        } else {
            enum Modifier mod_code = name_to_mod(ptr);
            if (mod_code == MOD_NONE)
                error("kbd invalid mode code: %s, %s\n", copy, ptr);

            key.mod |= (uint32_t)mod_code;
        }
        ptr = next;
    }
    return key;
}

static void print_help() {
    error("Usage: ./virtual-keyboard <subcommands> ...\n\n"
          "<subcommand>: type | pipe | send\n"
          "\ttype <text to send>\n"
          "\tpipe\n"
          "\tsend <keycode to send>\n");
}

static void parse_args(struct zwp_virtual_keyboard_v1 *keyboard, int argc, char *argv[]) {
    const int consumed = 2;
    const char *cmd = *argv;

    if (argc == 0) return;

    struct wkey key = { .key = 0, .mod = MOD_NONE };

    if (strcmp(cmd, "pipe") == 0) {
        uint8_t buf[100];
        while (1) {
            int rv = read(0, buf, 100);
            if (rv <= 0)
                break;
            for (int i = 0; i < rv; i++) {
                key.key = buf[i];
                do_type(keyboard, key);
            }
        }
        error("Unexpected error of pipe");
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
        exit(EXIT_SUCCESS);
    }

    if (argc < 2) {
        error("Syntax error, check help\n");
    }

    if (strcmp(cmd, "type") == 0) {
        const char *text = argv[1];
        while (*text) {
            // transform into internal representation first
            uint8_t c = (uint8_t)*text;
            uint32_t map = Charmap[c];
            key.key = map & 0xFF;
            key.mod = map & ~(map & 0xFF);
            debug_log("typing %c, %d", c, map);
            do_type(keyboard, key);
            text++;
        }
    } else if (strcmp(cmd, "send") == 0) {
        char *kbd = argv[1];
        key = parse_kbd(kbd);
        debug_log("sending %d, %d", key.key, key.mod);
        do_type(keyboard, key);
    } else if (strcmp(cmd, "sleep") == 0) {
        uint32_t ms = (uint32_t)atoi(argv[1]);
        usleep(ms * 1000);
    } else {
        // TODO support more advanced xdotool-style commands:
        //  - distinct key names (F*, insert/del, ...)
        //  - sending modifiers with keys (other than shift)
        //  - sending key-down and key-up independently
        // Other features:
        //  - Non-QWERTY keyboard layouts
        error("Invalid subcommand, %s\n", cmd);
    }
    parse_args(keyboard, argc - consumed, argv + consumed);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help();
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
        error("failed to create display: %m\n");
    }
    if (display != NULL) {
        struct wl_registry *registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener, NULL);
        wl_display_dispatch(display);
        wl_display_roundtrip(display);
    }

    if (Keyboard_manager == NULL) {
        error("compositor does not support wp-virtual-keyboard-unstable-v1\n");
    }

    struct zwp_virtual_keyboard_v1 *keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(Keyboard_manager,
                                                                Seat);
    setup_keymap(keyboard);
    parse_args(keyboard, argc - 1, argv + 1);

    // cleanup
    zwp_virtual_keyboard_v1_destroy(keyboard);
    wl_display_roundtrip(display);

    return EXIT_SUCCESS;
}
