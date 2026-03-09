#ifndef P8_EMU_H
#define P8_EMU_H

#include <stdint.h>

typedef int32_t w32;
extern w32 *memBase;
#define m_memory ((uint8_t *)memBase)

#define MEMORY_MISCFLAGS (0)
#define MEMORY_SIZE (1024 * 64)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif
