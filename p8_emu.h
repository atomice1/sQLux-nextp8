#ifndef P8_EMU_H
#define P8_EMU_H

#include <stdint.h>
#include "p8audio.h"

typedef int32_t w32;
extern w32 *memBase;
#define m_memory ((uint8_t *)memBase)

#define MEMORY_MUSIC (p8audio_music_base_lo | (p8audio_music_base_hi << 16))
#define MEMORY_SFX (p8audio_sfx_base_lo | (p8audio_sfx_base_hi << 16))

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif