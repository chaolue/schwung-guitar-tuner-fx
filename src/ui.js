/*
 * Guitar Tuner — chromatic tuner with pad-grid note display.
 *
 * The DSP plugin (guitar_tuner.c) runs pitch detection on Move Input every
 * audio block and publishes frequency, nearest-note, cents deviation, peak,
 * and RMS via get_param. This UI reads those values ~44x/sec and drives two
 * displays:
 *
 *   1. The 128x64 OLED — centered note readout + signed cents value + a
 *      full-width needle meter (zero at center, clamped to ±50 cents).
 *   2. The 32-pad grid — lights up a 3x4 bitmap of the current note letter
 *      in cols 0-2, with a 3x4 accidental glyph (# or ♭, user's choice) in
 *      cols 4-6 for sharps/flats. Color tells you how close you are:
 *      green (locked), white (close), amber (off), red (very off).
 *
 * A 1.5s hold window keeps the displayed note visible briefly after the
 * signal decays so the UI doesn't flicker at the end of each plucked note.
 * Accidental notation (flat vs sharp) is a user preference read from
 * shadow_config.json; it defaults to flats.
 */

import {
    Black, White,
    BrightGreen,
    BrightRed,
    VividYellow
} from '/data/UserData/schwung/shared/constants.mjs';

/* ---------------- Constants ---------------- */

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* OLED layout */
const HEADER_Y     = 2;
const HEADER_DIV_Y = 11;
const NOTE_Y       = 18;
const CENTS_Y      = 36;
const NEEDLE_Y     = 44;
const NEEDLE_H     = 7;
const FOOTER_Y     = SCREEN_HEIGHT - 8;

/* Cents thresholds for "how close am I" categorization. */
const LOCK_CENTS  = 3;    /* green, locked */
const CLOSE_CENTS = 10;   /* white */
const OFF_CENTS   = 25;   /* amber */
/* > OFF_CENTS → red */

/* MIDI constants */
const CC_BACK = 51;
const PAD_BASE = 68;    /* note number of bottom-left pad */

/* Name tables indexed by (midi % 12). */
const SHARP_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const FLAT_NAMES  = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"];

/* Hold window — keep the last good reading on-screen for this many ticks
 * after the signal drops, so the UI doesn't flicker during the decay of
 * each pluck. ~44 ticks/sec → 66 ticks ≈ 1.5s. */
const HOLD_FRAMES = 66;

const SHADOW_CFG = "/data/UserData/schwung/shadow_config.json";

/* ---------------- Letter + accidental glyphs ----------------
 *
 * Each glyph is 4 rows × 3 cols. We encode each row as a 3-bit integer
 * where bit 2 = col 0, bit 1 = col 1, bit 0 = col 2 (MSB = leftmost).
 * Rows are listed top-down: [row3, row2, row1, row0].
 *
 * Example — letter A:
 *   . X .   row 3 (top)     0b010 = 2
 *   X . X   row 2            0b101 = 5
 *   X X X   row 1            0b111 = 7
 *   X . X   row 0 (bottom)   0b101 = 5
 */

const LETTER_GLYPHS = {
    A: [0b010, 0b101, 0b111, 0b101],
    B: [0b110, 0b111, 0b110, 0b111],
    C: [0b011, 0b100, 0b100, 0b011],
    D: [0b110, 0b101, 0b101, 0b110],
    E: [0b111, 0b100, 0b110, 0b111],
    F: [0b111, 0b100, 0b110, 0b100],
    G: [0b011, 0b100, 0b101, 0b011]
};

const ACCIDENTAL_GLYPHS = {
    sharp: [0b101, 0b111, 0b101, 0b111],  /* # — verticals + 2 crossbars */
    flat:  [0b100, 0b110, 0b101, 0b110]   /* ♭ — stem + right loop */
};

/* ---------------- State ---------------- */

let frameCount = 0;

/* Accidental notation — loaded from shadow_config.json, defaults to flat. */
let accidentalStyle = "flat";

/* Most-recent DSP readings (after hold-window processing). */
let freqHz = 0;
let midiNote = -1;
let centsOff = 0;
let peak = 0;
let rms = 0;
let confidence = 0;
let holdFramesLeft = 0;

/* Last-sent pad colors. First frame sends everything; subsequent frames
 * only send changed pads to stay under the LED buffer budget. */
let padColors = new Array(32).fill(Black);
let padColorsInit = false;

/* ---------------- Config ---------------- */

