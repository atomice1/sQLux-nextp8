/*
 * FuncVal Testbench MMIO Support for sQLux-nextp8
 *
 * Implements a subset of the nextp8 FuncVal testbench peripheral MMIO
 * functionality, allowing test programs to run in software simulation
 * with similar behavior to hardware simulation.
 *
 * Address Range: 0x300000 - 0x56FFFF
 *   - 0x300000-0x37FFFF: Pin control/status (not implemented)
 *   - 0x380000-0x38FFFF: Peripheral control
 *     - 0x380001: Keyboard scancode (write to send PS/2 scancode)
 *     - 0x380021: Mouse buttons [2:0] = middle, right, left
 *     - 0x380022: Mouse X movement (signed 9-bit)
 *     - 0x380024: Mouse Y movement (signed 9-bit)
 *     - 0x380026: Mouse Z scroll (signed 4-bit) + triggers movement packet
 *     - 0x380041: Screenshot register (write to trigger PNG save)
 *     - 0x380043: Trace trigger register (write to set trace flag for logging)
 *     - 0x380045: WAV recording control (write 1 to start, 0 to stop recording)
 *     - 0x380061: Joystick 0 input (bits for directions/buttons)
 *     - 0x380063: Joystick 1 input (bits for directions/buttons)
 *   - 0x390000-0x392000: VGA framebuffer readback (128x128x16-bit 0RGB, 1/6 scale downsampled)
 */

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "QL68000.h"
#include "funcval_testbench.h"
#include "SDL2screen.h"

/* External asyncTrace flag for test logging */
extern bool asyncTrace;

/* Screenshot counter for generating unique filenames */
static int screenshot_counter = 0;

/* WAV recording state */
static int wav_recording = 0;
static FILE *wav_file = NULL;
static int wav_sample_count = 0;
static int wav_counter = 0;

/* Audio callback shim state */
static SDL_AudioCallback p8audio_callback = NULL;
static void *p8audio_userdata = NULL;
static SDL_AudioSpec p8audio_spec;
static int p8audio_shim_active = 0;
static double p8audio_src_pos = 0.0;

/* QL_sound.c audio device state (uses SDL_OpenAudioDevice) */
static SDL_AudioCallback da_callback = NULL;
static void *da_userdata = NULL;
static SDL_AudioSpec da_spec;
static int da_shim_active = 0;
static double da_src_pos = 0.0;

/* Keyboard registers (0x380001) */
#define FUNCVAL_KB_SCANCODE      0x380001

/* Mouse registers (0x380020-0x380026) */
#define FUNCVAL_MOUSE_BUTTONS    0x380021
#define FUNCVAL_MOUSE_X          0x380022
#define FUNCVAL_MOUSE_Y          0x380024
#define FUNCVAL_MOUSE_Z          0x380026

/* Screenshot register (0x380041) - write to trigger screenshot */
#define FUNCVAL_SCREENSHOT_REG   0x380041

/* asyncTrace register (0x380043) - write to set asyncTrace flag */
#define FUNCVAL_ASYNCTRACE_REG   0x380043

/* WAV recording register (0x380045) - write to start/stop recording */
#define FUNCVAL_WAV_REC_REG   	 0x380045

/* Joystick registers (0x380061-0x380063) */
#define FUNCVAL_JOY0             0x380061
#define FUNCVAL_JOY1             0x380063

/* Built-in keyboard matrix registers (0x380080-0x380087) - write only */
#define FUNCVAL_KB_MATRIX_BASE   0x380080
#define FUNCVAL_KB_MATRIX_MAX    0x380087

/* Built-in keyboard matrix bit-to-scancode mappings from mkeyboard.v
 * Each row has a set of bits that map to specific PS/2 scancodes
 * Rows are scanned via keyb_row_o (one-hot), columns read via keyb_col_i
 */

/* Row 0: Caps Shift at [0], Z at [1], X at [2], C at [3], V at [4], Up at [6] */
static const uint8_t keyboard_matrix_row0_scancode[] = {
    0x12,  /* [0] - 0x12: Caps Shift */
    0x1A,  /* [1] - 0x1A: Z */
    0x22,  /* [2] - 0x22: X */
    0x21,  /* [3] - 0x21: C */
    0x2A,  /* [4] - 0x2A: V */
    0xFF,  /* [5] - reserved */
    0xF5   /* [6] - 0xF5: Up */
};

