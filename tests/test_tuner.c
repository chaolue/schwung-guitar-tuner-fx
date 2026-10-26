/*
 * Offline tests for the Guitar Tuner FX DSP. Native build, no Move needed:
 *
 *   bash tests/run_tests.sh
 *
 * The plugin source is included directly so the tests can reach its
 * internals (the analysis window, the YIN buffers) as well as its API.
 *
 *   build/test_tuner                  run the tests
 *   build/test_tuner --dump-contract  print the served chain_params and
 *                                     ui_hierarchy, one per line
 */

#include "../src/dsp/guitar_tuner.c"

#include <time.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }                 \
} while (0)

#define BLOCK MOVE_FRAMES_PER_BLOCK
#define SR    MOVE_SAMPLE_RATE

static const audio_fx_api_v2_t *api(void) {
    static const audio_fx_api_v2_t *a = NULL;
    if (!a) a = move_audio_fx_init_v2(NULL);
    return a;
}

static void *make(void) { return api()->create_instance("/nonexistent", NULL); }

static void set(void *t, const char *k, const char *v) { api()->set_param(t, k, v); }

static const char *get(void *t, const char *k) {
    static char buf[4096];
    if (api()->get_param(t, k, buf, sizeof(buf)) < 0) return NULL;
    return buf;
}

/* ------------------------------------------------------------ signals */

typedef struct {
    double freq, amp, phase;
    int harmonics;     /* 1 = sine; more = a plucked-string-like spectrum */
} osc_t;

static int16_t osc_next(osc_t *o) {
    double v = 0.0, norm = 0.0;
    for (int h = 1; h <= o->harmonics; h++) {
        v += sin(o->phase * h) / h;
        norm += 1.0 / h;
    }
    o->phase += 2.0 * M_PI * o->freq / SR;
    if (o->phase > 2.0 * M_PI * 64) o->phase -= 2.0 * M_PI * 64;
    return (int16_t)lrint(o->amp * 32767.0 * v / norm);
}

/* The chain editor re-reads the reading about once a frame (~60/s, so every
 * ~6 blocks) while the tuner's page is up, and the tuner only analyses while
 * those reads keep coming. */
#define UI_READ_EVERY 6

static void ui_read(void *t) {
    char buf[64];
    api()->get_param(t, "tuner:effective", buf, sizeof(buf));
}

/* Run `seconds` of the oscillator (or silence, o == NULL) through the plugin,
 * the same signal in both channels -- `watched` with the editor's reads
 * interleaved as on the device, or with nobody looking. */
static void run_ex(void *t, osc_t *o, double seconds, int watched) {
    int16_t buf[BLOCK * 2];
    const int blocks = (int)(seconds * SR / BLOCK + 0.5);
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < BLOCK; i++) {
            const int16_t s = o ? osc_next(o) : 0;
            buf[2 * i] = buf[2 * i + 1] = s;
        }
        api()->process_block(t, buf, BLOCK);
        if (watched && b % UI_READ_EVERY == 0) ui_read(t);
    }
}

static void run(void *t, osc_t *o, double seconds) { run_ex(t, o, seconds, 1); }

/* ------------------------------------------------------------ helpers */

