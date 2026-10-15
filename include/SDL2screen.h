/*
 * Copyright (c) 2020-2021 Graeme Gregory
 *
 * SPDX: Zlib
 */

#ifndef _SDL2SCREEN_H
#define _SDL2SCREEN_H

#include <SDL.h>
#include <stdbool.h>

void QLSDLScreen(void);
void QLSDLRenderScreen(void);
void QLSDLProcessEvents(void);
void QLSDLExit(void);
Uint32 QLSDL50Hz(Uint32 interval, void *param);
void QLSDLUpdateScreenWord(uint32_t, uint16_t);
void QLSDLUpdateScreenLong(uint32_t, uint32_t);
void QLSDLWritePixels(uint32_t *pixelPtr32);

void QLSDLCreatePalette(const SDL_PixelFormat *format);
void QLSDLCreateIcon(SDL_Window *window);

#ifdef NEXTP8
extern unsigned int sdl_keyrow[32];
extern unsigned int sdl_keyrow_latched[32];
extern int16_t sdl_mouse_x_accum;
extern int16_t sdl_mouse_y_accum;
extern int16_t sdl_mouse_z_accum;
extern unsigned int sdl_mouse_buttons;
extern unsigned int sdl_mouse_buttons_latched;
uint32_t key_queue_pop(void);
uint32_t key_queue_peek(void);
void key_queue_clear(void);
#else
extern unsigned int sdl_keyrow[8];
#endif
extern int sdl_shiftstate, sdl_controlstate, sdl_altstate;

extern SDL_atomic_t doPoll;
extern SDL_sem* sem50Hz;
extern bool shaders_selected;
extern bool ql_fullscreen;
extern double ql_screen_ratio;

/* Keyboard row change function for testbench */
void SDLQLKeyrowChg(int code, int press, unsigned keymod);

/* FuncVal testbench functions */
void QLSDLSaveFuncvalScreenshot(const char *filename);
int QLSDLReadFramebufferPixel(int x, int y, uint16_t *pixel);

/* Key queue functions */
void key_queue_push(uint32_t key_event);

#define USER_CODE_SCREENREFRESH     0
#define USER_CODE_EMUEXIT           1

#ifdef NEXTP8
extern uint8_t joy_state[2];
extern uint8_t joy_latched[2];
#endif

#endif