/* Row 1: A at [0], S at [1], D at [2], F at [3], G at [4], Caps Lock at [5] */
static const uint8_t keyboard_matrix_row1_scancode[] = {
    0x1C,  /* [0] - 0x1C: A */
    0x1B,  /* [1] - 0x1B: S */
    0x23,  /* [2] - 0x23: D */
    0x2B,  /* [3] - 0x2B: F */
    0x34,  /* [4] - 0x34: G */
    0x58,  /* [5] - 0x58: Caps Lock */
    0xFF   /* [6] - reserved */
};

/* Row 2: Q at [0], W at [1], E at [2], R at [3], T at [4] */
static const uint8_t keyboard_matrix_row2_scancode[] = {
    0x15,  /* [0] - 0x15: Q */
    0x1D,  /* [1] - 0x1D: W */
    0x24,  /* [2] - 0x24: E */
    0x2D,  /* [3] - 0x2D: R */
    0x2C,  /* [4] - 0x2C: T */
    0xFF,  /* [5] - reserved */
    0xFF   /* [6] - reserved */
};

/* Row 3: 1 at [0], 2 at [1], 3 at [2], 4 at [3], 5 at [4], Break/ESC at [5] */
static const uint8_t keyboard_matrix_row3_scancode[] = {
    0x16,  /* [0] - 0x16: 1 */
    0x1E,  /* [1] - 0x1E: 2 */
    0x26,  /* [2] - 0x26: 3 */
    0x25,  /* [3] - 0x25: 4 */
    0x2E,  /* [4] - 0x2E: 5 */
    0x76,  /* [5] - 0x76: Break (ESC) */
    0xFF   /* [6] - reserved */
};

/* Row 4: 0 at [0], 9 at [1], 8 at [2], 7 at [3], 6 at [4], ; at [5], " at [6] */
static const uint8_t keyboard_matrix_row4_scancode[] = {
    0x45,  /* [0] - 0x45: 0 */
    0x46,  /* [1] - 0x46: 9 */
    0x3E,  /* [2] - 0x3E: 8 */
    0x3D,  /* [3] - 0x3D: 7 */
    0x36,  /* [4] - 0x36: 6 */
    0x4C,  /* [5] - 0x4C: ; */
    0x52   /* [6] - 0x52: " */
};

/* Row 5: P at [0], O at [1], I at [2], U at [3], Y at [4], , at [5], . at [6] */
static const uint8_t keyboard_matrix_row5_scancode[] = {
    0x4D,  /* [0] - 0x4D: P */
    0x44,  /* [1] - 0x44: O */
    0x43,  /* [2] - 0x43: I */
    0x3C,  /* [3] - 0x3C: U */
    0x35,  /* [4] - 0x35: Y */
    0x41,  /* [5] - 0x41: , */
    0x49   /* [6] - 0x49: . */
};

/* Row 6: Enter at [0], L at [1], K at [2], J at [3], H at [4], Delete at [5], Right at [6] */
static const uint8_t keyboard_matrix_row6_scancode[] = {
    0x5A,  /* [0] - 0x5A: Enter */
    0x4B,  /* [1] - 0x4B: L */
    0x42,  /* [2] - 0x42: K */
    0x3B,  /* [3] - 0x3B: J */
    0x33,  /* [4] - 0x33: H */
    0x71,  /* [5] - 0x71: Delete */
    0xF4   /* [6] - 0xF4: Right */
};

/* Row 7: Space at [0], Sym Shift/LAlt at [1], M at [2], N at [3], B at [4], Left at [5], Down at [6] */
static const uint8_t keyboard_matrix_row7_scancode[] = {
    0x29,  /* [0] - 0x29: Space */
    0x11,  /* [1] - 0x11: Sym Shift (LAlt) */
    0x3A,  /* [2] - 0x3A: M */
    0x31,  /* [3] - 0x31: N */
    0x32,  /* [4] - 0x32: B */
    0xEB,  /* [5] - 0xEB: Left */
    0xF2   /* [6] - 0xF2: Down */
};

/* Array of pointers to each row's scancode mappings */
static const uint8_t *keyboard_matrix_scancodes[] = {
    keyboard_matrix_row0_scancode,
    keyboard_matrix_row1_scancode,
    keyboard_matrix_row2_scancode,
    keyboard_matrix_row3_scancode,
    keyboard_matrix_row4_scancode,
    keyboard_matrix_row5_scancode,
    keyboard_matrix_row6_scancode,
    keyboard_matrix_row7_scancode
};

/* Mouse state */
static uint8_t mouse_buttons = 0x00;  /* [2:0] = middle, right, left */
static int16_t mouse_x = 0;