function loadConfig() {
    if (host_file_exists(SHADOW_CFG)) {
        try {
            const cfg = JSON.parse(host_read_file(SHADOW_CFG) || "{}");
            if (cfg.tuner_accidental_style === "sharp" ||
                cfg.tuner_accidental_style === "flat") {
                accidentalStyle = cfg.tuner_accidental_style;
            }
        } catch (e) { /* default stays */ }
    }
}

/* ---------------- Name helpers ---------------- */

function nameForMidi(midi) {
    const table = (accidentalStyle === "flat") ? FLAT_NAMES : SHARP_NAMES;
    const oct = Math.floor(midi / 12) - 1;
    return table[midi % 12] + oct;
}

/* Returns {letter, accidental} — letter is "A"-"G", accidental is "", "#", or "b". */
function noteParts(midi) {
    if (midi < 0) return {letter: "", accidental: ""};
    const table = (accidentalStyle === "flat") ? FLAT_NAMES : SHARP_NAMES;
    const name = table[midi % 12];
    const letter = name[0];
    const accidental = name.length > 1 ? name[1] : "";
    return {letter: letter, accidental: accidental};
}

/* ---------------- DSP readings + hold window ---------------- */

function readParamStr(key) {
    if (typeof host_module_get_param !== "function") return null;
    return host_module_get_param(key);
}
function readParamFloat(key, dflt) {
    const s = readParamStr(key);
    if (!s) return dflt;
    const f = parseFloat(s);
    return isNaN(f) ? dflt : f;
}
function readParamInt(key, dflt) {
    const s = readParamStr(key);
    if (!s) return dflt;
    const n = parseInt(s, 10);
    return isNaN(n) ? dflt : n;
}

/* Pull fresh readings from the DSP. If this tick is unvoiced but the last
 * voiced reading was recent, hold it so the UI doesn't blink during the
 * decay tail of each plucked note. */
function updateReadings() {
    const rawFreq  = readParamFloat("freq_hz", 0);
    const rawMidi  = readParamInt("midi_note", -1);
    const rawCents = readParamFloat("cents_off", 0);
    const rawConf  = readParamFloat("confidence", 0);
    peak = readParamInt("peak", 0);
    rms  = readParamFloat("rms", 0);

    const instantVoiced = rawFreq > 0 && rawConf > 0.3;

    if (instantVoiced) {
        freqHz = rawFreq;
        midiNote = rawMidi;
        centsOff = rawCents;
        confidence = rawConf;
        holdFramesLeft = HOLD_FRAMES;
    } else if (holdFramesLeft > 0) {
        holdFramesLeft--;
    } else {
        freqHz = 0;
        midiNote = -1;
        centsOff = 0;
        confidence = 0;
    }
}

function voiced() { return midiNote >= 0; }

function categoryColor(absC) {
    if (absC < LOCK_CENTS)  return BrightGreen;
    if (absC < CLOSE_CENTS) return White;
    if (absC < OFF_CENTS)   return VividYellow;
    return BrightRed;
}

/* ---------------- OLED rendering ---------------- */

function drawHeader() {
    print(2, HEADER_Y, "Guitar Tuner", 1);
    /* Accidental-notation indicator on the right so the user knows whether
     * they're looking at flats or sharps without digging into settings. */
    const label = (accidentalStyle === "flat") ? "b" : "#";
    print(SCREEN_WIDTH - 8, HEADER_Y, label, 1);
    draw_line(0, HEADER_DIV_Y, SCREEN_WIDTH - 1, HEADER_DIV_Y, 1);
}

function drawNote() {
    if (!voiced()) {
        const dots = ".".repeat(1 + Math.floor(frameCount / 8) % 3);
        print(2, NOTE_Y, "Listening" + dots, 1);
        return;
    }
    /* Centered note name. Default font is ~6px wide. */
    const name = nameForMidi(midiNote);
    const approxCharW = 6;
    const totalW = name.length * approxCharW;
    const x = Math.max(2, Math.floor((SCREEN_WIDTH - totalW) / 2));
    print(x, NOTE_Y, name, 1);

    /* Cents value centered below. */
    const sign = centsOff >= 0 ? "+" : "-";
    const absC = Math.abs(centsOff);
    const centsLabel = sign + Math.round(absC) + "c";
    const cW = centsLabel.length * approxCharW;
    const cx = Math.max(2, Math.floor((SCREEN_WIDTH - cW) / 2));
    print(cx, CENTS_Y, centsLabel, 1);

    /* Full-width horizontal needle. Zero-mark at center, needle at cents
     * position clamped to ±50. */
    const barX = 6;
    const barW = SCREEN_WIDTH - barX * 2;
    draw_rect(barX, NEEDLE_Y, barW, NEEDLE_H, 1);
    const centerX = barX + Math.floor(barW / 2);
    draw_line(centerX, NEEDLE_Y - 1, centerX, NEEDLE_Y + NEEDLE_H, 1);
    const clamped = Math.max(-50, Math.min(50, centsOff));
    const needleX = centerX + Math.round((clamped / 50) * (barW / 2 - 2));
    fill_rect(needleX - 1, NEEDLE_Y + 1, 3, NEEDLE_H - 2, 1);
}