static const char *SHARP[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void expected_name(double freq, char *out, size_t n, double *cents) {
    const double m = 69.0 + 12.0 * log2(freq / 440.0);
    const int nearest = (int)floor(m + 0.5);
    *cents = (m - nearest) * 100.0;
    snprintf(out, n, "%s%d", SHARP[nearest % 12], nearest / 12 - 1);
}

/* "E2 +3.4c 82.6Hz" -> name "E2", cents 3.4, hz 82.6. */
static int parse_reading(const char *s, char *name, size_t n, double *cents, double *hz) {
    char nm[16];
    if (!s || sscanf(s, "%15s %lfc %lfHz", nm, cents, hz) != 3) return 0;
    snprintf(name, n, "%s", nm);
    return 1;
}

/* ------------------------------------------------------------ tests */

static void test_pitch_accuracy(void) {
    printf("pitch accuracy\n");
    /* The six open strings, a few fretted notes up the neck, and the ends
     * of the detection range. */
    const double freqs[] = {
        82.41, 110.00, 146.83, 196.00, 246.94, 329.63,
        73.42, 92.50, 130.81, 277.18, 415.30, 523.25, 659.26, 880.00, 1046.50,
    };
    const int harmonics[] = { 1, 6 };
    for (size_t h = 0; h < sizeof(harmonics) / sizeof(harmonics[0]); h++) {
        for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
            void *t = make();
            osc_t o = { freqs[i], 0.4, 0.0, harmonics[h] };
            run(t, &o, 0.25);
            char want[16], name[16];
            double want_c, c, hz;
            expected_name(freqs[i], want, sizeof(want), &want_c);
            const char *r = get(t, "tuner");
            const int ok = parse_reading(r, name, sizeof(name), &c, &hz);
            CHECK(ok, "%.2f Hz (h%d): no reading, got \"%s\"", freqs[i], harmonics[h], r ? r : "(null)");
            if (ok) {
                CHECK(strcmp(name, want) == 0, "%.2f Hz (h%d): note %s, want %s",
                      freqs[i], harmonics[h], name, want);
                CHECK(fabs(c - want_c) < 1.0, "%.2f Hz (h%d): %.1f cents, want %.1f",
                      freqs[i], harmonics[h], c, want_c);
                CHECK(fabs(hz - freqs[i]) < freqs[i] * 0.001, "%.2f Hz (h%d): read %.1f Hz",
                      freqs[i], harmonics[h], hz);
            }
            api()->destroy_instance(t);
        }
    }
}

static void test_detuned(void) {
    printf("detuned notes\n");
    const double offsets[] = { -45.0, -20.0, -7.0, 0.0, 7.0, 20.0, 45.0 };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        void *t = make();
        osc_t o = { 110.0 * pow(2.0, offsets[i] / 1200.0), 0.4, 0.0, 4 };
        run(t, &o, 0.25);
        char name[16];
        double c, hz;
        const int ok = parse_reading(get(t, "tuner"), name, sizeof(name), &c, &hz);
        CHECK(ok && strcmp(name, "A2") == 0 && fabs(c - offsets[i]) < 1.0,
              "A2 %+.0fc read as \"%s\"", offsets[i], get(t, "tuner"));
        api()->destroy_instance(t);
    }
}

static void test_notation(void) {
    printf("notation\n");
    void *t = make();
    osc_t o = { 466.16, 0.4, 0.0, 1 };   /* A#4 / Bb4 */
    run(t, &o, 0.2);
    CHECK(strncmp(get(t, "tuner"), "A#4 ", 4) == 0, "sharps: \"%s\"", get(t, "tuner"));
    set(t, "notation", "Flats");
    CHECK(strcmp(get(t, "notation"), "1") == 0, "notation by name");
    CHECK(strncmp(get(t, "tuner"), "Bb4 ", 4) == 0, "flats: \"%s\"", get(t, "tuner"));
    set(t, "notation", "0");
    CHECK(strcmp(get(t, "notation"), "0") == 0, "notation by index");
    set(t, "notation", "banana");
    set(t, "notation", "7");
    CHECK(strcmp(get(t, "notation"), "0") == 0, "bad notation ignored");
    api()->destroy_instance(t);
}

static void test_passthrough_bit_exact(void) {
    printf("pass through is bit-exact\n");
    void *t = make();
    uint32_t seed = 12345;
    int identical = 1;
    for (int b = 0; b < 400; b++) {
        int16_t buf[BLOCK * 2], ref[BLOCK * 2];
        for (int i = 0; i < BLOCK * 2; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = ref[i] = (int16_t)(seed >> 16);
        }
        api()->process_block(t, buf, BLOCK);
        if (memcmp(buf, ref, sizeof(buf)) != 0) identical = 0;
    }
    CHECK(identical, "audio changed while not muted");
    CHECK(strcmp(get(t, "mute"), "0") == 0, "mute defaults to Off");
    api()->destroy_instance(t);
}

/* Feed a constant full-scale-ish signal and record what comes out. */
static void run_dc(void *t, int16_t level, int16_t *out, int frames) {
    for (int done = 0; done < frames; done += BLOCK) {
        int16_t buf[BLOCK * 2];
        for (int i = 0; i < BLOCK * 2; i++) buf[i] = level;
        api()->process_block(t, buf, BLOCK);
        for (int i = 0; i < BLOCK && done + i < frames; i++) out[done + i] = buf[2 * i];
    }
}

