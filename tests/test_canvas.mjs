#!/usr/bin/env node
/*
 * Offline tests for src/canvas.js: the tuner cell widget and the Tuner page.
 *
 *   node tests/test_canvas.mjs [build/readings.json]
 *
 * canvas.js is loaded the way the device loads it -- as a SCRIPT that assigns
 * globalThis.canvas_overlay, not as a module. The drawing context is a
 * stand-in for Schwung's frame context: the same primitives, clipped to the
 * frame, COUNTING anything drawn outside it. On the device such pixels are
 * silently dropped, so a drawer that overflows looks fine in one size and
 * loses its edge in another -- the count is how that shows up here.
 *
 * With build/readings.json (written by `test_tuner --dump-readings`) the
 * parser is checked against strings the C side really produced.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const src = fs.readFileSync(path.join(HERE, "..", "src", "canvas.js"), "utf8");
const g = {};
new Function("globalThis", src)(g);
const ov = g.canvas_overlay;

let pass = 0, fail = 0;
function check(cond, msg) {
    if (cond) pass++;
    else { fail++; console.log("  FAIL " + msg); }
}

/* The device font's real advance widths (Schwung's font5x7). */
const font = JSON.parse(fs.readFileSync(path.join(HERE, "font_widths.json"), "utf8"));
function textWidth(s) {
    let w = 0;
    for (const ch of String(s)) w += (font.widths[ch] !== undefined ? font.widths[ch] : 5) + font.charSpacing;
    return Math.max(0, w - font.charSpacing);
}

function makeCtx(w, h) {
    const px = new Uint8Array(w * h);
    let clipped = 0, printed = [];
    const ctx = {
        width: w, height: h,
        fillRect(x, y, rw, rh, color) {
            x = Math.round(x); y = Math.round(y); rw = Math.round(rw); rh = Math.round(rh);
            if (!(rw > 0 && rh > 0)) return;
            if (x < 0 || y < 0 || x + rw > w || y + rh > h) clipped++;
            for (let yy = Math.max(0, y); yy < Math.min(h, y + rh); yy++)
                for (let xx = Math.max(0, x); xx < Math.min(w, x + rw); xx++)
                    px[yy * w + xx] = color ? 1 : 0;
        },
        print(x, y, text, color) {
            if (x < 0 || y < 0 || x + textWidth(text) > w || y + 7 > h) clipped++;
            printed.push(String(text));
            /* A stand-in for the glyphs: light the text's box. */
            ctx.fillRect(x, y, textWidth(text), 7, color);
        },
        textWidth,
        setPixel(x, y, c) { ctx.fillRect(x, y, 1, 1, c); },
        line() {}, fillCircle() {}, drawCircle() {}, drawArc() {},
    };
    return {
        ctx, clipped: () => clipped, printed: () => printed,
        lit: () => px.reduce((a, b) => a + b, 0),
        px: (x, y) => px[y * w + x],
    };
}

/* ---------------------------------------------------------------- contract */

check(ov && ov.widgetKind === "custom:tuner", "registers custom:tuner");
check(typeof ov.drawCell === "function" && typeof ov.drawPage === "function",
      "exports drawCell and drawPage");
const mod = JSON.parse(fs.readFileSync(path.join(HERE, "..", "src", "module.json"), "utf8"));
const params = mod.capabilities.chain_params;
const declared = params.filter((p) => p.viz && p.viz.kind).map((p) => p.viz.kind);
check(declared.includes(ov.widgetKind), "the kind canvas.js registers is the one module.json declares");
const page = params.find((p) => p.type === "canvas" && p.as_page);
check(page && (page.extra_keys || []).includes("level"), "the page asks for `level`");

/* ------------------------------------------------------------------ parser */

const { parseReading, centsText, BIG } = ov._test;
const pr = (s) => JSON.stringify(parseReading(s));
const R = (letter, acc, octave, cents, hz) => JSON.stringify({ letter, acc, octave, cents, hz });
check(pr("E2 +3.4c 82.6Hz") === R("E", "", "2", 3.4, 82.6), "parses a natural");
check(pr("C#3 -12.0c 136.3Hz") === R("C", "#", "3", -12, 136.3), "parses a sharp");
check(pr("Bb4 +0.0c 466.2Hz") === R("B", "b", "4", 0, 466.2), "parses a flat");
check(pr("C-1 +1.0c 8.2Hz") === R("C", "", "-1", 1, 8.2), "parses octave -1");
for (const bad of [null, undefined, "", "--", "garbage", "E2", "E2 +3.4c", "H2 +1.0c 1.0Hz", "3.4"]) {
    check(parseReading(bad) === null, `unreadable ${JSON.stringify(bad)} is null`);
}
check(centsText({ cents: 3.4 }) === "+3c" && centsText({ cents: -12.6 }) === "-13c" &&
      centsText({ cents: 0.4 }) === "0c" && centsText({ cents: -0.4 }) === "0c", "cents text");

