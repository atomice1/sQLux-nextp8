/*
 * p8audio_verilated.h
 *
 * C interface for the Verilator-based p8audio model.
 *
 * Called from p8audio.c to forward MMIO register writes directly to the
 * Verilated model's port-level interface.
 *
 * Copyright (C) 2026 Chris January
 * GPL-3
 */

#ifndef P8AUDIO_VERILATED_H
#define P8AUDIO_VERILATED_H

#include <stdint.h>

#define P8AUDIO_VERSION UINT16_C(0)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue a 16-bit MMIO write to the Verilated p8audio model.
 *
 * byte_addr: the byte address within the p8audio register space, matching
 *            the ADDR_* localparams in p8audio.sv (e.g. 0x02 for CTRL,
 *            0x18 for SFX_CMD, 0x1C for MUSIC_CMD).
 * data:      16-bit write data.
 *
 * Thread-safe: may be called from any thread.  The write is queued for
 * the PB (playback) model and applied in the SDL audio callback thread.
 * The RT (real-time) model is advanced immediately and processes the
 * write synchronously in the calling thread.
 *
 * Both models receive the same write command to stay synchronized.
 */
void p8audio_verilated_mmio_write(uint8_t byte_addr, uint16_t data);

/*
 * Read a stat register from the Verilated p8audio model.
 *
 * byte_offset: byte offset within the p8audio register space (0x20..0x36,
 *              must be even), corresponding to ADDR_STAT46..ADDR_STAT57 in
 *              p8audio.sv (i.e. the value of addr - _P8AUDIO_BASE).
 * Returns:     16-bit register value; 0 if the offset is out of range.
 *
 * Thread-safe: reads from the RT model (real-time) which is advanced on
 * each MMIO operation. Returns current RT model state.
 */
uint16_t p8audio_verilated_mmio_read(uint8_t byte_offset);

/*
 * Read PCM output samples from the Verilated p8audio model.
 *
 * These functions advance the model by one sample and return the PCM value.
 * They are thread-safe and can be called from any thread.
 *
 * Returns: 16-bit signed PCM sample (left or right channel)
 */
int16_t p8audio_verilated_read_pcm_left(void);
int16_t p8audio_verilated_read_pcm_right(void);

/* SDL audio lifecycle — implemented in p8audio_verilated.cpp */
void p8audio_verilated_init(void);

#ifdef __cplusplus
}
#endif

#endif /* P8AUDIO_VERILATED_H */
