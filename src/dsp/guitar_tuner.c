/*
 * Guitar Tuner FX -- a chromatic tuner you put in a Signal Chain.
 *
 * It listens to whatever audio reaches its chain position (Line In, a Move
 * track under Move->Schwung, a synth, the Master FX bus) and publishes the
 * nearest note, its cents deviation, the frequency and the input level, which
 * canvas.js draws in the chain editor.
 *
 * The audio is passed through BIT-FOR-BIT untouched by default. Mute Output
 * silences it, for tuning without the room hearing you: a 10 ms fade either
 * way so switching never clicks, then true digital silence once the fade has
 * finished. The tuner keeps listening while muted -- it always analyses the
 * input, never its own output.
 *
 * It only analyses while someone is looking. The chain editor re-reads the
 * reading every frame while a page showing it is up, and nothing else asks
 * for it, so those reads are the signal: with none for half a second the
 * pitch detection stops, and the next read starts it again.
 *
 * Algorithm (unchanged from the Guitar Tuner tool): the Cumulative-Mean-
 * Normalized Difference Function at the core of YIN (de Cheveigne & Kawahara
 * 2002) over a 2048-sample window, the first dip below a threshold, then
 * parabolic refinement. An RMS gate keeps noise from reading as a note.
 *
 * Realtime: every entry point runs on the SPI audio callback (see the
 * threading contract at the top of plugin_api_v1.h). Nothing below allocates,
 * locks, logs or touches a file, except the calloc/free in create/destroy,
 * which the host runs on its loader thread. The one expensive step, the
 * difference function (~1.1M multiply-adds), is split across the four blocks
 * of each analysis cycle so no single block carries all of it.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "contract_gen.h"

/* ----------------------------------------------------------------
 * Detection parameters
 * ---------------------------------------------------------------- */

/* Analysis window length. Must fit below guitar low-E (82 Hz, period ~537
 * samples at 44.1k) plus headroom for the cumulative-mean normalization.
 * 2048 samples = ~46 ms of audio, comfortable for 70-1200 Hz range. */
#define TUNER_WINDOW 2048

/* Lag bounds (samples). Frequencies below TAU_MIN or above TAU_MAX are
 * rejected -- they're almost always noise or harmonics of the real pitch.
 * TAU_MIN=37 -> 44100/37 ~ 1192 Hz (above high-E string harmonics).
 * TAU_MAX=630 -> 44100/630 ~ 70 Hz (well below low-E at 82 Hz). */
#define TUNER_TAU_MIN 37
#define TUNER_TAU_MAX 630

/* YIN threshold for accepting the first local minimum of the CMNDF. Values
 * below ~0.10 are considered a confident pitch; higher thresholds accept
 * noisier input at the risk of octave errors. 0.15 is the classic default. */
#define TUNER_YIN_THRESHOLD 0.15f

/* Noise gate -- refuse to report a pitch unless the RMS of the window
 * exceeds this (int16 units). Keeps stale readings from showing up during
 * silence. */
#define TUNER_MIN_RMS 120.0f

/* A detection this unsure (1 - CMNDF at the accepted lag) is not shown. The
 * tool applied this in its UI; here the DSP owns the reading. */
#define TUNER_MIN_CONFIDENCE 0.3f

/* One analysis cycle every 4 blocks @ 128 frames = 512 samples = ~11.6 ms
 * between pitch updates, as in the tool. The cycle's difference function is
 * computed in TUNER_CHUNKS slices, one per block. */
#define TUNER_ANALYZE_INTERVAL (4 * MOVE_FRAMES_PER_BLOCK)
#define TUNER_CHUNKS 4

/* Keep the last good reading for this long after the signal drops, so the
 * display doesn't blink during the decay tail of each plucked note. */
#define TUNER_HOLD_SAMPLES (MOVE_SAMPLE_RATE * 3 / 2)   /* 1.5 s */

/* Level meter: rises instantly, falls at this rate; the floor is "silent". */
#define LEVEL_FLOOR_DB   (-60.0f)
#define LEVEL_FALL_DB_S  (24.0f)

/* After this many reading requests with no process_block in between, the
 * host has stopped running us (a slot is parked after ~1 s of silence, and
 * a bypassed FX is not called at all), so the reading is cleared instead of
 * being left frozen on the last note. Reads arrive at most ~60/s and blocks
 * at ~344/s, so in normal running this counter never exceeds 1-2. */
#define READING_STALE_READS 24

/* With no reading request for this long, nobody is looking: the analysis is
 * skipped until the next one. The editor asks every frame (~60/s) while the
 * Tuner or Main page is up, so this is ~30 missed frames -- long enough that
 * a stalled UI frame or a refused read never pauses a tuner in use. */
#define WATCH_TIMEOUT_SAMPLES (MOVE_SAMPLE_RATE / 2)     /* 0.5 s */

/* The Mute Output fade, either way. */
#define MUTE_RAMP_SAMPLES (MOVE_SAMPLE_RATE / 100)       /* 10 ms */

/* ----------------------------------------------------------------
 * Parameters
 * ---------------------------------------------------------------- */

static const char *const ONOFF_OPTIONS[]    = { "Off", "On" };
static const char *const NOTATION_OPTIONS[] = { "Sharps", "Flats" };
#define N_ONOFF    2
#define N_NOTATION 2

/* Name tables indexed by (midi % 12), one per notation. */
static const char *const NOTE_NAMES[N_NOTATION][12] = {
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" },
    { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" },
};

/* ----------------------------------------------------------------
 * Instance state
 * ---------------------------------------------------------------- */

typedef struct {
    /* Params. */
    int mute;       /* 0 = pass through, 1 = silence the output */
    int notation;   /* 0 = sharps, 1 = flats */

    /* Output gain: 1 passes through untouched, 0 is silence, and a ramp
     * runs between them whenever Mute Output changes. */
    float gain;
    float gain_step;
    int   ramp_left;
    int   primed;   /* set by the first block; before it, changes snap */

    /* Rolling input buffer -- mono sum of L+R. New blocks are appended on
     * the right; a cycle analyses a snapshot of the most recent
     * TUNER_WINDOW samples. */
    float ring[TUNER_WINDOW];
    int   ring_write;   /* next write position, wraps at TUNER_WINDOW */
    int   samples_since_snapshot;

    /* The analysis cycle: 0 = waiting for the next snapshot, 1..CHUNKS =
     * the difference-function slice the next block computes. */
    int   phase;
    float win[TUNER_WINDOW];   /* the snapshot, oldest -> newest */

    /* Scratch buffers for YIN. Kept on the instance so nothing large is
     * stack-allocated on the audio thread. */
    float yin_diff[TUNER_TAU_MAX + 1];
    float yin_cmnd[TUNER_TAU_MAX + 1];

    /* The published reading. midi < 0 means "no note". */
    int   midi;
    float cents;      /* signed cents from the nearest note */
    float freq_hz;
    int   hold_left;  /* samples the reading is held for without a new one */
    float level_db;   /* input peak, with the meter's fall */
    int   stale_reads;
    int   unwatched;  /* samples since the last reading request, capped */
} tuner_t;

/* ----------------------------------------------------------------
 * Pitch detection (CMNDF / YIN)
 * ---------------------------------------------------------------- */

/* sum over i < n of (a[i] - a[i + tau])^2: the "difference function" from
 * YIN step 1, for one lag. Eight independent partial sums rather than one,
 * so the adds don't wait on each other and the compiler can put them in
 * vector lanes without -ffast-math. The order of summation changes the
 * result only in its last bits. */
static float yin_difference_at(const float *buf, int n, int tau) {
    const float *a = buf, *b = buf + tau;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const float d0 = a[i]     - b[i];
        const float d1 = a[i + 1] - b[i + 1];
        const float d2 = a[i + 2] - b[i + 2];
        const float d3 = a[i + 3] - b[i + 3];
        const float d4 = a[i + 4] - b[i + 4];
        const float d5 = a[i + 5] - b[i + 5];
        const float d6 = a[i + 6] - b[i + 6];
        const float d7 = a[i + 7] - b[i + 7];
        s0 += d0 * d0; s1 += d1 * d1; s2 += d2 * d2; s3 += d3 * d3;
        s4 += d4 * d4; s5 += d5 * d5; s6 += d6 * d6; s7 += d7 * d7;
    }
    for (; i < n; i++) {
        const float d = a[i] - b[i];
        s0 += d * d;
    }
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}

/* One slice of yin_diff[1..TAU_MAX]. Classic YIN uses N/2 pairs; we use the
 * whole remaining window so low-freq lags still have enough samples to be
 * meaningful. */
static void yin_difference_chunk(tuner_t *t, int chunk) {
    const int lo = 1 + (chunk * TUNER_TAU_MAX) / TUNER_CHUNKS;
    const int hi = ((chunk + 1) * TUNER_TAU_MAX) / TUNER_CHUNKS;
    for (int tau = lo; tau <= hi; tau++)
        t->yin_diff[tau] = yin_difference_at(t->win, TUNER_WINDOW - tau, tau);
}

/* Cumulative-mean-normalized difference: yin_cmnd[tau] = diff[tau] /
 * ((1/tau) * sum(diff[1..tau])). Rescales so the function starts at 1
 * and dips toward 0 at the true period. */
static void yin_cmnd(tuner_t *t) {
    t->yin_cmnd[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) {
        running += t->yin_diff[tau];
        if (running <= 0.0f) {
            t->yin_cmnd[tau] = 1.0f;
        } else {
            t->yin_cmnd[tau] = t->yin_diff[tau] * tau / running;
        }
    }
}

/* Absolute-threshold step: walk from TAU_MIN upward, find the first dip
 * below THRESHOLD, then descend to the local minimum. Returns the tau
 * of that minimum, or 0 if no dip found (unvoiced). */
static int yin_absolute_threshold(const tuner_t *t) {
    for (int tau = TUNER_TAU_MIN; tau <= TUNER_TAU_MAX; tau++) {
        if (t->yin_cmnd[tau] < TUNER_YIN_THRESHOLD) {
            /* Descend to local minimum. */
            while (tau + 1 <= TUNER_TAU_MAX &&
                   t->yin_cmnd[tau + 1] < t->yin_cmnd[tau]) {
                tau++;
            }
            return tau;
        }
    }
    return 0;
}

/* Parabolic interpolation around tau_est to get sub-sample accuracy.
 * Returns the refined (fractional) tau. */
