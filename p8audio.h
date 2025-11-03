#ifndef P8AUDIO_H
#define P8AUDIO_H

#include <stdint.h>

#define P8AUDIO_VERSION UINT16_C(0)

extern uint16_t p8audio_control;
extern uint16_t p8audio_sfx_base_hi;
extern uint16_t p8audio_sfx_base_lo;
extern uint16_t p8audio_music_base_hi;
extern uint16_t p8audio_music_base_lo;
extern uint16_t p8audio_sfx_length;
extern uint16_t p8audio_music_fade_time;

extern void p8audio_sfx_command(uint16_t command);
extern void p8audio_music_command(uint16_t command);

#endif
