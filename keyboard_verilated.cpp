/*
 * keyboard_verilated.cpp
 *
 * Verilator-based keyboard matrix controller integration.
 *
 * Wraps the Verilated keyboard_top.sv module:
 *   - keyboard_top: Instantiates mkeyboard and kbd_event.
 *
 * The keyboard matrix is scanned by mkeyboard, which outputs a 256-bit bitfield
 * where each bit represents a key state. kbd_event compares the current and
 * previous matrix states to detect key presses and releases.
 */

#include "keyboard_verilated.h"
#include "p8_emu.h"     /* m_memory, memBase */
#include "SDL2screen.h" /* key_queue_push, sdl_keyrow, sdl_keyrow_latched */

#include "Vkbd_top.h"
#include "Vkbd_top___024root.h"
#include "verilated.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "verilated_types.h"

/* External asyncTrace flag for test logging */
extern bool asyncTrace;

#define DEBUG (asyncTrace)

/*==============================================================
 * Verilated model state
 *==============================================================*/

static Vkbd_top   *s_keyboard_top = nullptr;
static VerilatedContext *s_vl_ctx      = nullptr;

/* Matrix state cache - 8 rows × 7 columns */
static uint8_t s_matrix[8];

/*==============================================================
 * Clock helpers
 *==============================================================*/

/* Decode active row from keyb_row_o (one-hot, active-low) */
static inline int decode_active_row(uint8_t keyb_row)
{
    // keyb_row is active-low one-hot: bit 0 = row 0, bit 7 = row 7
    // Return the row index (0-7) or 0 if no row active
    if ((keyb_row & 0x80) == 0) return 7;
    if ((keyb_row & 0x40) == 0) return 6;
    if ((keyb_row & 0x20) == 0) return 5;
    if ((keyb_row & 0x10) == 0) return 4;
    if ((keyb_row & 0x08) == 0) return 3;
    if ((keyb_row & 0x04) == 0) return 2;
    if ((keyb_row & 0x02) == 0) return 1;
    if ((keyb_row & 0x01) == 0) return 0;
    return 0;  // No row active
}

/* Single mclk half-cycle */
static inline void tick_mclk(int level)
{
    s_keyboard_top->mclk = level;
    s_keyboard_top->eval();
}

/*==============================================================
 * Initialize Verilated models
 *==============================================================*/
void keyboard_verilated_init(void)
{
    if (s_keyboard_top != nullptr) {
        return;  /* Already initialized */
    }

    /* Create Verilated context and model */
    s_vl_ctx = new VerilatedContext;
    s_keyboard_top = new Vkbd_top(s_vl_ctx, "keyboard_top");

    /* Reset the model */
    s_keyboard_top->reset = 1;

    /* Initialize inputs */
    s_keyboard_top->keyb_col_i = 0x7F;  /* All columns released */
    s_keyboard_top->eval();

    s_keyboard_top->eval();

    /* Hold reset for several cycles */
    for (int i = 0; i < 8; i++) {
        tick_mclk(0);
        tick_mclk(1);
    }

    /* Deassert reset */
    s_keyboard_top->reset = 0;
    s_keyboard_top->eval();

    /* Initialize matrix state */
    memset(s_matrix, 0xFF, sizeof(s_matrix));  /* All keys released (1 = released) */

    printf("[keyboard_verilated] Initialized keyboard_top (mkeyboard + kbd_event)\n");
}

/*==============================================================
 * Set a column bit in the keyboard matrix
 *==============================================================*/
void keyboard_verilated_set_col(int row, int col, bool pressed)
{
    assert(row >= 0 && row < 8);
    assert(col >= 0 && col < 7);

    /* Columns are active-low: 0 = pressed, 1 = released */
    uint8_t mask = (1 << col);
    uint8_t prev_value = s_matrix[row];
    uint8_t new_value;

    if (pressed) {
        new_value = s_matrix[row] & ~mask;  /* Clear bit to press key */
    } else {
        new_value = s_matrix[row] | mask;   /* Set bit to release key */
    }

    s_matrix[row] = new_value;

    /* Trace logging for matrix changes */
    if (DEBUG && (prev_value != new_value)) {
        fprintf(stderr, "[keyboard_verilated] Matrix change: row=%d col=%d pressed=%d old=0x%02X new=0x%02X\n",
                row, col, pressed, prev_value, new_value);
    }
}

/*==============================================================
 * Tick keyboard_top N times, processing any events.
 * kbd_push_req is a pulse that goes high for one cycle when an event is ready.
 * We must check for this pulse after EACH tick, not just at the end.
 *==============================================================*/
