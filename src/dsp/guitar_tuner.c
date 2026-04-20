/*
 * Guitar Tuner DSP plugin
 *
 * Reads audio from Move's hardware input buffer (mic or 3.5mm line-in) and
 * runs continuous pitch detection. Publishes the detected frequency, the
 * nearest-note cents deviation, and a rolling peak level via get_param so
 * the JS UI can render the note readout without touching audio buffers.
 *
 * Algorithm: Cumulative-Mean-Normalized Difference Function (the core of
 * YIN, de Cheveigne & Kawahara 2002), evaluated over a ~2048-sample window.
 * This is cheap (O(N*tau_max), tau_max ~630 for 70Hz floor at 44.1k) and
 * robust to octave errors at instrument-playing levels.
 *
 * The plugin produces no audio — render_block always writes silence — but
 * it still runs every block so we can peek at the mic input in lockstep
 * with the SPI frame clock.
 *
 * V2 API (single instance per load, but v2 is the current standard).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"

/* ----------------------------------------------------------------
 * Detection parameters
 * ---------------------------------------------------------------- */

/* Analysis window length. Must fit below guitar low-E (82 Hz, period ~537
 * samples at 44.1k) plus headroom for the cumulative-mean normalization.
 * 2048 samples = ~46 ms of audio, comfortable for 70–1200 Hz range. */
#define TUNER_WINDOW 2048

/* Lag bounds (samples). Frequencies below TAU_MIN or above TAU_MAX are
 * rejected — they're almost always noise or harmonics of the real pitch.
 * TAU_MIN=37 → 44100/37 ≈ 1192 Hz (above high-E string harmonics).
 * TAU_MAX=630 → 44100/630 ≈ 70 Hz (well below low-E at 82 Hz). */
#define TUNER_TAU_MIN 37
#define TUNER_TAU_MAX 630

/* YIN threshold for accepting the first local minimum of the CMNDF. Values
 * below ~0.10 are considered a confident pitch; higher thresholds accept
 * noisier input at the risk of octave errors. 0.15 is the classic default. */
#define TUNER_YIN_THRESHOLD 0.15f

/* Noise gate — refuse to report a pitch unless the RMS of the window
 * exceeds this (int16 units). Keeps stale readings from showing up during
 * silence. */
#define TUNER_MIN_RMS 120.0f

/* ----------------------------------------------------------------
 * Instance state
 * ---------------------------------------------------------------- */

typedef struct {
    /* Rolling input buffer — mono sum of L+R from Move Input. New blocks
     * are appended on the right; analysis reads the most recent
     * TUNER_WINDOW samples. */
    float ring[TUNER_WINDOW];
    int   ring_write;  /* next write position, wraps at TUNER_WINDOW */
    int   samples_since_analysis;

    /* Latest analysis results. Updated every ~4 blocks so pitch detection
     * doesn't run every 128 samples (overkill and CPU-wasteful). */
    float last_freq_hz;   /* 0 if unvoiced */
    float last_cents_off; /* signed cents from nearest note */
    int   last_midi_note; /* nearest MIDI note number, or -1 if unvoiced */
    float last_confidence;/* 1 - yin_value at accepted tau */
    int   last_peak;      /* absolute peak in the most recent window (int16) */
    float last_rms;       /* RMS of the most recent window */

    /* Scratch buffers for YIN. Kept on the instance so we don't stack-
     * allocate 8KB every analysis (important on the realtime audio
     * thread). */
    float yin_diff[TUNER_TAU_MAX + 1];
    float yin_cmnd[TUNER_TAU_MAX + 1];
} guitar_tuner_t;

static const host_api_v1_t *g_host = NULL;

static void st_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

/* ----------------------------------------------------------------
 * Pitch detection (CMNDF / YIN)
 * ---------------------------------------------------------------- */

/* Fill yin_diff[tau] = sum over i of (buf[i] - buf[i+tau])^2 for tau in
 * [1, TAU_MAX]. The "difference function" from YIN step 1. */
static void yin_difference(const float *buf, guitar_tuner_t *st) {
    const int W = TUNER_WINDOW;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) {
        float sum = 0.0f;
        /* Classic YIN uses N/2 pairs; we use a similar window so low-freq
         * lags still have enough samples to be meaningful. */
        const int n = W - tau;
        for (int i = 0; i < n; i++) {
            float d = buf[i] - buf[i + tau];
            sum += d * d;
        }
        st->yin_diff[tau] = sum;
    }
}

