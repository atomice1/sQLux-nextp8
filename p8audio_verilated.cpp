/*
 * p8audio_verilated.cpp
 *
 * Verilator-based replacement for p8_audio.c / p8_dsp.c.
 * Dual-model architecture: RT model for functional validation, PB model for audio playback.
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
#include <stdint.h>
#include <pthread.h>
#include <deque>
#include <algorithm>

extern "C" uint64_t GetUserTimer(void);

/* Mutex for PB command queue (CPU thread writes, audio thread reads) */
static pthread_mutex_t s_pb_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;

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
 * MMIO command for PB queue
 *==============================================================*/
struct mmio_cmd_t {
    uint8_t  byte_addr;
    uint16_t data;
};

/*==============================================================
 * Dual-model architecture
 *
 * RT model: Real-time model for functional validation (CPU thread)
 * PB model: Playback model for audio generation (audio thread)
 *
 * Communication:
 * - MMIO writes: queued to PB model via s_pb_cmd_queue, processed in audio callback
 * - RT model: advances on MMIO operations via time-based advancement
 * - PB model: advances in audio callback, driven by SDL audio demand
 *==============================================================*/

/* Two Verilated models */
static Vp8audio        *s_rt_model  = nullptr;  /* Real-time model (CPU thread) */
static Vp8audio        *s_pb_model  = nullptr;  /* Playback model (audio thread) */
static VerilatedContext *s_vl_ctx   = nullptr;

/* RT model time tracking (CPU thread only, no mutex needed) */
static uint64_t         s_rt_start_time = 0;
static uint64_t         s_rt_samples_advanced = 0;

/* PB command queue (CPU thread writes, audio thread reads) */
static std::deque<mmio_cmd_t> s_pb_cmd_queue;

/*==============================================================
 * Forward declarations
 *==============================================================*/
static inline void tick_mclk(Vp8audio *model, int level);
static inline void tick_8x(Vp8audio *model, int level);
static inline void tick_pcm(Vp8audio *model, int level);
static void service_dma(Vp8audio *model);
static void drain_pb_queue_to_model(void);
static void advance_rt_model_samples(int n);
static void perform_mmio_transaction(Vp8audio *model, uint8_t byte_addr, uint16_t data);
static void advance_one_sample_common(Vp8audio *model);
static uint16_t read_stat_register_direct(Vp8audio *model, uint8_t byte_offset);
static void audio_callback_verilated(void *userdata, uint8_t *stream, int len);

/*==============================================================
 * Helper functions for dual-model architecture
 *==============================================================*/

/* Calculate elapsed samples since last RT model operation */
static inline int elapsed_samples_since_last(uint64_t now)
{
    uint64_t elapsed_us = now - s_rt_start_time;

    /* 22050 Hz = ~45.3 µs per sample */
    uint64_t elapsed_samples = (int)(elapsed_us * 22050ULL / 1000000ULL);
    return elapsed_samples - s_rt_samples_advanced;
}

/* Advance RT model by elapsed time since last operation */
static inline void advance_rt_model_by_time(void)
{
    uint64_t now = GetUserTimer();
    uint64_t to_advance = elapsed_samples_since_last(now);
    if (to_advance > 0) {
        advance_rt_model_samples(to_advance);
        s_rt_samples_advanced += to_advance;
    }
}

/* Get current pcm_out from a model without advancement */
static inline int16_t get_model_sample(Vp8audio *model)
{
    // Sign-extend from PCM_WID bits to 16 bits
    int16_t val = (int16_t)model->pcm_out;
    int shift = 16 - PCM_WID;
    return (val << shift) >> shift;  // Arithmetic shift extends sign bit
}

/* Single MMIO write transaction on given model */
static void perform_mmio_transaction(Vp8audio *model, uint8_t byte_addr, uint16_t data)
{
    /* The 7-bit 'address' port is the byte-address shifted right by 1 (word address) */
    model->address  = byte_addr >> 1;
    model->din      = data;
    model->nUDS     = 0;
    model->nLDS     = 0;
    model->write_en = 1;
    model->read_en  = 0;

    tick_mclk(model, 0);   /* setup on falling edge */
    tick_mclk(model, 1);   /* latch on rising edge  */
    //service_dma(model);

    model->write_en = 0;
    model->nUDS     = 1;
    model->nLDS     = 1;

    tick_mclk(model, 0);
    tick_mclk(model, 1);
    //service_dma(model);
}

