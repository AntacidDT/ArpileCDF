/* Input adapter: Arpile keyboard events → PrBoom D_PostEvent.
 * HID task pushes; the engine drains in I_StartTic via doom_input_poll(). */
#include <string.h>
#include <stdbool.h>
#include "doomtype.h"
#include "d_event.h"
#include "g_game.h"

#include "usb_host_input.h"
#include "doom_arpile.h"

#define RING_SZ 32

typedef struct { bool down; int key; } keyev_t;

static keyev_t s_ring[RING_SZ];
static volatile int s_head, s_tail;

void doom_input_post(bool down, int doom_key)
{
    int next = (s_head + 1) % RING_SZ;
    if (next == s_tail) {
        return;                       /* full: drop oldest-press overflow */
    }
    s_ring[s_head].down = down;
    s_ring[s_head].key = doom_key;
    s_head = next;
}

/* Drained by the engine thread once per tic. */
void doom_input_poll(void)
{
    event_t ev;
    while (s_tail != s_head) {
        ev.type = s_ring[s_tail].down ? ev_keydown : ev_keyup;
        ev.data1 = s_ring[s_tail].key;
        ev.data2 = ev.data3 = 0;
        D_PostEvent(&ev);
        s_tail = (s_tail + 1) % RING_SZ;
    }
}

/* Map an Arpile HID keycode to a PrBoom action-key reference. Returns the
 * current value of the bound engine key (gamepad.c dereference pattern). */
int doom_input_translate(uint16_t arpile_keycode)
{
    switch (arpile_keycode) {
    case ARPILE_KEY_W:       return key_up;
    case ARPILE_KEY_S:       return key_down;
    case ARPILE_KEY_D:       return key_straferight;
    case ARPILE_KEY_A:       return key_strafeleft;
    case ARPILE_KEY_O:       return key_left;
    case ARPILE_KEY_P:       return key_right;
    case ARPILE_KEY_E:       return key_fire;
    case ARPILE_KEY_SPACE:   return key_fire;
    case ARPILE_KEY_UP:      return key_up;
    case ARPILE_KEY_DOWN:    return key_down;
    case ARPILE_KEY_LEFT:    return key_left;
    case ARPILE_KEY_RIGHT:   return key_right;
    case ARPILE_KEY_ENTER:   return key_menu_enter;
    case ARPILE_KEY_ESCAPE:  return key_menu_escape;
    case ARPILE_KEY_BACKSPACE: return key_menu_backspace;
    default: break;
    }
    if (arpile_keycode >= 0x04 && arpile_keycode <= 0x1D) {
        return 'a' + (arpile_keycode - 0x04);
    }
    if (arpile_keycode >= 0x1E && arpile_keycode <= 0x27) {
        const char digits[10] = { '9', '8', '7', '6', '5', '4', '3', '2', '1', '0' };
        int idx = arpile_keycode - 0x1E;
        if (idx <= 5) {
            return '1' + idx;
        }
        return digits[idx];
    }
    if (arpile_keycode == 0x2B) {
        return key_map ? key_map : '\t';
    }
    return 0;
}
