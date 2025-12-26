// Client-side inline event recording functions
//
// This header provides fast inline functions for recording profiler events
// from the main emulation thread.
// This header is C-compatible and can be included from both C and C++ code.

#ifndef PROFILER_EVENTS_H
#define PROFILER_EVENTS_H

#include <stdint.h>

#ifdef PROFILER
#include "profiler_cost_model.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Global buffer pointers managed by profiler_client.cpp
extern uint32_t* profiler_current_buffer_ptr;
extern uint32_t* profiler_buffer_end_ptr;

// Function to switch to a new buffer when current is full
void Profiler_SwitchBuffer(void);

// Inline event recording functions - direct pointer manipulation for maximum speed
// Each event is encoded as: (type << 24) | (address & 0xffffff)

static inline void Profiler_RecordInstructionExecute(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x00000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
}

static inline void Profiler_RecordJump(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x10000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
}

static inline void Profiler_RecordCall(uint32_t address, uint32_t return_offset) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x20000000 | (return_offset << 24);
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
}

static inline void Profiler_RecordReturn(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x30000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
}

static inline void Profiler_RecordDataRead(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x40000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
#ifdef PROFILER
    Profiler_RecordDataReadCycles();
#endif
}

static inline void Profiler_RecordDataWrite(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x50000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr) {
        Profiler_SwitchBuffer();
    }
#ifdef PROFILER
    Profiler_RecordDataWriteCycles();
#endif
}

static inline void Profiler_RecordInstrRead(uint32_t address) {
    *profiler_current_buffer_ptr++ = (address & 0xffffff) | 0x60000000;
    if (profiler_current_buffer_ptr == profiler_buffer_end_ptr)
        Profiler_SwitchBuffer();
#ifdef PROFILER
    Profiler_RecordInstrReadCycles();
#endif
}

#ifdef __cplusplus
}
#endif

#endif // PROFILER_EVENTS_H