static int16_t mouse_y = 0;
static int8_t mouse_z = 0;

/* PS/2 scancode state machine */
static uint8_t ps2_state = 0;  /* 0 = normal, 1 = saw 0xF0 (break), 2 = saw 0xE0 (extended) */

/* VGA framebuffer range: 0x390000 - 0x398000 (128x128x2 bytes = 32,768 bytes)
 * Each pixel represents 1/6 scale: samples at (x*6+3+130, y*6+3) from VGA buffer */
#define FUNCVAL_VGA_FB_START    0x390000
#define FUNCVAL_VGA_FB_END      0x398000

/* Process PS/2 scancode byte - handles Set 2 protocol */
static void funcval_latch_scancode(uint8_t data)
{
	if (data == 0xF0) {
		/* Break prefix - next byte is key release */
		ps2_state = 1;
		return;
	} else if (data == 0xE0) {
		/* Extended scancode prefix */
		ps2_state = 2;
		return;
	}

	/* Process scancode based on state */
	int press = (ps2_state != 1);  /* 1 = press, 0 = release */
	int code = data;

	/* For extended scancodes, set bit 7 */
	if (ps2_state == 2) {
		code |= 0x80;
	}

	/* Update keyboard matrix */
	SDLQLKeyrowChg(code, press);

	/* Reset state machine */
	ps2_state = 0;

	if (asyncTrace) {
		printf("FuncVal PS/2: scancode 0x%02X -> code 0x%02X, %s\n",
		       data, code, press ? "press" : "release");
	}
}

/* Save screenshot to PPM file */
static void funcval_save_screenshot(void)
{
	char filename[256];

	snprintf(filename, sizeof(filename), "screenshot_%04d.ppm", screenshot_counter++);
	QLSDLSaveFuncvalScreenshot(filename);
}

/* WAV file header writing (mono, 16-bit, 22050 Hz) */
static void wav_write_header(FILE *f, int n_samples)
{
	if (!f) return;

	int bytes = n_samples * 2;  // 16-bit mono
	int br = 22050 * 2;  // bytes/sec

	// RIFF header
	fwrite("RIFF", 1, 4, f);
	uint32_t chunk_size = bytes + 36;
	fwrite(&chunk_size, 4, 1, f);  // Little-endian
	fwrite("WAVEfmt ", 1, 8, f);

	uint32_t fmt_chunk_size = 16;
	fwrite(&fmt_chunk_size, 4, 1, f);

	uint16_t format = 1;  // PCM
	fwrite(&format, 2, 1, f);

	uint16_t channels = 1;
	fwrite(&channels, 2, 1, f);

	uint32_t sample_rate = 22050;
	fwrite(&sample_rate, 4, 1, f);

	uint32_t byte_rate = br;
	fwrite(&byte_rate, 4, 1, f);

	uint16_t block_align = 2;
	fwrite(&block_align, 2, 1, f);

	uint16_t bits_per_sample = 16;
	fwrite(&bits_per_sample, 2, 1, f);

	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
}

/* Start WAV recording */
static void funcval_wav_start_recording(void)
{
	char filename[256];

	if (wav_file) {
		// Already recording, close previous file
		fclose(wav_file);
	}

	snprintf(filename, sizeof(filename), "funcval_audio_%04d.wav", wav_counter++);
	wav_file = fopen(filename, "wb");

	if (!wav_file) {
		printf("ERROR: Could not open %s for WAV recording\n", filename);
		wav_recording = 0;
		return;
	}

	// Write WAV header with size=0 (will be updated on close)
	wav_write_header(wav_file, 0);
	wav_sample_count = 0;
	wav_recording = 1;

	printf("Started WAV recording to %s\n", filename);
}

/* Stop WAV recording */
static void funcval_wav_stop_recording(void)
{
	if (!wav_file) {
		wav_recording = 0;
		return;
	}

	// Rewind and rewrite header with actual sample count
	fseek(wav_file, 0, SEEK_SET);
	wav_write_header(wav_file, wav_sample_count);
	fclose(wav_file);

	printf("Stopped WAV recording: %d samples written\n", wav_sample_count);

	wav_file = NULL;
	wav_recording = 0;
	wav_sample_count = 0;
}