/* Advance model by exactly one PCM sample (common implementation) */
static void advance_one_sample_common(Vp8audio *model)
{
    for (int slot = 0; slot < PCM_8X_PER_PCM; slot++) {
        tick_8x(model, 1);
        tick_8x(model, 0);
        tick_mclk(model, 1);
        if (model->dma_req) {
            for (int i=0;i<200;++i) {
                //service_dma(model);
                tick_mclk(model, 0);
                tick_mclk(model, 1);
            }
            //service_dma(model);
        }
        tick_mclk(model, 0);
    }

    tick_pcm(model, 1);   /* clk_pcm rising – model outputs sample */
    tick_pcm(model, 0);
}

/* Drain PB command queue into PB model.
 * Must be called with s_pb_cmd_mutex locked.
 * The caller is responsible for locking/unlocking the mutex. */
static void drain_pb_queue_to_model(void)
{
    while (!s_pb_cmd_queue.empty()) {
        mmio_cmd_t cmd = s_pb_cmd_queue.front();
        s_pb_cmd_queue.pop_front();

        perform_mmio_transaction(s_pb_model, cmd.byte_addr, cmd.data);
    }
}

/* Advance RT model by N samples */
static void advance_rt_model_samples(int n)
{
    for (int i = 0; i < n; i++) {
        advance_one_sample_common(s_rt_model);
    }
}

/* Advance stat register directly from model */
static uint16_t read_stat_register_direct(Vp8audio *model, uint8_t byte_offset)
{
    if (byte_offset < 0x20 || byte_offset > 0x36 || (byte_offset & 1u))
        return 0;

    uint16_t value = 0;
    model->read_en  = 1;
    model->write_en = 0;

    uint8_t addr_offset = byte_offset >> 1;
    model->address = addr_offset;
    // First cycle: propagate address and read_en
    tick_mclk(model, 0);   /* setup on falling edge */
    tick_mclk(model, 1);   /* latch on rising edge  */
    // Second cycle: ensure dout is stable
    tick_mclk(model, 0);   /* setup on falling edge */
    tick_mclk(model, 1);   /* latch on rising edge  */
    value = model->dout;

    model->read_en = 0;

    // Cleanup cycle
    tick_mclk(model, 0);   /* setup on falling edge */
    tick_mclk(model, 1);   /* latch on rising edge  */

    return value;
}

/*==============================================================
 * Clock helpers (both models run in sync)
 *==============================================================*/

/* Single mclk half-cycle on given model */
static inline void tick_mclk(Vp8audio *model, int level)
{
    if (level == 0 && model->dma_req) {
        /* dma_addr is a 31-bit word address; each word is 16 bits, big-endian. */
        uint32_t byte_addr = (uint32_t)model->dma_addr * 2u;

        /* memBase is int32_t* so m_memory = (uint8_t*)memBase */
        uint16_t rdata = (uint16_t)(((uint16_t)m_memory[byte_addr] << 8) |
                                    (uint16_t)m_memory[byte_addr + 1u]);

        model->dma_rdata = rdata;
        model->dma_ack = 1;
    }
    model->mclk = level;
    model->eval();
    if (level == 1 && model->dma_ack)
        model->dma_ack = 0;
}

/* Single clk_pcm_8x half-cycle on given model */
static inline void tick_8x(Vp8audio *model, int level)
{
    model->clk_pcm_8x = level;
    model->eval();
}

/* Single clk_pcm half-cycle on given model */
static inline void tick_pcm(Vp8audio *model, int level)
{
    model->clk_pcm = level;
    model->eval();
}