static float yin_parabolic_refine(const tuner_t *t, int tau) {
    if (tau <= 1 || tau >= TUNER_TAU_MAX) return (float)tau;
    float s0 = t->yin_cmnd[tau - 1];
    float s1 = t->yin_cmnd[tau];
    float s2 = t->yin_cmnd[tau + 1];
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

static void clear_reading(tuner_t *t) {
    t->midi = -1;
    t->cents = 0.0f;
    t->freq_hz = 0.0f;
    t->hold_left = 0;
}

/* Collapse the block to mono (L+R)/2 in [-1, 1] and append it to the ring. */
static void capture_block(tuner_t *t, const int16_t *audio, int frames) {
    for (int i = 0; i < frames; i++) {
        int32_t sum = (int32_t)audio[i * 2] + (int32_t)audio[i * 2 + 1];
        t->ring[t->ring_write] = (float)sum / 65536.0f;  /* /2 then /32768 */
        if (++t->ring_write == TUNER_WINDOW) t->ring_write = 0;
    }
    t->samples_since_snapshot += frames;
}

/* Copy the ring into the analysis window (oldest -> newest), so the cycle
 * analyses one fixed stretch of audio while new blocks keep arriving, and
 * update the level meter. Returns the window's RMS in int16 units. */
static float take_snapshot(tuner_t *t) {
    const int head = t->ring_write;  /* next-to-write = oldest sample */
    const int tail = TUNER_WINDOW - head;
    memcpy(t->win, t->ring + head, (size_t)tail * sizeof(float));
    memcpy(t->win + tail, t->ring, (size_t)head * sizeof(float));

    float peak = 0.0f;
    double sumsq = 0.0;
    for (int i = 0; i < TUNER_WINDOW; i++) {
        float a = fabsf(t->win[i]);
        if (a > peak) peak = a;
        sumsq += (double)t->win[i] * (double)t->win[i];
    }

    const float peak_db = peak > 0.0f ? 20.0f * log10f(peak) : LEVEL_FLOOR_DB;
    const float fallen = t->level_db -
        LEVEL_FALL_DB_S * (float)TUNER_ANALYZE_INTERVAL / (float)MOVE_SAMPLE_RATE;
    t->level_db = peak_db > fallen ? peak_db : fallen;
    if (t->level_db < LEVEL_FLOOR_DB) t->level_db = LEVEL_FLOOR_DB;
    if (t->level_db > 0.0f) t->level_db = 0.0f;

    return sqrtf((float)(sumsq / TUNER_WINDOW)) * 32767.0f;
}

/* The end of a cycle: a fresh detection replaces the reading; no detection
 * leaves the last one up until the hold runs out. One call per cycle. */
static void publish(tuner_t *t, int voiced, int midi, float cents, float freq) {
    if (voiced) {
        t->midi = midi;
        t->cents = cents;
        t->freq_hz = freq;
        t->hold_left = TUNER_HOLD_SAMPLES;
    } else if (t->hold_left > TUNER_ANALYZE_INTERVAL) {
        t->hold_left -= TUNER_ANALYZE_INTERVAL;
    } else {
        clear_reading(t);
    }
}

static void finish_analysis(tuner_t *t) {
    yin_cmnd(t);
    const int tau = yin_absolute_threshold(t);
    if (tau == 0) {
        publish(t, 0, -1, 0.0f, 0.0f);
        return;
    }
    const float confidence = 1.0f - t->yin_cmnd[tau];
    const float freq = (float)MOVE_SAMPLE_RATE / yin_parabolic_refine(t, tau);
    int midi;
    float cents;
    freq_to_note(freq, &midi, &cents);
    publish(t, confidence > TUNER_MIN_CONFIDENCE, midi, cents, freq);
}

/* Advance the analysis cycle by one block: a snapshot every
 * TUNER_ANALYZE_INTERVAL samples (below the gate that is the whole cycle),
 * then one difference-function slice per block, then the decision. */
static void analysis_step(tuner_t *t) {
    if (t->phase == 0) {
        if (t->samples_since_snapshot < TUNER_ANALYZE_INTERVAL) return;
        t->samples_since_snapshot = 0;
        if (take_snapshot(t) < TUNER_MIN_RMS) {
            publish(t, 0, -1, 0.0f, 0.0f);
            return;
        }
        t->phase = 1;
    }
    yin_difference_chunk(t, t->phase - 1);
    if (t->phase < TUNER_CHUNKS) {
        t->phase++;
        return;
    }
    t->phase = 0;
    finish_analysis(t);
}

/* ----------------------------------------------------------------
 * Mute Output
 * ---------------------------------------------------------------- */

static void set_mute(tuner_t *t, int mute) {
    mute = mute ? 1 : 0;
    const float target = mute ? 0.0f : 1.0f;

    /* Before any audio has passed (the host restoring saved state while it
     * builds the instance) there is nothing to fade from: jump straight
     * there, or a tuner saved muted would leak 10 ms of sound on load. */
    if (!t->primed) {
        t->mute = mute;
        t->gain = target;
        t->ramp_left = 0;
        return;
    }
    /* The same value again (a repeated write, a scene morph) must not
     * restart the fade, or a stream of identical writes would stall it. */
    if (mute == t->mute) return;
    t->mute = mute;
    if (t->gain == target) {
        t->ramp_left = 0;
        return;
    }
    /* Always a full-length fade from wherever the gain is now, so a switch
     * flipped back mid-fade turns around smoothly. */
    t->ramp_left = MUTE_RAMP_SAMPLES;
    t->gain_step = (target - t->gain) / (float)MUTE_RAMP_SAMPLES;
}

static inline int16_t scale_sample(int16_t s, float g) {
    return (int16_t)lrintf((float)s * g);   /* |g| <= 1: cannot overflow */
}

static void apply_output_gain(tuner_t *t, int16_t *audio, int frames) {
    int i = 0;
    for (; i < frames && t->ramp_left > 0; i++) {
        t->gain += t->gain_step;
        if (--t->ramp_left == 0) t->gain = t->mute ? 0.0f : 1.0f;
        audio[2 * i]     = scale_sample(audio[2 * i], t->gain);
        audio[2 * i + 1] = scale_sample(audio[2 * i + 1], t->gain);
    }
    if (i == frames) return;
    if (t->gain == 1.0f) return;   /* pass through: the audio is untouched */
    memset(audio + 2 * i, 0, (size_t)(frames - i) * 2 * sizeof(int16_t));
}

/* ----------------------------------------------------------------
 * Params and state
 * ---------------------------------------------------------------- */

/* Parse a number, refusing text that is not one (atof would read it as 0). */
static int parse_number(const char *s, float *out) {
    if (!s) return 0;
    char *end = NULL;
    const float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* An enum accepts its option NAME or its INDEX, and nothing else. get_param
 * reports the index, which is what the knob grid learns the wire from. */
static int parse_enum(const char *val, const char *const *options, int n, int *out) {
    if (!val) return 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(val, options[i]) == 0) { *out = i; return 1; }
    }
    float v;
    if (!parse_number(val, &v)) return 0;
    const int idx = (int)lrintf(v);
    if (idx < 0 || idx >= n) return 0;
    *out = idx;
    return 1;
}