static void test_mute(void) {
    printf("mute output\n");
    void *t = make();
    enum { N = BLOCK * 16 };
    int16_t out[N];
    run_dc(t, 20000, out, BLOCK * 4);   /* primed, passing through */

    set(t, "mute", "On");
    CHECK(strcmp(get(t, "mute"), "1") == 0, "mute by name");
    run_dc(t, 20000, out, N);
    int monotonic = 1, max_step = 0, first_zero = -1;
    for (int i = 0; i < N; i++) {
        if (i > 0 && out[i] > out[i - 1]) monotonic = 0;
        if (i > 0 && abs(out[i] - out[i - 1]) > max_step) max_step = abs(out[i] - out[i - 1]);
        if (out[i] == 0 && first_zero < 0) first_zero = i;
    }
    CHECK(monotonic, "fade out is not monotonic");
    CHECK(max_step <= 20000 / (MUTE_RAMP_SAMPLES - 1) + 1, "fade out steps by %d", max_step);
    CHECK(first_zero >= MUTE_RAMP_SAMPLES - 2 && first_zero <= MUTE_RAMP_SAMPLES,
          "silent after %d samples, want ~%d", first_zero, MUTE_RAMP_SAMPLES);
    int all_zero = 1;
    for (int i = MUTE_RAMP_SAMPLES; i < N; i++) if (out[i] != 0) all_zero = 0;
    CHECK(all_zero, "not digital silence after the fade");

    /* Repeated writes must not restart (and so stall) the fade. */
    set(t, "mute", "0");
    int16_t back[N];
    for (int done = 0; done < N; done += BLOCK) {
        set(t, "mute", "0");
        run_dc(t, 20000, back + done, BLOCK);
    }
    CHECK(back[MUTE_RAMP_SAMPLES + 1] == 20000, "fade in stalled by repeated writes: %d",
          back[MUTE_RAMP_SAMPLES + 1]);
    monotonic = 1;
    for (int i = 1; i < MUTE_RAMP_SAMPLES; i++) if (back[i] < back[i - 1]) monotonic = 0;
    CHECK(monotonic, "fade in is not monotonic");

    /* Flipped back mid-fade: turns around without a jump. */
    set(t, "mute", "1");
    run_dc(t, 20000, out, BLOCK);       /* part way down */
    const int16_t mid = out[BLOCK - 1];
    set(t, "mute", "0");
    run_dc(t, 20000, out, BLOCK);
    CHECK(mid > 0 && mid < 20000 && abs(out[0] - mid) <= 100,
          "turnaround jumped from %d to %d", mid, out[0]);

    set(t, "mute", "maybe");
    CHECK(strcmp(get(t, "mute"), "0") == 0, "bad mute value ignored");
    api()->destroy_instance(t);
}

static void test_tunes_while_muted(void) {
    printf("listens while muted\n");
    void *t = make();
    set(t, "mute", "1");
    osc_t o = { 196.0, 0.4, 0.0, 3 };
    int16_t buf[BLOCK * 2];
    int silent = 1;
    for (int b = 0; b < 100; b++) {
        for (int i = 0; i < BLOCK; i++) buf[2 * i] = buf[2 * i + 1] = osc_next(&o);
        api()->process_block(t, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; i++) if (buf[i] != 0) silent = 0;
        if (b % UI_READ_EVERY == 0) ui_read(t);
    }
    CHECK(silent, "muted before the first block, yet audio leaked");
    CHECK(strncmp(get(t, "tuner"), "G3 ", 3) == 0, "muted reading \"%s\"", get(t, "tuner"));
    api()->destroy_instance(t);
}

static void test_hold_and_release(void) {
    printf("hold after the note stops\n");
    void *t = make();
    osc_t o = { 146.83, 0.4, 0.0, 3 };
    run(t, &o, 0.3);
    CHECK(strncmp(get(t, "tuner"), "D3 ", 3) == 0, "before: \"%s\"", get(t, "tuner"));
    run(t, NULL, 1.2);
    CHECK(strncmp(get(t, "tuner"), "D3 ", 3) == 0, "held 1.2 s: \"%s\"", get(t, "tuner"));
    run(t, NULL, 0.5);
    CHECK(strcmp(get(t, "tuner"), "--") == 0, "after the hold: \"%s\"", get(t, "tuner"));
    api()->destroy_instance(t);
}

