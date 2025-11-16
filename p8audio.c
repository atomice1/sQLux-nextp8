#include <stdio.h>
#include "p8audio.h"
#include "p8_audio.h"

uint16_t p8audio_control;
uint16_t p8audio_sfx_base_hi;
uint16_t p8audio_sfx_base_lo;
uint16_t p8audio_music_base_hi;
uint16_t p8audio_music_base_lo;
uint16_t p8audio_sfx_length;
uint16_t p8audio_music_fade_time;

void p8audio_sfx_command(uint16_t command)
{
    int32_t index = command & 0x3f;
    if (index & 0x20) index = -(((~index) & 0x3f) + 1);
    int32_t channel = (command >> 12) & 0x7;
    if (channel & 0x4) channel = -(((~channel) & 0x7) + 1);
    uint32_t start = (command >> 6) & 0x3f;
    uint32_t end = p8audio_sfx_length & 0x3f;
    if (end == 0) end = 32;
    printf("SFX(%d, %d, %u, %u)\n", (int) index, (int) channel, (unsigned) start, (unsigned) end);
    audio_sound(index, channel, start, end);
}

void p8audio_music_command(uint16_t command)
{
    int32_t index = (command >> 7) & 0x3f;
    if (index & 0x20) index = -(((~index) & 0x3f) + 1);
    int32_t fade_ms = p8audio_music_fade_time;
    int32_t mask = (command >> 3) & 0xf;
    if (mask == 0) mask = 0x7;
    printf("MUSIC(%d, %d, %d)\n", (int) index, (int) fade_ms, (int) mask);
    audio_music(index, fade_ms, mask);
}