/* Find `"key"` in a flat JSON object and return a pointer to its value. */
static const char *json_value(const char *json, const char *key) {
    char pat[32];
    const int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* A saved enum, as an index or (hand-written) as its quoted name. */
static int json_enum(const char *json, const char *key,
                     const char *const *options, int n, int *out) {
    const char *v = json_value(json, key);
    if (!v) return 0;
    if (*v != '"') return parse_enum(v, options, n, out);
    char name[16];
    const char *end = strchr(v + 1, '"');
    if (!end || end - (v + 1) >= (int)sizeof(name)) return 0;
    memcpy(name, v + 1, (size_t)(end - (v + 1)));
    name[end - (v + 1)] = '\0';
    return parse_enum(name, options, n, out);
}

/* snprintf reports the length it WANTED; a truncated answer is a failed one. */
static int fit(int n, int len) {
    return (n < 0 || n >= len) ? -1 : n;
}

static int put(char *buf, int len, const char *s) {
    const int n = (int)strlen(s);
    if (n >= len) return -1;
    memcpy(buf, s, (size_t)n + 1);
    return n;
}

/* A reading request: someone is looking, so analysis runs (see
 * WATCH_TIMEOUT_SAMPLES). Also counts requests since the last block; see
 * READING_STALE_READS. */
static void check_stale(tuner_t *t) {
    t->unwatched = 0;
    if (++t->stale_reads > READING_STALE_READS) {
        t->stale_reads = READING_STALE_READS + 1;   /* no wrap */
        clear_reading(t);
        t->level_db = LEVEL_FLOOR_DB;
    }
}

/* "E2 +3.4c 82.6Hz", or "--" when there is no note. One string on purpose:
 * the knob grid re-reads a `live` key every tick, one read shared between
 * all of them, so splitting it into several keys would slow each down. The
 * note comes first so the grid's short cell label lands on it. */
static int format_reading(tuner_t *t, char *buf, int len) {
    check_stale(t);
    if (t->midi < 0) return put(buf, len, "--");
    /* Exactly zero prints as "+0.0", never "-0.0". */
    const double cents = fabsf(t->cents) < 0.05f ? 0.0 : (double)t->cents;
    return fit(snprintf(buf, len, "%s%d %+.1fc %.1fHz",
                        NOTE_NAMES[t->notation][t->midi % 12],
                        t->midi / 12 - 1, cents, (double)t->freq_hz), len);
}

static int format_level(tuner_t *t, char *buf, int len) {
    check_stale(t);
    return fit(snprintf(buf, len, "%.1f", (double)t->level_db), len);
}

/* ----------------------------------------------------------------
 * audio_fx_api_v2
 * ---------------------------------------------------------------- */

static void *tuner_create(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    tuner_t *t = (tuner_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    clear_reading(t);
    t->level_db = LEVEL_FLOOR_DB;
    t->unwatched = WATCH_TIMEOUT_SAMPLES;   /* asleep until first looked at */
    set_mute(t, 0);
    return t;
}

static void tuner_destroy(void *instance) {
    free(instance);
}

static void tuner_process(void *instance, int16_t *audio, int frames) {
    tuner_t *t = (tuner_t *)instance;
    if (!t || !audio || frames <= 0) return;
    t->primed = 1;
    t->stale_reads = 0;

    /* Always captured, before any muting: ~one add per sample, and it keeps
     * the window full of current audio so a reading is back within one
     * analysis cycle (~12 ms) of someone looking again. */
    capture_block(t, audio, frames);
    if (t->unwatched < WATCH_TIMEOUT_SAMPLES) {
        t->unwatched += frames;
        analysis_step(t);
    } else if (t->phase != 0 || t->midi >= 0 || t->level_db > LEVEL_FLOOR_DB) {
        /* Nobody looking: drop the half-done cycle and the old reading, so
         * the next look starts fresh instead of showing a stale note. */
        t->phase = 0;
        clear_reading(t);
        t->level_db = LEVEL_FLOOR_DB;
    }
    apply_output_gain(t, audio, frames);
}

static void tuner_set_param(void *instance, const char *key, const char *val) {
    tuner_t *t = (tuner_t *)instance;
    if (!t || !key || !val) return;
    int idx;

    if (strcmp(key, "mute") == 0) {
        if (parse_enum(val, ONOFF_OPTIONS, N_ONOFF, &idx)) set_mute(t, idx);
        return;
    }
    if (strcmp(key, "notation") == 0) {
        if (parse_enum(val, NOTATION_OPTIONS, N_NOTATION, &idx)) t->notation = idx;
        return;
    }
    /* Saved state, as produced by get_param("state") below. */
    if (strcmp(key, "state") == 0) {
        if (json_enum(val, "mute", ONOFF_OPTIONS, N_ONOFF, &idx)) set_mute(t, idx);
        if (json_enum(val, "notation", NOTATION_OPTIONS, N_NOTATION, &idx)) t->notation = idx;
        return;
    }
    /* `tuner` and `level` are readouts and `display` is a page; writes to
     * them mean nothing. */
}

static int tuner_get_param(void *instance, const char *key, char *buf, int len) {
    tuner_t *t = (tuner_t *)instance;
    if (!t || !key || !buf || len <= 0) return -1;

    if (strcmp(key, "mute") == 0)     return fit(snprintf(buf, len, "%d", t->mute), len);
    if (strcmp(key, "notation") == 0) return fit(snprintf(buf, len, "%d", t->notation), len);

    /* The readouts. The grid asks for `tuner:effective` every tick (the
     * param is declared live), and for the plain key or `:base` on its value
     * rotation; all three are the same reading. */
    if (strcmp(key, "tuner") == 0 || strcmp(key, "tuner:effective") == 0 ||
        strcmp(key, "tuner:base") == 0)
        return format_reading(t, buf, len);
    if (strcmp(key, "level") == 0 || strcmp(key, "level:effective") == 0 ||
        strcmp(key, "level:base") == 0)
        return format_level(t, buf, len);

    if (strcmp(key, "state") == 0)
        return fit(snprintf(buf, len, "{\"mute\":%d,\"notation\":%d}",
                            t->mute, t->notation), len);
    if (strcmp(key, "name") == 0) return put(buf, len, "Guitar Tuner FX");

    /* Served from here, not left to module.json: the chain host's fallback
     * re-serialises module.json's chain_params and drops `viz`, `live`,
     * `access` and `step`, which would lose the custom widget, the live
     * refresh and the read-only frame. Both strings are generated from
     * module.json at build time, so there is one source of truth. */
    if (strcmp(key, "chain_params") == 0) return put(buf, len, TUNER_CHAIN_PARAMS_JSON);
    if (strcmp(key, "ui_hierarchy") == 0) return put(buf, len, TUNER_UI_HIERARCHY_JSON);

    return -1;
}

/* Built at compile time and never written: the host may load a second
 * instance while this one renders, so init must not touch shared state. */
static audio_fx_api_v2_t g_fx_api = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = tuner_create,
    .destroy_instance = tuner_destroy,
    .process_block    = tuner_process,
    .set_param        = tuner_set_param,
    .get_param        = tuner_get_param,
    .on_midi          = NULL,
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &g_fx_api;
}