static void test_level(void) {
    printf("input level\n");
    void *t = make();
    CHECK(strcmp(get(t, "level"), "-60.0") == 0, "silent level \"%s\"", get(t, "level"));
    osc_t o = { 220.0, 0.5, 0.0, 1 };
    run(t, &o, 0.1);
    const double db = atof(get(t, "level"));
    CHECK(fabs(db - 20.0 * log10(0.5)) < 0.5, "0.5 amplitude reads %.1f dB", db);
    CHECK(strcmp(get(t, "level:effective"), get(t, "level")) == 0, "level:effective");
    run(t, NULL, 0.5);
    const double fallen = atof(get(t, "level"));
    CHECK(fallen < db - 10.0 && fallen > db - 14.0, "fell to %.1f dB in 0.5 s", fallen);
    api()->destroy_instance(t);
}

static void test_quiet_and_noise_gate(void) {
    printf("noise gate\n");
    void *t = make();
    osc_t o = { 110.0, 0.002, 0.0, 1 };   /* ~-54 dBFS, under the gate */
    run(t, &o, 0.3);
    CHECK(strcmp(get(t, "tuner"), "--") == 0, "gated signal read as \"%s\"", get(t, "tuner"));
    api()->destroy_instance(t);
}

static void test_stale_reads(void) {
    printf("reading clears when the host stops running us\n");
    void *t = make();
    osc_t o = { 82.41, 0.4, 0.0, 3 };
    run(t, &o, 0.3);
    run_ex(t, &o, (double)BLOCK / SR, 0);   /* end on a block, not a read */
    /* READING_STALE_READS reads may go unanswered by a block; the next
     * one clears. (Each get() below is itself a read.) */
    for (int i = 0; i < READING_STALE_READS - 1; i++) get(t, "tuner:effective");
    CHECK(strncmp(get(t, "tuner"), "--", 2) != 0, "cleared too early");
    CHECK(strcmp(get(t, "tuner"), "--") == 0, "not cleared after %d reads",
          READING_STALE_READS + 1);
    CHECK(strcmp(get(t, "level"), "-60.0") == 0, "level not cleared");
    run(t, &o, 0.2);
    CHECK(strncmp(get(t, "tuner"), "E2 ", 3) == 0, "did not come back: \"%s\"", get(t, "tuner"));
    api()->destroy_instance(t);
}

static void test_state(void) {
    printf("state save / restore\n");
    void *a = make();
    run(a, NULL, 0.01);
    set(a, "mute", "1");
    set(a, "notation", "1");
    const char *st = get(a, "state");
    CHECK(st && strcmp(st, "{\"mute\":1,\"notation\":1}") == 0, "state \"%s\"", st ? st : "(null)");
    char saved[4096];
    snprintf(saved, sizeof(saved), "%s", st ? st : "");

    /* Restored before the first block, as the host does while building the
     * instance: the first block out must already be silent, not a fade. */
    void *b = make();
    set(b, "state", saved);
    CHECK(strcmp(get(b, "mute"), "1") == 0 && strcmp(get(b, "notation"), "1") == 0,
          "restored mute=%s notation=%s", get(b, "mute"), get(b, "notation"));
    int16_t buf[BLOCK * 2];
    for (int i = 0; i < BLOCK * 2; i++) buf[i] = 12000;
    api()->process_block(b, buf, BLOCK);
    int silent = 1;
    for (int i = 0; i < BLOCK * 2; i++) if (buf[i] != 0) silent = 0;
    CHECK(silent, "restored-muted instance leaked audio on its first block");

    set(b, "state", "{\"mute\": \"Off\", \"notation\": \"Sharps\"}");
    CHECK(strcmp(get(b, "mute"), "0") == 0 && strcmp(get(b, "notation"), "0") == 0,
          "names in state: mute=%s notation=%s", get(b, "mute"), get(b, "notation"));
    set(b, "state", "{\"notation\":1}");
    CHECK(strcmp(get(b, "mute"), "0") == 0 && strcmp(get(b, "notation"), "1") == 0,
          "partial state");
    set(b, "state", "garbage");
    CHECK(strcmp(get(b, "notation"), "1") == 0, "garbage state changed params");
    api()->destroy_instance(a);
    api()->destroy_instance(b);
}