#if 0
/* DMA service on given model - pulse dma_ack for one mclk cycle */
static void service_dma(Vp8audio *model)
{
    if (!model->dma_req)
        return;

    /* dma_addr is a 31-bit word address; each word is 16 bits, big-endian. */
    uint32_t byte_addr = (uint32_t)model->dma_addr * 2u;

    /* memBase is int32_t* so m_memory = (uint8_t*)memBase */
    uint16_t rdata = (uint16_t)(((uint16_t)m_memory[byte_addr] << 8) |
                                (uint16_t)m_memory[byte_addr + 1u]);

    model->dma_rdata = rdata;
    model->dma_ack = 1;
    tick_mclk(model, 0);   /* falling edge */
    tick_mclk(model, 1);   /* rising edge with ack=1 */
    model->dma_ack = 0;
}
#endif

/*==============================================================
 * Reset the Verilated models and hold for a few cycles.
 *==============================================================*/
static void model_reset(void)
{
    s_rt_model->resetn_sys   = 0;
    s_rt_model->resetn_pcm   = 0;
    s_rt_model->resetn_pcm_8x = 0;

    s_rt_model->mclk       = 0;
    s_rt_model->clk_pcm    = 0;
    s_rt_model->clk_pcm_8x = 0;

    s_rt_model->address  = 0;
    s_rt_model->din      = 0;
    s_rt_model->nUDS     = 1;
    s_rt_model->nLDS     = 1;
    s_rt_model->write_en = 0;
    s_rt_model->read_en  = 0;
    s_rt_model->dma_rdata = 0;
    s_rt_model->dma_ack   = 0;

    s_rt_model->eval();

    /* Drive reset for several cycles on each domain */
    for (int i = 0; i < 12; i++) { tick_mclk(s_rt_model, 1); tick_mclk(s_rt_model, 0); }
    for (int i = 0; i < 5;  i++) { tick_8x(s_rt_model, 1);   tick_8x(s_rt_model, 0);   }
    for (int i = 0; i < 3;  i++) { tick_pcm(s_rt_model, 1);  tick_pcm(s_rt_model, 0);  }

    s_rt_model->resetn_sys    = 1;
    s_rt_model->resetn_pcm    = 1;
    s_rt_model->resetn_pcm_8x = 1;
    s_rt_model->eval();

    /* Settle after reset deassertion */
    for (int i = 0; i < 4; i++) { tick_mclk(s_rt_model, 1); tick_mclk(s_rt_model, 0); }

        if (s_pb_model) {
        /* Reset PB model to match RT model */
        s_pb_model->resetn_sys   = 0;
        s_pb_model->resetn_pcm   = 0;
        s_pb_model->resetn_pcm_8x = 0;

        s_pb_model->mclk       = 0;
        s_pb_model->clk_pcm    = 0;
        s_pb_model->clk_pcm_8x = 0;

        s_pb_model->address  = 0;
        s_pb_model->din      = 0;
        s_pb_model->nUDS     = 1;
        s_pb_model->nLDS     = 1;
        s_pb_model->write_en = 0;
        s_pb_model->read_en  = 0;
        s_pb_model->dma_rdata = 0;
        s_pb_model->dma_ack   = 0;

        s_pb_model->eval();

        for (int i = 0; i < 12; i++) { tick_mclk(s_pb_model, 1); tick_mclk(s_pb_model, 0); }
        for (int i = 0; i < 5;  i++) { tick_8x(s_pb_model, 1);   tick_8x(s_pb_model, 0);   }
        for (int i = 0; i < 3;  i++) { tick_pcm(s_pb_model, 1);  tick_pcm(s_pb_model, 0);  }

        s_pb_model->resetn_sys    = 1;
        s_pb_model->resetn_pcm    = 1;
        s_pb_model->resetn_pcm_8x = 1;
        s_pb_model->eval();

        for (int i = 0; i < 4; i++) { tick_mclk(s_pb_model, 1); tick_mclk(s_pb_model, 0); }
    }

    /* Initialize RT model time tracking */
    s_rt_start_time = GetUserTimer();
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

    /* Create Verilated context and both models */
    s_vl_ctx = new VerilatedContext;
    s_rt_model = new Vp8audio(s_vl_ctx, "p8audio_rt");
    //s_pb_model = new Vp8audio(s_vl_ctx, "p8audio_pb");

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
        /* Create fallback thread that generates audio without SDL */
        ret = pthread_create(&s_thread,
            nullptr,
            [](void*) -> void* {
                int16_t *buf = new int16_t[SDL_BUFFER_SAMPLES];
                while (!s_thread_stopping) {
                    /* Generate dummy audio by advancing PB model */
                    audio_callback_verilated(nullptr, (uint8_t*)buf, sizeof(int16_t) * SDL_BUFFER_SAMPLES);
                    /* Sleep for the duration of the buffered samples */
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
    /* Stop fallback thread if running */
    if (s_thread != pthread_t{}) {
        s_thread_stopping = true;
        pthread_join(s_thread, nullptr);
        s_thread = pthread_t{};
    }

    /* Close SDL audio if it was opened */
    if (s_model_init) {
        SDL_CloseAudio();
    }

    if (s_rt_model) {
        s_rt_model->final();
        delete s_rt_model;
        s_rt_model = nullptr;
    }
    if (s_pb_model) {
        s_pb_model->final();
        delete s_pb_model;
        s_pb_model = nullptr;
    }
    delete s_vl_ctx;
    s_vl_ctx  = nullptr;
    s_model_init = false;
}

/*==============================================================
 * MMIO write (CPU thread)
 * Mirrors write to both RT and PB models:
 * - Queue write to PB model for audio callback processing
 * - Advance RT model and process write immediately
 *==============================================================*/

extern "C" void p8audio_verilated_mmio_write(uint8_t byte_addr, uint16_t data)
{
    /* Queue write for PB model first (audio thread reads, needs mutex) */
    if (s_pb_model) {
        pthread_mutex_lock(&s_pb_cmd_mutex);
        s_pb_cmd_queue.push_back({byte_addr, data});
        pthread_mutex_unlock(&s_pb_cmd_mutex);
    }

    /* Advance RT model by elapsed time (CPU thread, no mutex needed) */
    advance_rt_model_by_time();

    /* Process write immediately in RT model (real-time command processing) */
    perform_mmio_transaction(s_rt_model, byte_addr, data);
}

/*==============================================================
 * MMIO read (CPU thread)
 * Reads from RT model only (no caching needed)
 *==============================================================*/

extern "C" uint16_t p8audio_verilated_mmio_read(uint8_t byte_offset)
{
    if (byte_offset < 0x20 || byte_offset > 0x36 || (byte_offset & 1u))
        return 0;

    /* Advance RT model by elapsed time (CPU thread, no mutex needed) */
    advance_rt_model_by_time();

    /* Read stat register directly from RT model */
    uint16_t value = read_stat_register_direct(s_rt_model, byte_offset);

    return value;
}

/*==============================================================
 * PCM read functions (CPU thread)
 * Advance RT model by one sample and return PCM value
 *==============================================================*/

extern "C" int16_t p8audio_verilated_read_pcm_left(void)
{
    advance_rt_model_by_time();

    /* Get current pcm_out from RT model */
    int16_t s_pcm = get_model_sample(s_rt_model);
    return s_pcm << (16 - PCM_WID);
}

extern "C" int16_t p8audio_verilated_read_pcm_right(void)
{
    /* Right channel is the same as left for p8audio */
    return p8audio_verilated_read_pcm_left();
}

/*==============================================================
 * SDL audio callback (PB model driven by SDL audio demand)
 *==============================================================*/

static void audio_callback_verilated(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    int16_t *buf     = (int16_t *)stream;
    int      samples = len / (int)sizeof(int16_t);

    if (s_pb_model) {
        /* Drain PB queue into PB model (audio thread, mutex protects the queue) */
        pthread_mutex_lock(&s_pb_cmd_mutex);
        drain_pb_queue_to_model();
        pthread_mutex_unlock(&s_pb_cmd_mutex);
    }

    /* Advance PB model and get samples in one loop */
    for (int i = 0; i < samples; i++) {
        if (s_pb_model) {
            advance_one_sample_common(s_pb_model);
            int16_t s_pcm = get_model_sample(s_pb_model);
            buf[i] = s_pcm << (16 - PCM_WID);
        } else {
            buf[i] = 0;
        }
    }
}