/* Cumulative-mean-normalized difference: yin_cmnd[tau] = diff[tau] /
 * ((1/tau) * sum(diff[1..tau])). Rescales so the function starts at 1
 * and dips toward 0 at the true period. */
static void yin_cmnd(guitar_tuner_t *st) {
    st->yin_cmnd[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) {
        running += st->yin_diff[tau];
        if (running <= 0.0f) {
            st->yin_cmnd[tau] = 1.0f;
        } else {
            st->yin_cmnd[tau] = st->yin_diff[tau] * tau / running;
        }
    }
}

/* Absolute-threshold step: walk from TAU_MIN upward, find the first dip
 * below THRESHOLD, then descend to the local minimum. Returns the tau
 * of that minimum, or 0 if no dip found (unvoiced). */
static int yin_absolute_threshold(const guitar_tuner_t *st) {
    for (int tau = TUNER_TAU_MIN; tau <= TUNER_TAU_MAX; tau++) {
        if (st->yin_cmnd[tau] < TUNER_YIN_THRESHOLD) {
            /* Descend to local minimum. */
            while (tau + 1 <= TUNER_TAU_MAX &&
                   st->yin_cmnd[tau + 1] < st->yin_cmnd[tau]) {
                tau++;
            }
            return tau;
        }
    }
    return 0;
}

/* Parabolic interpolation around tau_est to get sub-sample accuracy.
 * Returns the refined (fractional) tau. */
static float yin_parabolic_refine(const guitar_tuner_t *st, int tau) {
    if (tau <= 1 || tau >= TUNER_TAU_MAX) return (float)tau;
    float s0 = st->yin_cmnd[tau - 1];
    float s1 = st->yin_cmnd[tau];
    float s2 = st->yin_cmnd[tau + 1];
    float denom = (s0 + s2 - 2.0f * s1);
    if (denom == 0.0f) return (float)tau;
    float delta = 0.5f * (s0 - s2) / denom;
    return (float)tau + delta;
}

/* Map a frequency to the nearest MIDI note + signed cents deviation.
 * MIDI note 69 = A4 = 440 Hz. */
static void freq_to_note(float freq, int *midi_note, float *cents_off) {
    if (freq <= 0.0f) {
        *midi_note = -1;
        *cents_off = 0.0f;
        return;
    }
    float semitones_from_a4 = 12.0f * log2f(freq / 440.0f);
    float midi_f = 69.0f + semitones_from_a4;
    int nearest = (int)floorf(midi_f + 0.5f);
    if (nearest < 0) nearest = 0;
    if (nearest > 127) nearest = 127;
    *midi_note = nearest;
    *cents_off = (midi_f - (float)nearest) * 100.0f;
}

/* ----------------------------------------------------------------
 * Audio capture + analysis
 * ---------------------------------------------------------------- */

static void capture_block(guitar_tuner_t *st) {
    if (!g_host || !g_host->mapped_memory) return;
    const int16_t *audio =
        (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    /* Move delivers interleaved stereo int16 at 44.1k. Collapse to mono
     * (L+R)/2 in [-1, 1] so the analysis window is a single float stream. */
    for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
        int32_t sum = (int32_t)audio[i * 2] + (int32_t)audio[i * 2 + 1];
        float f = (float)sum / 65536.0f;  /* /2 then /32768 in one shot */
        st->ring[st->ring_write] = f;
        st->ring_write = (st->ring_write + 1) % TUNER_WINDOW;
    }
    st->samples_since_analysis += MOVE_FRAMES_PER_BLOCK;
}

/* Copy the ring into a contiguous linear buffer (oldest → newest) so the
 * difference function can index without wrap-math. TUNER_WINDOW floats
 * is only 8KB, cheap on the stack. */
static void linearize(const guitar_tuner_t *st, float *out) {
    const int W = TUNER_WINDOW;
    int head = st->ring_write;  /* next-to-write = oldest sample */
    for (int i = 0; i < W; i++) {
        out[i] = st->ring[(head + i) % W];
    }
}