static void funcval_capture_audio(const SDL_AudioSpec *spec, Uint8 *stream, int len, double *src_pos)
{
	if (!wav_recording || !wav_file || !spec || spec->format == 0) {
		return;
	}

	int samples = len / ((spec->format & 0xFF) / 8);  // Calculate number of samples

	if (spec->format == AUDIO_S16SYS && spec->freq == 44100) {
		/* 44100 Hz S16 -> 22050 Hz S16: Simple 2:1 downsample */
		int16_t *s16_stream = (int16_t *)stream;
		for (int i = 0; i < samples; i += 2) {
			if (i + 1 < samples) {
				int16_t sample = ((int32_t)s16_stream[i] + (int32_t)s16_stream[i + 1]) / 2;
				fwrite(&sample, 2, 1, wav_file);
				wav_sample_count++;
			}
		}
	} else if (spec->format == AUDIO_S16SYS && spec->freq == 22050) {
		/* 22050 Hz S16 -> 22050 Hz S16: Direct copy */
		fwrite(stream, len, 1, wav_file);
		wav_sample_count += samples;
	} else if (spec->format == AUDIO_S8 && spec->freq == 24000) {
		/* 24000 Hz S8 -> 22050 Hz S16: Resample and convert */
		int8_t *s8_stream = (int8_t *)stream;
		double src_rate = 24000.0;
		double dst_rate = 22050.0;
		int output_samples = (int)(samples * dst_rate / src_rate);

		for (int i = 0; i < output_samples; i++) {
			int src_idx = (int)(*src_pos);
			if (src_idx >= samples) break;

			int16_t sample = ((int16_t)s8_stream[src_idx]) << 8;
			fwrite(&sample, 2, 1, wav_file);
			wav_sample_count++;

			*src_pos += src_rate / dst_rate;
		}
		*src_pos -= (int)(*src_pos);
	} else if (spec->format == AUDIO_S16SYS && spec->freq == 5513) {
		/* 5513 Hz S16 -> 22050 Hz S16: Upsample by 4 */
		int16_t *s16_stream = (int16_t *)stream;
		for (int i = 0; i < samples; i++) {
			for (int j = 0; j < 4; j++) {
				fwrite(&s16_stream[i], 2, 1, wav_file);
				wav_sample_count++;
			}
		}
	} else {
		/* Fallback: write as-is (may be wrong format/rate) */
		if (spec->format == AUDIO_S16SYS) {
			fwrite(stream, len, 1, wav_file);
			wav_sample_count += samples;
		} else if (spec->format == AUDIO_S8) {
			int8_t *s8_stream = (int8_t *)stream;
			for (int i = 0; i < samples; i++) {
				int16_t sample = ((int16_t)s8_stream[i]) << 8;
				fwrite(&sample, 2, 1, wav_file);
				wav_sample_count++;
			}
		}
	}
}

/* Audio callback wrapper for WAV recording */
static void funcval_p8audio_callback_wrapper(void *userdata, Uint8 *stream, int len)
{
	/* Call the original callback first */
	if (p8audio_callback) {
		p8audio_callback(p8audio_userdata, stream, len);
	}

	funcval_capture_audio(&p8audio_spec, stream, len, &p8audio_src_pos);
}

/* QL_sound.c audio callback wrapper */
static void funcval_da_audio_callback_wrapper(void *userdata, Uint8 *stream, int len)
{
	/* Call the original callback first */
	if (da_callback) {
		da_callback(da_userdata, stream, len);
	}

	funcval_capture_audio(&da_spec, stream, len, &da_src_pos);
}

/* SDL_OpenAudio shim for intercepting audio callbacks (p8_audio.c) */
int SDL_OpenAudio_shim(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
	/* Store the original callback and userdata */
	p8audio_callback = desired->callback;
	p8audio_userdata = desired->userdata;

	/* Replace with our wrapper */
	desired->callback = funcval_p8audio_callback_wrapper;
	desired->userdata = NULL;

	/* Call the real SDL_OpenAudio (no define applies here, so this is the real function) */
	int result = SDL_OpenAudio(desired, obtained);

	if (result == 0) {
		/* Store the spec for format conversion */
		if (obtained) {
			p8audio_spec = *obtained;
		} else {
			p8audio_spec = *desired;
		}
		p8audio_shim_active = 1;
		printf("FuncVal audio shim: Intercepted SDL_OpenAudio (freq=%d, format=0x%x, channels=%d, samples=%d)\n",
		       p8audio_spec.freq, p8audio_spec.format, p8audio_spec.channels, p8audio_spec.samples);
	}

	return result;
}

