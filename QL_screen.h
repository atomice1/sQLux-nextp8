/*
 * (c) UQLX - see COPYRIGHT
 */
#pragma once

#include <stddef.h>

typedef struct _SCREEN_SPECS {
	uint32_t qm_lo;
	uint32_t qm_hi;
	uint32_t qm_len;

	uint32_t linel;
	int yres;
	int xres;
} screen_specs;

extern screen_specs qlscreen;

void QLPatchPTRENV(void);

#ifdef NEXTP8
extern uint8_t frameBuffer[2][8192];
extern uint8_t overlayBuffer[2][8192];
extern uint8_t screenPalette[2][16]; /* [bank][byte_offset]; bank selected by vfront */
extern int vfront, vfrontreq;
extern uint8_t overlay_control;
extern uint8_t vblank_intr_enable;
extern uint8_t screen_transform;
extern uint8_t high_colour_mode;
extern uint8_t secondaryPalette[2][16];
extern uint8_t highColourBitfield[2][16];
#endif
