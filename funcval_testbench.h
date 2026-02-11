/*
 * FuncVal Testbench MMIO Support for sQLux-nextp8
 *
 * Implements a subset of the Vivado FuncVal testbench peripheral MMIO
 * functionality, allowing test programs to run in software simulation
 * with similar behavior to hardware simulation.
 *
 * Address Range: 0x300000 - 0x3FFFFF (3MB-4MB)
 *   - 0x300000-0x37FFFF: Pin control/VGA framebuffer readback
 *   - 0x380000-0x3FFFFF: Peripheral control (keyboard, mouse, screenshot)
 */

#ifndef FUNCVAL_TESTBENCH_H
#define FUNCVAL_TESTBENCH_H

#include "QL68000.h"
#include <SDL2/SDL.h>

/* FuncVal testbench initialization */
void funcval_init(void);

/* FuncVal testbench memory access handlers */
rw8 funcval_read_byte(aw32 addr);
rw16 funcval_read_word(aw32 addr);
rw32 funcval_read_long(aw32 addr);
void funcval_write_byte(aw32 addr, aw8 data);
void funcval_write_word(aw32 addr, aw16 data);
void funcval_write_long(aw32 addr, aw32 data);

/* Check if address is in FuncVal testbench range */
int funcval_is_testbench_addr(aw32 addr);

/* SDL audio shims for audio recording */
int SDL_OpenAudio_shim(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
SDL_AudioDeviceID SDL_OpenAudioDevice_shim(const char *device, int iscapture,
                                            const SDL_AudioSpec *desired,
                                            SDL_AudioSpec *obtained,
                                            int allowed_changes);

#endif /* FUNCVAL_TESTBENCH_H */