/* SDL_OpenAudioDevice shim for intercepting audio callbacks (QL_sound.c) */
SDL_AudioDeviceID SDL_OpenAudioDevice_shim(const char *device, int iscapture,
                                            const SDL_AudioSpec *desired,
                                            SDL_AudioSpec *obtained,
                                            int allowed_changes)
{
	/* Make a copy of desired spec since we need to modify it */
	SDL_AudioSpec modified_spec;
	if (desired) {
		modified_spec = *desired;

		/* Store the original callback and userdata */
		da_callback = modified_spec.callback;
		da_userdata = modified_spec.userdata;

		/* Replace with our wrapper */
		modified_spec.callback = funcval_da_audio_callback_wrapper;
		modified_spec.userdata = NULL;
	}

	/* Call the real SDL_OpenAudioDevice */
	SDL_AudioDeviceID result = SDL_OpenAudioDevice(device, iscapture,
	                                               desired ? &modified_spec : NULL,
	                                               obtained,
	                                               allowed_changes);

	if (result != 0) {
		/* Store the spec for format conversion */
		if (obtained) {
			da_spec = *obtained;
		} else if (desired) {
			da_spec = modified_spec;
		}
		da_shim_active = 1;
		printf("FuncVal audio shim: Intercepted SDL_OpenAudioDevice (freq=%d, format=0x%x, channels=%d, samples=%d)\n",
		       da_spec.freq, da_spec.format, da_spec.channels, da_spec.samples);
	}

	return result;
}

/* Initialize FuncVal testbench */
void funcval_init(void)
{
	/* Nothing to initialize - we read directly from SDL window */
}

/* Check if address is in FuncVal testbench range */
int funcval_is_testbench_addr(aw32 addr)
{
	return (addr >= 0x300000 && addr < 0x400000);
}

/* Map downsampled VGA framebuffer address to VGA screen coordinates */
static int funcval_addr_to_coords(aw32 addr, int *vga_x, int *vga_y)
{
	/* Check if in VGA framebuffer range */
	if (addr < FUNCVAL_VGA_FB_START || addr >= FUNCVAL_VGA_FB_END)
		return -1;

	/* Calculate pixel index in 128x128 downsampled buffer (2 bytes per pixel) */
	aw32 offset = addr - FUNCVAL_VGA_FB_START;
	aw32 pixel_index = offset / 2;

	/* Convert to x, y in downsampled space (128x128) */
	int downsamp_y = pixel_index / 128;
	int downsamp_x = pixel_index % 128;

	/* Map to VGA coordinates: each downsampled pixel represents 1/6 scale */
	/* Sample at center of 6x6 block: (x*6+3+130, y*6+3) */
	*vga_x = downsamp_x * 6 + 3 + 130;
	*vga_y = downsamp_y * 6 + 3;

	printf("Mapping FuncVal address 0x%06X (offset 0x%06X) to downsampled space (%d, %d), VGA coordinates (%d, %d)\n", addr, offset, downsamp_x, downsamp_y, *vga_x, *vga_y);

	/* Check downsampled bounds */
	if (downsamp_x >= 128 || downsamp_y >= 128)
		return -1;

	return 0;
}

/* Read byte from testbench */
rw8 funcval_read_byte(aw32 addr)
{
	int x, y;
	uint16_t pixel;

	/* VGA framebuffer readback */
	if (funcval_addr_to_coords(addr, &x, &y) == 0) {
		if (QLSDLReadFramebufferPixel(x, y, &pixel) == 0) {
			/* Return high or low byte depending on address alignment */
			if (addr & 1)
				return pixel & 0xFF;  /* Low byte */
			else
				return (pixel >> 8) & 0xFF;  /* High byte */
		}
		return 0xFF;
	}

	/* Unimplemented region - return 0xFF */
	return 0xFF;
}

/* Read word from testbench (big-endian) */
rw16 funcval_read_word(aw32 addr)
{
	int x, y;
	uint16_t pixel;

	/* VGA framebuffer readback */
	if (funcval_addr_to_coords(addr, &x, &y) == 0) {
		if (QLSDLReadFramebufferPixel(x, y, &pixel) == 0) {
			return pixel;  /* Already in 0RGB format */
		}
		return 0xFFFF;
	}

	/* Unimplemented region - return 0xFFFF */
	return 0xFFFF;
}

/* Read long from testbench (big-endian) */
rw32 funcval_read_long(aw32 addr)
{
	int x, y;
	uint16_t pixel1, pixel2;

	/* VGA framebuffer readback - read two pixels */
	if (funcval_addr_to_coords(addr, &x, &y) == 0) {
		if (QLSDLReadFramebufferPixel(x, y, &pixel1) == 0 &&
		    QLSDLReadFramebufferPixel(x + 1, y, &pixel2) == 0) {
			return ((aw32)pixel1 << 16) | pixel2;
		}
		return 0xFFFFFFFF;
	}

	/* Unimplemented region - return 0xFFFFFFFF */
	return 0xFFFFFFFF;
}