static void test_api_edges(void) {
    printf("api edges\n");
    void *t = make();
    char small[4];
    CHECK(api()->get_param(t, "chain_params", small, sizeof(small)) == -1,
          "a truncated answer must fail");
    CHECK(api()->get_param(t, "nonsense", small, sizeof(small)) == -1, "unknown key");
    CHECK(get(t, "name") && strcmp(get(t, "name"), "Guitar Tuner FX") == 0, "name");
    CHECK(get(t, "tuner:base") && strcmp(get(t, "tuner:base"), "--") == 0, "tuner:base");
    api()->process_block(t, NULL, BLOCK);
    api()->process_block(NULL, NULL, BLOCK);
    set(t, NULL, "1");
    set(t, "mute", NULL);
    CHECK(api()->on_midi == NULL, "on_midi");
    CHECK(api()->api_version == AUDIO_FX_API_VERSION_2, "api version");
    api()->destroy_instance(t);
    api()->destroy_instance(NULL);
}

/* Nobody looking, no analysis: never read since creation, or not read for
 * WATCH_TIMEOUT_SAMPLES. Audio still passes, and a look brings the reading
 * back within one analysis cycle. */
static void test_only_analyses_while_watched(void) {
    printf("only analyses while watched\n");
    tuner_t *t = (tuner_t *)make();
    osc_t o = { 82.41, 0.4, 0.0, 3 };

    /* Never opened: a second of playing does no pitch work at all. The YIN
     * buffers are calloc'd, so any analysis would have written them. */
    run_ex(t, &o, 1.0, 0);
    int untouched = 1;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) if (t->yin_diff[tau] != 0.0f) untouched = 0;
    CHECK(untouched, "analysed with nobody looking");
    CHECK(t->midi < 0 && t->level_db == LEVEL_FLOOR_DB, "unwatched instance has a reading");

    /* The first look answers "--" and wakes it; the note follows within one
     * cycle (a snapshot, then four slices), because capture never stopped. */
    CHECK(strcmp(get(t, "tuner"), "--") == 0, "first look \"%s\"", get(t, "tuner"));
    int16_t buf[BLOCK * 2];
    int blocks = 0;
    while (blocks < 40) {
        for (int i = 0; i < BLOCK; i++) buf[2 * i] = buf[2 * i + 1] = osc_next(&o);
        api()->process_block(t, buf, BLOCK);
        blocks++;
        if (strncmp(get(t, "tuner"), "E2 ", 3) == 0) break;
    }
    CHECK(blocks <= TUNER_CHUNKS + 1, "reading took %d blocks after the first look", blocks);
    CHECK(atof(get(t, "level")) > -20.0, "level after waking: %s", get(t, "level"));

    /* Kept awake by the editor's reads, for far longer than the timeout. */
    run(t, &o, 2.0);
    CHECK(strncmp(get(t, "tuner"), "E2 ", 3) == 0, "watched tuner fell asleep: \"%s\"", get(t, "tuner"));

    /* Looked away: once the timeout passes, the analysis stops and the old
     * reading goes, so the next look cannot show a stale note. */
    run_ex(t, &o, 0.6, 0);
    CHECK(t->midi < 0 && t->phase == 0 && t->level_db == LEVEL_FLOOR_DB,
          "still holding a reading after nobody looked for 0.6 s");
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) t->yin_diff[tau] = -1.0f;
    run_ex(t, &o, 0.5, 0);
    int idle = 1;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) if (t->yin_diff[tau] != -1.0f) idle = 0;
    CHECK(idle, "kept analysing after nobody looked");

    /* Just inside the timeout it is still awake. */
    get(t, "tuner");
    run_ex(t, &o, 0.4, 0);
    CHECK(t->unwatched < WATCH_TIMEOUT_SAMPLES && t->midi >= 0, "asleep before the timeout");
    api()->destroy_instance(t);

    /* The audio path does not care who is looking: pass-through stays
     * bit-exact and Mute still mutes, unwatched. (test_mute and
     * test_passthrough_bit_exact never read the reading at all.) */
    void *u = make();
    int16_t ref[BLOCK * 2];
    for (int i = 0; i < BLOCK * 2; i++) buf[i] = ref[i] = (int16_t)(i * 97);
    api()->process_block(u, buf, BLOCK);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0, "unwatched pass-through changed the audio");
    api()->destroy_instance(u);
}