/* Every glyph a reading can need, at the declared 5x7. */
for (const ch of "ABCDEFG#b-0123456789") {
    const glyph = BIG[ch];
    check(glyph && glyph.length === 7 && glyph.every((r) => /^[01]{5}$/.test(r)), `big glyph ${ch}`);
}

/* ------------------------------------------ parser == DSP (if given) */

const readingsFile = process.argv[2];
if (readingsFile && fs.existsSync(readingsFile)) {
    const rows = JSON.parse(fs.readFileSync(readingsFile, "utf8"));
    const SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    const FLAT = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"];
    let checked = 0;
    for (const row of rows) {
        const r = parseReading(row.reading);
        if (!row.freq) { check(r === null && row.reading === "--", "the DSP's 'no note' is unreadable"); continue; }
        const m = 69 + 12 * Math.log2(row.freq / 440);
        const n = Math.round(m);
        const want = (row.notation ? FLAT : SHARP)[n % 12] + (Math.floor(n / 12) - 1);
        check(r && r.letter + r.acc + r.octave === want, `${row.reading} names ${want}`);
        check(r && Math.abs(r.cents - (m - n) * 100) < 1, `${row.reading} cents`);
        check(r && Math.abs(r.hz - row.freq) < row.freq * 0.0005, `${row.reading} Hz`);
        checked++;
    }
    check(checked >= 20, `checked ${checked} DSP readings`);
} else {
    console.log("  (no readings file given; skipping the C cross-check)");
}

/* ---------------------------------------------------------- drawCell sweep */

const READINGS = [null, undefined, "", "--", "garbage", "E2 +0.0c 82.4Hz", "E2 +2.9c 82.6Hz",
                  "A2 -12.3c 108.5Hz", "C#3 +49.9c 142.6Hz", "Gb5 -50.0c 719.6Hz",
                  "Bb4 +88.0c 500.0Hz", "C-1 +1.0c 8.2Hz", "D#6 -0.1c 1244.5Hz"];
let cellOverflow = 0, cellThrew = 0, cells = 0;
for (let w = 7; w <= 64; w += 1) {
    for (let h = 3; h <= 30; h += 1) {
        for (const v of READINGS) {
            const t = makeCtx(w, h);
            try {
                ov.drawCell(t.ctx, { values: { tuner: v }, group: { keys: ["tuner"] }, nowMs: 0 });
            } catch (e) { cellThrew++; if (cellThrew < 3) console.log("    threw:", w, h, v, e.message); }
            if (t.clipped()) { cellOverflow++; if (cellOverflow < 4) console.log("    overflow:", w, h, v); }
            cells++;
        }
    }
}
check(cellThrew === 0, `drawCell never throws (${cellThrew} of ${cells})`);
check(cellOverflow === 0, `drawCell stays inside its frame (${cellOverflow} of ${cells} overflowed)`);

let threw = false;
try { const t = makeCtx(32, 15); ov.drawCell(t.ctx, {}); ov.drawCell(t.ctx, undefined); } catch (e) { threw = true; }
check(!threw, "drawCell tolerates a missing payload");

/* The device's grid hands the cell 32x15: note on top, meter below. */
{
    const t = makeCtx(32, 15);
    ov.drawCell(t.ctx, { values: { tuner: "A2 -12.3c 108.5Hz" }, group: { keys: ["tuner"] } });
    check(t.printed().includes("A2"), "the 32x15 cell names the note");
    const none = makeCtx(32, 15);
    ov.drawCell(none.ctx, { values: { tuner: "--" }, group: { keys: ["tuner"] } });
    check(none.printed().includes("--"), "no note reads --");
}

/* In tune fills the centre window; off, only a needle -- so "in tune" lights more. */
function cellLit(v) {
    const t = makeCtx(32, 15);
    ov.drawCell(t.ctx, { values: { tuner: v }, group: { keys: ["tuner"] } });
    return t.lit();
}
check(cellLit("E2 +1.0c 82.5Hz") > cellLit("E2 +20.0c 83.4Hz"), "in tune is the louder picture");
check(cellLit("E2 -20.0c 81.5Hz") === cellLit("E2 +20.0c 83.4Hz"), "flat and sharp mirror each other");