/* Write byte to testbench */
void funcval_write_byte(aw32 addr, aw8 data)
{
	/* Keyboard scancode - trigger send */
	if (addr == FUNCVAL_KB_SCANCODE) {
		funcval_latch_scancode(data);
		return;
	}

	/* Mouse button state */
	if (addr == FUNCVAL_MOUSE_BUTTONS) {
		sdl_mouse_buttons = data & 0x1F;
		sdl_mouse_buttons_latched |= data & 0x1F;
		return;
	}

	/* Mouse Z scroll - accumulate signed increment */
	if (addr == FUNCVAL_MOUSE_Z) {
		sdl_mouse_z_accum += (int16_t)(int8_t)data;
		return;
	}

	/* Screenshot register - trigger screenshot on any write */
	if (addr == FUNCVAL_SCREENSHOT_REG) {
		funcval_save_screenshot();
		return;
	}

	/* asyncTrace register - set flag on any non-zero write */
	if (addr == FUNCVAL_ASYNCTRACE_REG) {
		asyncTrace = (data != 0);
		return;
	}

	/* WAV recording register - start/stop recording */
	if (addr == FUNCVAL_WAV_REC_REG) {
		if (data && !wav_recording) {
			funcval_wav_start_recording();
		} else if (!data && wav_recording) {
			funcval_wav_stop_recording();
		}
		return;
	}

	/* Joystick registers */
	if (addr == FUNCVAL_JOY0) {
		joy_state[0] = data & 0xff;
		joy_latched[0] |= data & 0xff;
	}

	if (addr == FUNCVAL_JOY1) {
		joy_state[1] = data & 0xff;
		joy_latched[1] |= data & 0xff;
	}

	/* Built-in keyboard matrix writes (0x380080-0x380087)
	 * Each write contains an 8-bit value where each bit controls one key
	 * in that row. Bits are mapped to PS/2 scancodes via the
	 * keyboard_matrix_scancodes array.
	 */
	if (addr >= FUNCVAL_KB_MATRIX_BASE && addr <= FUNCVAL_KB_MATRIX_MAX) {
		int row = addr - FUNCVAL_KB_MATRIX_BASE;
		const uint8_t *row_scancodes = keyboard_matrix_scancodes[row];

		/* Iterate through each bit in this row */
		for (int bit = 0; bit < 7; bit++) {
			uint8_t scancode = row_scancodes[bit];

			/* Skip reserved scancode values */
			if (scancode == 0xFF)
				continue;

			/* Extract bit value from data and determine press/release */
			int bit_set = (data >> bit) & 1;

			/* Send press or release based on bit value */
			SDLQLKeyrowChg(scancode, bit_set ? 1 : 0);

			if (asyncTrace) {
				printf("FuncVal KB Matrix: row %d, bit %d, scancode 0x%02X -> code 0x%02X, %s\n",
				       row, bit, scancode, code, bit_set ? "press" : "release");
			}
		}
		return;
	}

	/* VGA framebuffer is read-only */
	/* Writes to unimplemented regions are ignored */
}

/* Write word to testbench (big-endian) */
void funcval_write_word(aw32 addr, aw16 data)
{
	/* Mouse button state */
	if (addr == FUNCVAL_MOUSE_BUTTONS) {
		sdl_mouse_buttons = data & 0x1F;
		sdl_mouse_buttons_latched |= data & 0x1F;
		return;
	}

	/* Mouse X movement - accumulate signed increment */
	if (addr == FUNCVAL_MOUSE_X) {
		sdl_mouse_x_accum += (int16_t)data;
		return;
	}

	/* Mouse Y movement - accumulate signed increment */
	if (addr == FUNCVAL_MOUSE_Y) {
		sdl_mouse_y_accum += (int16_t)data;
		return;
	}

	/* Mouse Z scroll - accumulate signed increment */
	if (addr == FUNCVAL_MOUSE_Z) {
		sdl_mouse_z_accum += (int16_t)data;
		return;
	}


	/* Writes to unimplemented regions are ignored */
}

/* Write long to testbench (big-endian) */
void funcval_write_long(aw32 addr, aw32 data)
{
	/* Writes to unimplemented regions are ignored */
}