static void analyze(guitar_tuner_t *st) {
    float linear[TUNER_WINDOW];
    linearize(st, linear);

    /* Peak + RMS over the window. Both feed UX: peak drives the VU meter,
     * RMS is the noise gate. */
    float peak = 0.0f;
    double sumsq = 0.0;
    for (int i = 0; i < TUNER_WINDOW; i++) {
        float a = fabsf(linear[i]);
        if (a > peak) peak = a;
        sumsq += (double)linear[i] * (double)linear[i];
    }
    st->last_peak = (int)(peak * 32767.0f);
    st->last_rms = sqrtf((float)(sumsq / TUNER_WINDOW)) * 32767.0f;

    if (st->last_rms < TUNER_MIN_RMS) {
        /* Below the gate — mark unvoiced but don't reset so the UI can
         * show the last valid reading fading rather than flicker. */
        st->last_freq_hz = 0.0f;
        st->last_confidence = 0.0f;
        st->last_midi_note = -1;
        st->last_cents_off = 0.0f;
        return;
    }

    yin_difference(linear, st);
    yin_cmnd(st);
    int tau = yin_absolute_threshold(st);
    if (tau == 0) {
        st->last_freq_hz = 0.0f;
        st->last_confidence = 0.0f;
        st->last_midi_note = -1;
        st->last_cents_off = 0.0f;
        return;
    }
    float tau_refined = yin_parabolic_refine(st, tau);
    float freq = (float)MOVE_SAMPLE_RATE / tau_refined;
    int midi_note;
    float cents;
    freq_to_note(freq, &midi_note, &cents);

    st->last_freq_hz   = freq;
    st->last_midi_note = midi_note;
    st->last_cents_off = cents;
    st->last_confidence = 1.0f - st->yin_cmnd[tau];
}

/* ----------------------------------------------------------------
 * v2 plugin API
 * ---------------------------------------------------------------- */

/* Analyze every Nth audio block. 4 blocks @ 128 frames = 512 samples =
 * ~11.6ms between pitch updates — plenty smooth for the UI (44Hz tick
 * rate) and keeps CPU cost sub-1% on the Move. */
#define TUNER_ANALYZE_INTERVAL 4

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    guitar_tuner_t *st = calloc(1, sizeof(guitar_tuner_t));
    if (!st) return NULL;
    st->last_midi_note = -1;
    st_log("guitar_tuner: instance created");
    return st;
}

static void v2_destroy_instance(void *instance) {
    guitar_tuner_t *st = (guitar_tuner_t *)instance;
    if (!st) return;
    free(st);
    st_log("guitar_tuner: instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    (void)instance; (void)key; (void)val;
    /* No writable params for V1 — all control lives in the JS UI. */
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    guitar_tuner_t *st = (guitar_tuner_t *)instance;
    if (!st || !key || !buf || buf_len < 2) return -1;

    if (strcmp(key, "freq_hz") == 0) {
        return snprintf(buf, buf_len, "%.3f", st->last_freq_hz);
    } else if (strcmp(key, "cents_off") == 0) {
        return snprintf(buf, buf_len, "%.2f", st->last_cents_off);
    } else if (strcmp(key, "midi_note") == 0) {
        return snprintf(buf, buf_len, "%d", st->last_midi_note);
    } else if (strcmp(key, "confidence") == 0) {
        return snprintf(buf, buf_len, "%.3f", st->last_confidence);
    } else if (strcmp(key, "peak") == 0) {
        return snprintf(buf, buf_len, "%d", st->last_peak);
    } else if (strcmp(key, "rms") == 0) {
        return snprintf(buf, buf_len, "%.1f", st->last_rms);
    }
    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static void v2_render_block(void *instance, int16_t *out_lr, int frames) {
    guitar_tuner_t *st = (guitar_tuner_t *)instance;
    /* Plugin is analysis-only — always emit silence. */
    memset(out_lr, 0, frames * 2 * sizeof(int16_t));
    if (!st) return;

    capture_block(st);
    if (st->samples_since_analysis >=
        TUNER_ANALYZE_INTERVAL * MOVE_FRAMES_PER_BLOCK) {
        analyze(st);
        st->samples_since_analysis = 0;
    }
}

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    st_log("guitar_tuner: plugin initialized (v2)");
    return &g_plugin_api_v2;
}