function drawFooter() {
    /* VU bar on the left. */
    const vuMaxW = 48;
    const vuW = Math.min(vuMaxW, Math.round((peak / 32768.0) * vuMaxW));
    draw_rect(0, FOOTER_Y, vuMaxW + 2, 6, 1);
    if (vuW > 0) fill_rect(1, FOOTER_Y + 1, vuW, 4, 1);
    /* Frequency on the right. */
    const fStr = voiced() ? (freqHz.toFixed(1) + "Hz") : "--";
    print(vuMaxW + 6, FOOTER_Y, fStr, 1);
}

/* ---------------- Pad rendering ---------------- */

function sendLed(note, color) {
    if (typeof move_midi_internal_send !== "function") return;
    move_midi_internal_send([0x09, 0x90, note, color]);
}

/* Return bit for col inside a 3-bit glyph row (bit 2 = col 0, bit 0 = col 2). */
function glyphBit(rowBits, col) {
    return ((rowBits >> (2 - col)) & 1) === 1;
}

/* Compute per-pad target colors for the current frame. Pad index 0 = bottom-
 * left (note 68), index 31 = top-right (note 99). Within a row,
 * col 0 = leftmost, col 7 = rightmost. Letter glyph occupies cols 0-2,
 * accidental glyph occupies cols 4-6; cols 3 and 7 stay dark. */
function computePadColors() {
    const target = new Array(32).fill(Black);
    if (!voiced()) return target;

    const col = categoryColor(Math.abs(centsOff));
    const parts = noteParts(midiNote);
    const letterRows = LETTER_GLYPHS[parts.letter];
    const accidentalRows = parts.accidental
        ? ((accidentalStyle === "flat")
           ? ACCIDENTAL_GLYPHS.flat
           : ACCIDENTAL_GLYPHS.sharp)
        : null;

    for (let r = 0; r < 4; r++) {
        /* Glyph rows are stored top-down: idx 0 = row 3 (top of OLED/pads). */
        const letterRowBits = letterRows ? letterRows[3 - r] : 0;
        const accRowBits = accidentalRows ? accidentalRows[3 - r] : 0;

        for (let c = 0; c < 3; c++) {
            if (glyphBit(letterRowBits, c)) target[r * 8 + c] = col;
        }
        for (let c = 0; c < 3; c++) {
            if (glyphBit(accRowBits, c)) target[r * 8 + (c + 4)] = col;
        }
    }

    return target;
}

function refreshPadLeds() {
    const target = computePadColors();
    for (let i = 0; i < 32; i++) {
        if (!padColorsInit || target[i] !== padColors[i]) {
            sendLed(PAD_BASE + i, target[i]);
            padColors[i] = target[i];
        }
    }
    padColorsInit = true;
}

/* ---------------- Lifecycle ---------------- */

globalThis.init = function() {
    frameCount = 0;
    freqHz = 0; midiNote = -1; centsOff = 0;
    peak = 0; rms = 0; confidence = 0;
    holdFramesLeft = 0;
    padColors = new Array(32).fill(Black);
    padColorsInit = false;
    accidentalStyle = "flat";
    loadConfig();
};

globalThis.tick = function() {
    frameCount++;

    /* Re-read config once per second so a change in the web UI takes effect
     * without the user re-entering the tool. */
    if ((frameCount % 44) === 0) loadConfig();

    updateReadings();

    clear_screen();
    drawHeader();
    drawNote();
    drawFooter();

    refreshPadLeds();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* Filter capacitive-touch noise from knob touches (notes 0-9). */
    if ((status === 0x90 || status === 0x80) && d1 < 16) return;

    if (status === 0xB0 && d1 === CC_BACK && d2 > 0) {
        host_exit_module();
    }
};