void keyboard_verilated_tick_n(int n)
{
    if (DEBUG && n > 0) {
        fprintf(stderr, "[keyboard_verilated] Ticking keyboard_top %d cycles...\n", n);
    }

    for (int i = 0; i < n; i++) {
        /* Tick one mclk cycle */
        tick_mclk(0);
        tick_mclk(1);

        /* Monitor keyb_row_o to determine which row is currently active.
         * Drive keyb_col_i with the column data for the active row.
         */
        int active_row = decode_active_row(s_keyboard_top->keyb_row_o);

        /* Drive the column data for the currently active row */
        s_keyboard_top->keyb_col_i = s_matrix[active_row] & 0x7F;

        /* Process any kbd_push_req pulses generated during this tick.
         * The pulse is high for exactly one cycle, so we must check after each tick.
         */
        while (s_keyboard_top->kbd_push_req != 0) {
            uint32_t event = s_keyboard_top->kbd_push_data;

            if (DEBUG) {
                int release = (event >> 31) & 1;
                int membrane = (event >> 30) & 1;
                uint8_t scancode = event & 0xFF;
                fprintf(stderr, "[keyboard_verilated] kbd_push_req pulse: event=0x%08X %s membrane=%d scancode=0x%02X\n",
                        event, release ? "release" : "press", membrane, scancode);
            }

            key_queue_push(event);

            /* Decode the keyboard event and update sdl_keyrow directly */
            int press = !((event >> 31) & 1);  /* 0 = press, 1 = release */
            uint8_t scancode = event & 0xFF;

#ifdef NEXTP8
            int row = scancode / 8;
#else
            int row = 7 - scancode / 8;
#endif
            int col = 0x1 << (scancode % 8);

            /* Extract modifiers from event (bits 23-16
            /* Update sdl_keyrow and sdl_keyrow_latched */
            if (press) {
#ifdef NEXTP8
                if (!(sdl_keyrow[row] & col)) {
                    sdl_keyrow_latched[row] |= col;
                }
#endif
                sdl_keyrow[row] |= col;
            } else {
                sdl_keyrow[row] &= ~col;
            }

            /* Clear the push request */
            s_keyboard_top->kbd_push_req = 0;

            /* Tick again to allow kbd_event to process next event */
            tick_mclk(0);
            tick_mclk(1);
        }
    }

    if (DEBUG && n > 0) {
        fprintf(stderr, "[keyboard_verilated] Tick complete, omatrix=0x%02X%02X%02X%02X%02X%02X%02X%02X kbd_push_req=%d\n",
                (unsigned int)(s_keyboard_top->omatrix[7]),
                (unsigned int)(s_keyboard_top->omatrix[6]),
                (unsigned int)(s_keyboard_top->omatrix[5]),
                (unsigned int)(s_keyboard_top->omatrix[4]),
                (unsigned int)(s_keyboard_top->omatrix[3]),
                (unsigned int)(s_keyboard_top->omatrix[2]),
                (unsigned int)(s_keyboard_top->omatrix[1]),
                (unsigned int)(s_keyboard_top->omatrix[0]),
                s_keyboard_top->kbd_push_req);
    }
}

/*==============================================================
 * Tick the Verilated model once (convenience wrapper)
 *==============================================================*/
void keyboard_verilated_tick(void)
{
    keyboard_verilated_tick_n(1);
}

/*==============================================================
 * Process any keyboard events generated by kbd_event.
 * This is now a no-op because events are processed inline during tick_n.
 * Kept for API compatibility.
 *==============================================================*/
void keyboard_verilated_process_events(void)
{
    /* Events are now processed inline during keyboard_verilated_tick_n().
     * This function is kept for API compatibility but does nothing.
     */
}

/*==============================================================
 * Check if a keyboard event is available
 *==============================================================*/
bool keyboard_verilated_has_event(void)
{
    return (s_keyboard_top->kbd_push_req != 0);
}

/*==============================================================
 * Get the next keyboard event from the event queue
 *==============================================================*/
uint32_t keyboard_verilated_get_event(void)
{
    uint32_t event = s_keyboard_top->kbd_push_data;

    /* Clear the push request */
    s_keyboard_top->kbd_push_req = 0;

    return event;
}

/*==============================================================
 * Cleanup
 *==============================================================*/
void keyboard_verilated_shutdown(void)
{
    if (s_keyboard_top) {
        s_keyboard_top->final();
        delete s_keyboard_top;
        s_keyboard_top = nullptr;
    }
    if (s_vl_ctx) {
        delete s_vl_ctx;
        s_vl_ctx = nullptr;
    }
}