/* The difference function is computed in slices across blocks, with eight
 * partial sums: it must still be the textbook one. */
static void test_difference_function(void) {
    printf("sliced difference function == reference\n");
    tuner_t *t = (tuner_t *)make();
    for (int i = 0; i < TUNER_WINDOW; i++)
        t->win[i] = (float)(0.3 * sin(i * 0.05) + 0.1 * sin(i * 0.37 + 1.0));
    for (int c = 0; c < TUNER_CHUNKS; c++) yin_difference_chunk(t, c);
    double worst = 0.0;
    for (int tau = 1; tau <= TUNER_TAU_MAX; tau++) {
        double ref = 0.0;
        for (int i = 0; i < TUNER_WINDOW - tau; i++) {
            const double d = (double)t->win[i] - (double)t->win[i + tau];
            ref += d * d;
        }
        const double err = fabs(t->yin_diff[tau] - ref) / (ref > 1e-9 ? ref : 1.0);
        if (err > worst) worst = err;
    }
    CHECK(worst < 1e-4, "worst relative error %g", worst);
    api()->destroy_instance(t);
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Not a pass/fail on this machine's speed, just a number to watch: the
 * slow tail of blocks, which the slicing exists to keep down. The p99 rather
 * than the maximum, which on a shared machine measures the scheduler. */
static void report_block_cost(void) {
    void *t = make();
    osc_t o = { 82.41, 0.4, 0.0, 6 };
    int16_t buf[BLOCK * 2];
    enum { BLOCKS = 4000 };
    static double cost[BLOCKS];
    for (int watched = 1; watched >= 0; watched--) {
        if (!watched) run_ex(t, &o, 0.6, 0);   /* past the timeout first */
        for (int b = 0; b < BLOCKS; b++) {
            for (int i = 0; i < BLOCK; i++) buf[2 * i] = buf[2 * i + 1] = osc_next(&o);
            const double t0 = now_us();
            api()->process_block(t, buf, BLOCK);
            cost[b] = now_us() - t0;
            if (watched && b % UI_READ_EVERY == 0) ui_read(t);
        }
        qsort(cost, BLOCKS, sizeof(cost[0]), cmp_double);
        printf("block cost on this machine, %s: median %.1f us, p99 %.1f us\n",
               watched ? "watched, playing" : "unwatched, playing",
               cost[BLOCKS / 2], cost[BLOCKS * 99 / 100]);
    }
    api()->destroy_instance(t);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump-contract") == 0) {
        void *t = make();
        printf("%s\n", get(t, "chain_params"));
        printf("%s\n", get(t, "ui_hierarchy"));
        api()->destroy_instance(t);
        return 0;
    }
    /* Real readings, as JSON lines {reading, freq, notation}, for
     * tests/test_canvas.mjs to parse: the widget must read what the DSP
     * actually writes, not what someone remembers it writing. */
    if (argc > 1 && strcmp(argv[1], "--dump-readings") == 0) {
        const double freqs[] = { 82.41, 87.3, 92.5, 110.0 * 1.0116, 155.56, 196.0,
                                 233.08, 329.63, 369.99, 466.16, 830.61, 1046.5 };
        printf("[\n");
        for (int n = 0; n < 2; n++) {
            for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
                void *t = make();
                set(t, "notation", n ? "1" : "0");
                osc_t o = { freqs[i], 0.4, 0.0, 3 };
                run(t, &o, 0.25);
                printf("  {\"reading\": \"%s\", \"freq\": %.2f, \"notation\": %d},\n",
                       get(t, "tuner"), freqs[i], n);
                api()->destroy_instance(t);
            }
        }
        void *t = make();
        printf("  {\"reading\": \"%s\", \"freq\": 0, \"notation\": 0}\n]\n", get(t, "tuner"));
        api()->destroy_instance(t);
        return 0;
    }
    test_pitch_accuracy();
    test_detuned();
    test_notation();
    test_passthrough_bit_exact();
    test_mute();
    test_tunes_while_muted();
    test_hold_and_release();
    test_level();
    test_quiet_and_noise_gate();
    test_stale_reads();
    test_state();
    test_api_edges();
    test_only_analyses_while_watched();
    test_difference_function();
    report_block_cost();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