/* The needle moves the right way. */
function needleX(v) {
    const t = makeCtx(64, 20);
    ov.drawCell(t.ctx, { values: { tuner: v }, group: { keys: ["tuner"] } });
    /* The needle is the run of lit pixels on the meter's middle row, minus
     * the frame and the centre mark. */
    const y = 20 - 4;
    const xs = [];
    for (let x = 3; x < 61; x++) if (t.px(x, y) && x !== 32 && x !== 31) xs.push(x);
    return xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : null;
}
const flatX = needleX("A2 -30.0c 108.1Hz"), sharpX = needleX("A2 +30.0c 111.9Hz");
check(flatX !== null && sharpX !== null && flatX < 31 && sharpX > 32, `needle left when flat, right when sharp (${flatX}, ${sharpX})`);

/* ---------------------------------------------------------- drawPage sweep */

const PAGES = [
    {},
    { tuner: null },
    { tuner: "--", mute: "0", level: "-60.0" },
    { tuner: "--", mute: "1", level: "-20.0" },
    { tuner: "E2 +1.0c 82.5Hz", mute: "0", level: "-6.0" },
    { tuner: "F#2 -17.0c 90.9Hz", mute: "1", level: "-40.0" },
    { tuner: "Bb4 +38.0c 476.9Hz", mute: "1", level: "0.0" },
    { tuner: "D#6 -49.9c 1209.3Hz", mute: "0", level: "-3.0" },
    { tuner: "C-1 +1.0c 8.2Hz", mute: "1", level: "abc" },
    { tuner: "E2 +1.0c 82.5Hz", mute: null, level: null },
];
let pageOverflow = 0, pageThrew = 0, pagesDrawn = 0;
for (let w = 40; w <= 128; w += 4) {
    for (let h = 20; h <= 52; h += 1) {
        for (const values of PAGES) {
            for (const nowMs of [0, 400, 800]) {
                const t = makeCtx(w, h);
                try { ov.drawPage(t.ctx, { values, nowMs }); }
                catch (e) { pageThrew++; if (pageThrew < 3) console.log("    threw:", w, h, JSON.stringify(values), e.message); }
                if (t.clipped()) { pageOverflow++; if (pageOverflow < 4) console.log("    overflow:", w, h, JSON.stringify(values)); }
                pagesDrawn++;
            }
        }
    }
}
check(pageThrew === 0, `drawPage never throws (${pageThrew} of ${pagesDrawn})`);
check(pageOverflow === 0, `drawPage stays inside its band (${pageOverflow} of ${pagesDrawn} overflowed)`);
threw = false;
try { const t = makeCtx(10, 10); ov.drawPage(t.ctx, {}); ov.drawPage(makeCtx(128, 48).ctx, undefined); } catch (e) { threw = true; }
check(!threw, "drawPage tolerates a tiny band and a missing payload");

/* What the page says, on the device's 128x48 band. */
function pageText(values) {
    const t = makeCtx(128, 48);
    ov.drawPage(t.ctx, { values, nowMs: 0 });
    return t.printed();
}
check(pageText({ tuner: "E2 -4.6c 82.2Hz", mute: "1" }).includes("MUTED"), "muted says MUTED");
check(!pageText({ tuner: "E2 -4.6c 82.2Hz", mute: "0" }).includes("MUTED"), "through says nothing");
check(pageText({ tuner: "--", mute: "1" }).includes("MUTED"), "muted says MUTED with no note too");
check(pageText({ tuner: "E2 -4.6c 82.2Hz" }).includes("-5c"), "shows the cents");
check(pageText({ tuner: "E2 -4.6c 82.2Hz" }).includes("82.2 Hz"), "shows the frequency");
check(pageText({ tuner: "--" }).some((s) => s.startsWith("Listening")), "no note says Listening");
check(pageText({ tuner: "D#6 -49.9c 1209.3Hz", mute: "1" }).some((s) => /^1209\.3 ?Hz$|^1209Hz$/.test(s)),
      "a four-digit frequency still fits");

/* More input, more of the level ladder lit. */
function pageLit(values) { const t = makeCtx(128, 48); ov.drawPage(t.ctx, { values }); return t.lit(); }
check(pageLit({ tuner: "--", level: "-6.0" }) > pageLit({ tuner: "--", level: "-40.0" }) &&
      pageLit({ tuner: "--", level: "-40.0" }) > pageLit({ tuner: "--", level: "-60.0" }),
      "the level ladder grows with the input");

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
