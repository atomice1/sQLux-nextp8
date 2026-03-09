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
 * Thread-safe: may be called from any thread.  The write is queued and
 * applied in the SDL audio callback thread before the next batch of
 * samples is generated.
 */
void p8audio_verilated_mmio_write(uint8_t byte_addr, uint16_t data);

/*
 * Read a stat register from the Verilated p8audio model's shadow cache.
 *
 * byte_offset: byte offset within the p8audio register space (0x20..0x36,
 *              must be even), corresponding to ADDR_STAT46..ADDR_STAT57 in
 *              p8audio.sv (i.e. the value of addr - _P8AUDIO_BASE).
 * Returns:     16-bit register value; 0 if the offset is out of range.
 *
 * Thread-safe: reads from a shadow cache updated each sample by the SDL
 * audio thread.  Stale by at most ~45 µs (one sample period).
 */
uint16_t p8audio_verilated_mmio_read(uint8_t byte_offset);

/* SDL audio lifecycle — implemented in p8audio_verilated.cpp */
void p8audio_verilated_init(void);

#ifdef __cplusplus
}
#endif

#endif /* P8AUDIO_VERILATED_H */
