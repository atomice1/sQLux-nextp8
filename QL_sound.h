/* sQLux Sound */
#ifndef _QL_sound_
#define _QL_sound_

#include <stdbool.h>

#ifdef NEXTP8
#include "nextp8.h"
#endif

extern volatile bool soundOn;

void initSound(int volume);
void BeepSound(unsigned char*);
void KillSound(void);
void closeSound();

#ifdef NEXTP8
extern bool da_start;
extern bool da_mono;
extern uint16_t da_period;
extern unsigned da_address;

#define DA_SAMPLES (_DA_MEMORY_SIZE / 2)
extern int16_t da_memory[DA_SAMPLES];
#endif
#endif
