/*
 * input.c -- the PS/2 keyboard and the PS/2 mouse.
 *
 * Both devices sit behind the same 8042 controller on ports 0x60 and 0x64.
 * Every byte that arrives says which device it came from (bit 5 of the status
 * register), so one polling loop feeds both.
 *
 * The mouse is the main input device of DimOS. On a touch screen a tap is a
 * click, dragging moves the pointer, and holding a button down for a moment
 * produces a right click, because a touch screen has no second button.
 */

#include "dimos.h"

#define QUEUE_SIZE 16u

/* Controller status bits. */
#define STATUS_OUTPUT_FULL 0x01u /* a byte is waiting to be read          */
#define STATUS_INPUT_FULL 0x02u  /* the controller is still busy          */
#define STATUS_FROM_MOUSE 0x20u  /* the waiting byte came from the mouse  */

/* Controller commands (written to port 0x64). */
#define COMMAND_ENABLE_MOUSE 0xA8u
#define COMMAND_WRITE_MOUSE 0xD4u /* the next byte goes to the mouse */

/* Mouse commands (written to port 0x60 after COMMAND_WRITE_MOUSE). */
#define MOUSE_SET_DEFAULTS 0xF6u
#define MOUSE_SET_SCALING_1_1 0xE6u
#define MOUSE_START_STREAMING 0xF4u
#define MOUSE_ACKNOWLEDGE 0xFAu

/* A press longer than this, without moving, counts as a right click. */
#define LONG_PRESS_TICKS 55u

static Event queue[QUEUE_SIZE];
static u8 queue_head;
static u8 queue_tail;

static s16 pointer_x = (s16)(SCREEN_WIDTH / 2u);
static s16 pointer_y = (s16)(SCREEN_HEIGHT / 2u);
static u8 pointer_buttons;
static u8 mouse_present;

/* Keyboard decoding state. */
static u8 shift_held;
static u8 extended_byte_coming;
static u8 bytes_to_ignore;

/* Mouse packet state. A packet is three bytes: flags, delta x, delta y. */
static u8 packet[3];
static u8 packet_index;

static s16 press_x;
static s16 press_y;
static u8 press_pending;
static u16 press_ticks;
static u8 long_press_sent;

/* ------------------------------------------------------------------ */
/* Event queue                                                         */
/* ------------------------------------------------------------------ */

static void queue_push(u8 type, u16 key, s16 x, s16 y) {
    u8 next = (u8)((queue_head + 1u) % QUEUE_SIZE);

    if (next == queue_tail) {
        return; /* full: drop the event instead of blocking the kernel */
    }
    queue[queue_head].type = type;
    queue[queue_head].key = key;
    queue[queue_head].buttons = pointer_buttons;
    queue[queue_head].x = x;
    queue[queue_head].y = y;
    queue_head = next;
}

