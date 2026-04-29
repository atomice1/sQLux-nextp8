/*
 * p8audio_verilated.cpp
 *
 * Verilator-based replacement for p8_audio.c / p8_dsp.c.
 *
 * Wraps the Verilated p8audio.sv + p8sfx_core_mux.sv model and exposes:
 *   - p8audio_verilated_init  (called from SDL2main.c)
 *   - p8audio_verilated_mmio_write  (called from reworked p8audio.c)
 *
 * Clock scheduling (all driven in the SDL audio callback thread):
 *
 *   mclk      = 40 MHz
 *   clk_pcm   = 22.05 kHz  (one sample per edge)
 *   clk_pcm_8x = 176.4 kHz (8 × clk_pcm, time-multiplexes 4 voices × 2 instr)
 *
 *   Per PCM sample: 8 clk_pcm_8x sub-slots × MCLK_PER_8X mclk ticks each,
 *   then one clk_pcm rising edge.  DMA requests are serviced inline during
 *   each mclk tick.
 *
 * Copyright (C) 2026 Chris January
 * GPL-3
 */

#include "p8audio_verilated.h"
#include "p8_emu.h"     /* m_memory, memBase */

#include "Vp8audio.h"
#include "Vp8audio___024root.h"
#include "verilated.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <pthread.h>
#include <stdint.h>

/*==============================================================
 * Clock ratios
 *==============================================================*/
static const int PCM_8X_PER_PCM   = 8;

/* PCM output width — must match p8audio.sv parameter PCM_WID */
#define PCM_WID 12

/* SDL output configuration */
static const int SAMPLE_RATE_HW   = 22050;  /* matches clk_pcm */
static const int SDL_BUFFER_SAMPLES = 1024; /* ~46 ms */

/*==============================================================
 * MMIO command queue (CPU thread → audio thread)
 *==============================================================*/
struct mmio_cmd_t {
    uint8_t  byte_addr;
    uint16_t data;
};

static const int QUEUE_CAPACITY = 1024;

static mmio_cmd_t   s_queue_buf[QUEUE_CAPACITY];
static int          s_queue_head = 0;   /* next slot to read  */
static int          s_queue_tail = 0;   /* next slot to write */
static int          s_queue_count = 0;
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Push a write – silently drops if queue is full (shouldn't happen). */
extern "C" void p8audio_verilated_mmio_write(uint8_t byte_addr, uint16_t data)
{
    pthread_mutex_lock(&s_queue_mutex);
    if (s_queue_count < QUEUE_CAPACITY) {
        s_queue_buf[s_queue_tail].byte_addr = byte_addr;
        s_queue_buf[s_queue_tail].data      = data;
        s_queue_tail = (s_queue_tail + 1) % QUEUE_CAPACITY;
        s_queue_count++;
    } else {
        fprintf(stderr, "[p8audio_verilated] MMIO queue overflow – dropped write addr=0x%02x data=0x%04x\n",
                byte_addr, data);
    }
    pthread_mutex_unlock(&s_queue_mutex);
}

/* Pop one entry.  Returns false if empty.  Call from audio thread only. */
static bool queue_pop(mmio_cmd_t *out)
{
    pthread_mutex_lock(&s_queue_mutex);
    if (s_queue_count == 0) {
        pthread_mutex_unlock(&s_queue_mutex);
        return false;
    }
    *out = s_queue_buf[s_queue_head];
    s_queue_head = (s_queue_head + 1) % QUEUE_CAPACITY;
    s_queue_count--;
    pthread_mutex_unlock(&s_queue_mutex);
    return true;
}

/*==============================================================
 * Verilated model state
 *==============================================================*/
static Vp8audio        *s_model     = nullptr;
static VerilatedContext *s_vl_ctx   = nullptr;

/*==============================================================
 * Stat shadow cache (audio thread writes, CPU thread reads)
 *
 * Mirrors ADDR_STAT46..ADDR_STAT57 from p8audio.sv:
 *   index 0-3  : STAT46-49  SFX slot per channel (0xFFFF = idle)
 *   index 4-7  : STAT50-53  note index per channel (0xFFFF = idle)
 *   index 8    : STAT54     music pattern index
 *   index 9    : STAT55     music pattern loop count
 *   index 10   : STAT56     music note-tick count
 *   index 11   : STAT57     music playing flag (0 or 1)
 *==============================================================*/
static const uint8_t k_stat_offsets[12] = {
    0x20, 0x22, 0x24, 0x26,   /* STAT46-49: SFX slot per channel     */
    0x28, 0x2A, 0x2C, 0x2E,   /* STAT50-53: note index per channel   */
    0x30, 0x32, 0x34, 0x36    /* STAT54-57: music pattern/count/tick/playing */
};
static uint16_t        s_stat_cache[12];
static pthread_mutex_t s_stat_mutex = PTHREAD_MUTEX_INITIALIZER;

/*==============================================================
 * Clock helpers
 *==============================================================*/

/* Single mclk half-cycle. */
static inline void tick_mclk(int level)
{
    s_model->mclk = level;
    s_model->eval();
}

/* Single clk_pcm_8x half-cycle. */
static inline void tick_8x(int level)
{
    s_model->clk_pcm_8x = level;
    s_model->eval();
}

/* Single clk_pcm half-cycle. */
static inline void tick_pcm(int level)
{
    s_model->clk_pcm = level;
    s_model->eval();
}

/*==============================================================
 * DMA service
 * Called after every mclk rising edge.  If the model has asserted
 * dma_req, provide the requested word from base RAM and pulse dma_ack.
 *==============================================================*/
static void service_dma(void)
{
    if (!s_model->dma_req)
        return;

    /* dma_addr is a 31-bit word address; each word is 16 bits, big-endian. */
    uint32_t byte_addr = (uint32_t)s_model->dma_addr * 2u;

    /* memBase is int32_t* so m_memory = (uint8_t*)memBase */
    s_model->dma_rdata = (uint16_t)(((uint16_t)m_memory[byte_addr] << 8) |
                                     (uint16_t)m_memory[byte_addr + 1u]);

    /* Pulse dma_ack for one mclk cycle */
    s_model->dma_ack = 1;
    tick_mclk(0);   /* falling edge */
    tick_mclk(1);   /* rising edge with ack=1 */
    s_model->dma_ack = 0;
}

/*==============================================================
 * Flush queued MMIO writes into the model via mclk transactions.
 * Called from the audio thread before each sample batch.
 *==============================================================*/
static void flush_mmio_queue(void)
{
    mmio_cmd_t cmd;
    while (queue_pop(&cmd)) {
        /* The 7-bit 'address' port is the byte-address shifted right by 1
         * (word address).  nUDS and nLDS are both active (0) for 16-bit
         * writes. */
        s_model->address  = cmd.byte_addr >> 1;
        s_model->din      = cmd.data;
        s_model->nUDS     = 0;
        s_model->nLDS     = 0;
        s_model->write_en = 1;
        s_model->read_en  = 0;

        tick_mclk(0);   /* setup on falling edge */
        tick_mclk(1);   /* latch on rising edge  */
        service_dma();

        s_model->write_en = 0;
        s_model->nUDS     = 1;
        s_model->nLDS     = 1;

        tick_mclk(0);
        tick_mclk(1);
        service_dma();
    }
}

/*==============================================================
 * Snapshot stat registers into the shadow cache.
 * Called from the audio thread after every PCM sample.
 * The read mux in p8audio.sv is combinational: setting address +
 * eval() makes dout valid immediately without a clock edge.
 *==============================================================*/
static void update_stat_cache(void)
{
    uint16_t tmp[12];
    s_model->read_en  = 1;
    s_model->write_en = 0;
    for (int i = 0; i < 12; i++) {
        s_model->address = k_stat_offsets[i] >> 1;
        s_model->eval();
        tmp[i] = s_model->dout;
    }
    s_model->read_en = 0;
    s_model->eval();

    pthread_mutex_lock(&s_stat_mutex);
    memcpy(s_stat_cache, tmp, sizeof(tmp));
    pthread_mutex_unlock(&s_stat_mutex);
}

/*==============================================================
 * Advance model by exactly one PCM sample.
 *
 * Schedule:
 *   for slot in 0..7:
 *     rise clk_pcm_8x
 *     fall clk_pcm_8x
 *     rise mclk, service DMA, fall mclk   ← interleaved after each 8x slot
 *   rise clk_pcm  → pcm_out is latched
 *   fall clk_pcm
 *
 * One mclk tick AFTER each clk_pcm_8x slot allows the 2-stage CDC
 * synchronizer to capture the sfx_done (voice_done_pcm) pulse that fires
 * in the clk_pcm_8x domain.  Without this interleaving all 8 sub-slots
 * complete before any mclk edge, so the pulse is always missed and the
 * music sequencer never advances past the first pattern.
 *
 * Returns the new pcm_out value (signed PCM_WID-bit, sign-extended to int16_t).
 *==============================================================*/
static int16_t advance_one_sample(void)
{
    for (int slot = 0; slot < PCM_8X_PER_PCM; slot++) {
        tick_8x(1);
        tick_8x(0);
        tick_mclk(1);
        if (s_model->dma_req) {
            for (int i=0;i<200;++i) {
                service_dma();
                tick_mclk(0);
                tick_mclk(1);
            }
            service_dma();
        }
        tick_mclk(0);
    }

    tick_pcm(1);   /* clk_pcm rising – model outputs sample */
    tick_pcm(0);
    /* Sign-extend PCM_WID-bit pcm_out to int16_t. */
    int16_t s = (int16_t)((s_model->pcm_out ^ (1u << (PCM_WID - 1))) - (1u << (PCM_WID - 1)));
    update_stat_cache();
    return s;
}

/*==============================================================
 * SDL audio callback
 *==============================================================*/
static void audio_callback_verilated(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    int16_t *buf     = (int16_t *)stream;
    int      samples = len / (int)sizeof(int16_t);

    flush_mmio_queue();

    for (int i = 0; i < samples; i++) {
        int16_t s_pcm = advance_one_sample();
        /* Scale signed PCM_WID-bit → signed 16-bit */
        buf[i] = s_pcm << (16 - PCM_WID);
    }
}

/*==============================================================
 * Reset the Verilated model and hold for a few cycles.
 *==============================================================*/
static void model_reset(void)
{
    s_model->resetn_sys   = 0;
    s_model->resetn_pcm   = 0;
    s_model->resetn_pcm_8x = 0;

    s_model->mclk       = 0;
    s_model->clk_pcm    = 0;
    s_model->clk_pcm_8x = 0;

    s_model->address  = 0;
    s_model->din      = 0;
    s_model->nUDS     = 1;
    s_model->nLDS     = 1;
    s_model->write_en = 0;
    s_model->read_en  = 0;
    s_model->dma_rdata = 0;
    s_model->dma_ack   = 0;

    s_model->eval();

    /* Drive reset for several cycles on each domain */
    for (int i = 0; i < 12; i++) { tick_mclk(1); tick_mclk(0); }
    for (int i = 0; i < 5;  i++) { tick_8x(1);   tick_8x(0);   }
    for (int i = 0; i < 3;  i++) { tick_pcm(1);  tick_pcm(0);  }

    s_model->resetn_sys    = 1;
    s_model->resetn_pcm    = 1;
    s_model->resetn_pcm_8x = 1;
    s_model->eval();

    /* Settle after reset deassertion */
    for (int i = 0; i < 4; i++) { tick_mclk(1); tick_mclk(0); }

    /* Populate initial stat cache */
    update_stat_cache();
}

/*==============================================================
 * Public API (implements p8_audio.h declarations)
 *==============================================================*/

static SDL_AudioSpec s_audio_spec;
static bool          s_model_init = false;
static pthread_t     s_thread {};
static volatile bool s_thread_stopping = false;

void p8audio_verilated_init(void)
{
    if (s_model_init)
        return;

    /* Create Verilated context and model */
    s_vl_ctx = new VerilatedContext;
    s_model  = new Vp8audio(s_vl_ctx, "p8audio");

    model_reset();
    s_model_init = true;

    /* Open SDL audio */
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq     = SAMPLE_RATE_HW;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = SDL_BUFFER_SAMPLES;
    want.callback = audio_callback_verilated;
    want.userdata = nullptr;

    int ret = SDL_OpenAudio(&want, &s_audio_spec);
    if (ret != 0) {
        fprintf(stderr, "[p8audio_verilated] SDL_OpenAudio failed: %s\n",
                SDL_GetError());
        int ret = pthread_create(&s_thread,
            nullptr,
            [](void*) -> void* {
                int16_t *buf = new int16_t[SDL_BUFFER_SAMPLES];
                while (!s_thread_stopping) {
                    audio_callback_verilated(nullptr, (uint8_t*)buf, sizeof(int16_t) * SDL_BUFFER_SAMPLES);
                    SDL_Delay(SDL_BUFFER_SAMPLES * 1000 / SAMPLE_RATE_HW);
                }
                delete[] buf;
                return nullptr;
            }, nullptr);
        if (ret != 0) {
            fprintf(stderr, "[p8audio_verilated] Failed to create fallback audio thread: %s\n",
                    SDL_GetError());
        }
        return;
    }

    SDL_PauseAudio(0);
}

void audio_resume(void)
{
    SDL_PauseAudio(0);
}

void audio_pause(void)
{
    SDL_PauseAudio(1);
}

void audio_close(void)
{
    if (s_thread != pthread_t{}) {
        s_thread_stopping = true;
        pthread_join(s_thread, nullptr);
    } else {
        SDL_CloseAudio();
    }
    if (s_model) {
        s_model->final();
        delete s_model;
        s_model = nullptr;
    }
    delete s_vl_ctx;
    s_vl_ctx  = nullptr;
    s_model_init = false;
}

/*==============================================================
 * Stat register read (CPU thread)
 * Returns the cached value for byte offsets 0x20..0x36 (even only).
 * Offsets outside this range return 0.
 *==============================================================*/
extern "C" uint16_t p8audio_verilated_mmio_read(uint8_t byte_offset)
{
    if (byte_offset < 0x20 || byte_offset > 0x36 || (byte_offset & 1u))
        return 0;
    int idx = (byte_offset - 0x20) / 2;
    pthread_mutex_lock(&s_stat_mutex);
    uint16_t val = s_stat_cache[idx];
    pthread_mutex_unlock(&s_stat_mutex);
    return val;
}