u8 input_next_event(Event *event) {
    if (queue_tail == queue_head) {
        return 0u;
    }
    *event = queue[queue_tail];
    queue_tail = (u8)((queue_tail + 1u) % QUEUE_SIZE);
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Controller plumbing                                                 */
/* ------------------------------------------------------------------ */

static void controller_wait_until_ready_to_send(void) {
    u32 guard = 0u;

    while ((port_read_byte(PORT_KEYBOARD_STATUS) & STATUS_INPUT_FULL) != 0u) {
        if (++guard > 200000u) {
            return;
        }
    }
}

static void controller_wait_until_byte_arrives(void) {
    u32 guard = 0u;

    while ((port_read_byte(PORT_KEYBOARD_STATUS) & STATUS_OUTPUT_FULL) == 0u) {
        if (++guard > 200000u) {
            return;
        }
    }
}

static void controller_send(u8 command) {
    controller_wait_until_ready_to_send();
    port_write_byte(PORT_KEYBOARD_STATUS, command);
}

static void mouse_send(u8 command) {
    controller_wait_until_ready_to_send();
    port_write_byte(PORT_KEYBOARD_STATUS, COMMAND_WRITE_MOUSE);
    controller_wait_until_ready_to_send();
    port_write_byte(PORT_KEYBOARD_DATA, command);
}

static u8 mouse_reply(void) {
    controller_wait_until_byte_arrives();
    return port_read_byte(PORT_KEYBOARD_DATA);
}

static void controller_drain(void) {
    u8 guard = 0u;

    while ((port_read_byte(PORT_KEYBOARD_STATUS) & STATUS_OUTPUT_FULL) != 0u) {
        (void)port_read_byte(PORT_KEYBOARD_DATA);
        if (++guard > 32u) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/* Scan code set 1 to a key code. Letters and digits come back as their ASCII
 * value, everything else as one of the KEY_ constants. */
static u16 keyboard_translate(u8 scan_code) {
    const u8 shifted = (shift_held != 0u) ? 1u : 0u;
    char character = '\0';

    switch (scan_code) {
        case 0x01u: return KEY_ESCAPE;
        case 0x02u: return (u16)(shifted != 0u ? '!' : '1');
        case 0x03u: return (u16)(shifted != 0u ? '@' : '2');
        case 0x04u: return (u16)(shifted != 0u ? '#' : '3');
        case 0x05u: return (u16)(shifted != 0u ? '$' : '4');
        case 0x06u: return (u16)(shifted != 0u ? '%' : '5');
        case 0x07u: return (u16)(shifted != 0u ? '^' : '6');
        case 0x08u: return (u16)(shifted != 0u ? '&' : '7');
        case 0x09u: return (u16)(shifted != 0u ? '*' : '8');
        case 0x0Au: return (u16)(shifted != 0u ? '(' : '9');
        case 0x0Bu: return (u16)(shifted != 0u ? ')' : '0');
        case 0x0Cu: return (u16)(shifted != 0u ? '_' : '-');
        case 0x0Du: return (u16)(shifted != 0u ? '+' : '=');
        case 0x0Eu: return KEY_BACKSPACE;
        case 0x0Fu: return KEY_TAB;
        case 0x10u: character = 'q'; break;
        case 0x11u: character = 'w'; break;
        case 0x12u: character = 'e'; break;
        case 0x13u: character = 'r'; break;
        case 0x14u: character = 't'; break;
        case 0x15u: character = 'y'; break;
        case 0x16u: character = 'u'; break;
        case 0x17u: character = 'i'; break;
        case 0x18u: character = 'o'; break;
        case 0x19u: character = 'p'; break;
        case 0x1Au: return (u16)(shifted != 0u ? '{' : '[');
        case 0x1Bu: return (u16)(shifted != 0u ? '}' : ']');
        case 0x1Cu: return KEY_ENTER;
        case 0x1Eu: character = 'a'; break;
        case 0x1Fu: character = 's'; break;
        case 0x20u: character = 'd'; break;
        case 0x21u: character = 'f'; break;
        case 0x22u: character = 'g'; break;
        case 0x23u: character = 'h'; break;
        case 0x24u: character = 'j'; break;
        case 0x25u: character = 'k'; break;
        case 0x26u: character = 'l'; break;
        case 0x27u: return (u16)(shifted != 0u ? ':' : ';');
        case 0x28u: return (u16)(shifted != 0u ? '"' : '\'');
        case 0x29u: return (u16)(shifted != 0u ? '~' : '`');
        case 0x2Bu: return (u16)(shifted != 0u ? '|' : '\\');
        case 0x2Cu: character = 'z'; break;
        case 0x2Du: character = 'x'; break;
        case 0x2Eu: character = 'c'; break;
        case 0x2Fu: character = 'v'; break;
        case 0x30u: character = 'b'; break;
        case 0x31u: character = 'n'; break;
        case 0x32u: character = 'm'; break;
        case 0x33u: return (u16)(shifted != 0u ? '<' : ',');
        case 0x34u: return (u16)(shifted != 0u ? '>' : '.');
        case 0x35u: return (u16)(shifted != 0u ? '?' : '/');
        case 0x37u: return (u16)'*';
        case 0x39u: return (u16)' ';
        case 0x49u: return KEY_PAGE_UP;
        case 0x4Bu: return KEY_ARROW_LEFT;
        case 0x4Du: return KEY_ARROW_RIGHT;
        case 0x4Fu: return KEY_PAGE_DOWN;
        case 0x50u: return KEY_ARROW_DOWN;
        case 0x48u: return KEY_ARROW_UP;
        case 0x53u: return KEY_DELETE;
        default: return KEY_NOTHING;
    }

    return (u16)character;
}

static void keyboard_receive_byte(u8 scan_code) {
    const u8 released = (u8)((scan_code & 0x80u) != 0u);
    const u8 base = (u8)(scan_code & 0x7Fu);
    u16 key;

    if (bytes_to_ignore != 0u) { /* the Pause key sends six bytes */
        --bytes_to_ignore;
        return;
    }
    if (scan_code == 0xE1u) {
        bytes_to_ignore = 5u;
        return;
    }
    if (scan_code == 0xE0u) {
        extended_byte_coming = 1u;
        return;
    }

    if (released) {
        if (base == 0x2Au || base == 0x36u) {
            shift_held = 0u;
        }
        extended_byte_coming = 0u;
        return;
    }

    if (base == 0x2Au || base == 0x36u) { /* either shift key */
        shift_held = 1u;
        return;
    }

    /* With the 0xE0 prefix the arrows arrive as 0x48/0x4B/0x4D/0x50, the
     * same codes the number pad uses without the prefix. Both are useful. */
    key = keyboard_translate(base);
    extended_byte_coming = 0u;

    if (key != KEY_NOTHING) {
        queue_push(EVENT_KEY, key, pointer_x, pointer_y);
    }
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static void clamp_pointer(void) {
    if (pointer_x < 0) {
        pointer_x = 0;
    }
    if (pointer_x > (s16)(SCREEN_WIDTH - 1u)) {
        pointer_x = (s16)(SCREEN_WIDTH - 1u);
    }
    if (pointer_y < 0) {
        pointer_y = 0;
    }
    if (pointer_y > (s16)(SCREEN_HEIGHT - 1u)) {
        pointer_y = (s16)(SCREEN_HEIGHT - 1u);
    }
}

static void mouse_finish_packet(void) {
    const u8 flags = packet[0];
    const u8 left = (u8)((flags & 0x01u) != 0u);
    const u8 right = (u8)((flags & 0x02u) != 0u);
    const u8 left_was_down = (u8)((pointer_buttons & 0x01u) != 0u);
    const u8 right_was_down = (u8)((pointer_buttons & 0x02u) != 0u);
    s16 delta_x;
    s16 delta_y;

    if ((flags & 0xC0u) != 0u) {
        return; /* the controller reported an overflow, drop the packet */
    }

    /* The deltas are nine bit two's complement numbers: the low eight bits
     * are in the packet and the sign is a flag in the first byte. */
    delta_x = (s16)packet[1];
    if ((flags & 0x10u) != 0u) {
        delta_x = (s16)(delta_x - 256);
    }
    delta_y = (s16)packet[2];
    if ((flags & 0x20u) != 0u) {
        delta_y = (s16)(delta_y - 256);
    }

    pointer_buttons = (u8)(flags & 0x03u);

    if (delta_x != 0 || delta_y != 0) {
        pointer_x = (s16)(pointer_x + delta_x);
        /* Moving the mouse up reports a positive delta. */
        pointer_y = (s16)(pointer_y - delta_y);
        clamp_pointer();
        long_press_sent = 0u;
        if (left != 0u) {
            queue_push(EVENT_DRAG, KEY_NOTHING, pointer_x, pointer_y);
        } else {
            queue_push(EVENT_MOVE, KEY_NOTHING, pointer_x, pointer_y);
        }
    }

    if (left != 0u && left_was_down == 0u) {
        press_x = pointer_x;
        press_y = pointer_y;
        press_ticks = 0u;
        press_pending = 1u;
        long_press_sent = 0u;
        queue_push(EVENT_PRESS, KEY_NOTHING, pointer_x, pointer_y);
    }
    if (left == 0u && left_was_down != 0u) {
        press_pending = 0u;
        if (long_press_sent == 0u) {
            queue_push(EVENT_CLICK, KEY_NOTHING, pointer_x, pointer_y);
        }
        long_press_sent = 0u;
    }
    if (right != 0u && right_was_down == 0u) {
        queue_push(EVENT_RIGHT_CLICK, KEY_NOTHING, pointer_x, pointer_y);
    }
}

static void mouse_receive_byte(u8 value) {
    if (packet_index == 0u) {
        /* The first byte of a packet always has bit 3 set, which is how the
         * driver finds its way back after losing a byte. */
        if ((value & 0x08u) == 0u) {
            return;
        }
    }
    packet[packet_index] = value;
    ++packet_index;
    if (packet_index == 3u) {
        packet_index = 0u;
        mouse_finish_packet();
    }
}

/* Called once per frame and while waiting, so a press that is never released
 * still turns into a right click on a touch screen. */
static void mouse_update_long_press(u16 ticks) {
    if (press_pending == 0u || long_press_sent != 0u) {
        return;
    }
    press_ticks = (u16)(press_ticks + ticks);
    if (press_ticks >= LONG_PRESS_TICKS) {
        long_press_sent = 1u;
        queue_push(EVENT_RIGHT_CLICK, KEY_NOTHING, pointer_x, pointer_y);
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

void input_init(void) {
    shift_held = 0u;
    extended_byte_coming = 0u;
    bytes_to_ignore = 0u;
    packet_index = 0u;
    queue_head = 0u;
    queue_tail = 0u;
    pointer_buttons = 0u;
    pointer_x = (s16)(SCREEN_WIDTH / 2u);
    pointer_y = (s16)(SCREEN_HEIGHT / 2u);
    press_pending = 0u;
    long_press_sent = 0u;

    controller_drain();

    controller_send(COMMAND_ENABLE_MOUSE);
    mouse_send(MOUSE_SET_DEFAULTS);
    if (mouse_reply() == MOUSE_ACKNOWLEDGE) {
        /* VNC and Android clients often apply host acceleration. Explicit
         * PS/2 1:1 scaling keeps guest mickeys aligned with their cursor. */
        mouse_send(MOUSE_SET_SCALING_1_1);
        if (mouse_reply() == MOUSE_ACKNOWLEDGE) {
            mouse_send(MOUSE_START_STREAMING);
            mouse_present = (u8)(mouse_reply() == MOUSE_ACKNOWLEDGE);
        } else {
            mouse_present = 0u;
        }
    } else {
        mouse_present = 0u;
    }
    controller_drain();
}

void input_poll(void) {
    u8 guard = 0u;

    while ((port_read_byte(PORT_KEYBOARD_STATUS) & STATUS_OUTPUT_FULL) != 0u) {
        const u8 status = port_read_byte(PORT_KEYBOARD_STATUS);
        const u8 value = port_read_byte(PORT_KEYBOARD_DATA);

        if ((status & STATUS_FROM_MOUSE) != 0u) {
            mouse_receive_byte(value);
        } else {
            keyboard_receive_byte(value);
        }
        if (++guard > 64u) {
            return;
        }
    }
}

s16 input_pointer_x(void) {
    return pointer_x;
}

s16 input_pointer_y(void) {
    return pointer_y;
}

u8 input_pointer_buttons(void) {
    return pointer_buttons;
}

u8 input_mouse_available(void) {
    return mouse_present;
}

void input_advance(u16 ticks) {
    mouse_update_long_press(ticks);
}
