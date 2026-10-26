/*
 * Guitar Tuner FX -- the tuner cell (a knob-grid widget) and the Tuner page.
 *
 * Both draw from ONE value, `tuner`, which the DSP serves as
 *
 *     "<note><octave> <signed cents>c <frequency>Hz"   e.g. "E2 +3.4c 82.6Hz"
 *     "--"                                                  (no note)
 *
 * with the note already spelled in the chosen notation (sharps or flats).
 * The DSP holds a reading for 1.5 s after the signal drops and clears it when
 * it stops being run, so nothing here keeps state between frames. The page
 * also reads `mute` (to say MUTED) and `level` (the input meter).
 *
 * The rules this file lives by (Schwung's custom-widget contract):
 *   - (0,0) is the top-left of the frame we are handed and ctx.width/height
 *     are its size; everything is sized from those, never from constants.
 *   - We are given values and cannot read any. A value that did not arrive is
 *     null/undefined, and draws as "--" rather than as a zero.
 *   - If a drawer throws it is disabled for the session and the host draws a
 *     plain cell instead, so the page stays usable.
 */

(function () {
    "use strict";

    var GLYPH_H = 7;        /* the device font's cap height */
    var RANGE_CENTS = 50;   /* the needle's full deflection either way */
    var LOCK_CENTS = 3;     /* closer than this is in tune */
    var TICKS_CENTS = [10, 25];
    var LEVEL_FLOOR_DB = -60;

    /* "E2 +3.4c 82.6Hz" -> { letter: "E", acc: "", octave: "2", cents: 3.4,
     * hz: 82.6 }. Anything else -- "--", null, a partial read -- is null. */
    function parseReading(raw) {
        if (raw === null || raw === undefined) return null;
        var m = /^\s*([A-G])([#b]?)(-?\d+)\s+([+-]?\d+(?:\.\d+)?)c\s+(\d+(?:\.\d+)?)Hz\s*$/
            .exec(String(raw));
        if (!m) return null;
        return {
            letter: m[1], acc: m[2], octave: m[3],
            cents: Number(m[4]), hz: Number(m[5]),
        };
    }

    function num(v, fallback) {
        if (v === null || v === undefined || v === "") return fallback;
        var n = Number(v);
        return isFinite(n) ? n : fallback;
    }

    function isOn(v) { return num(v, 0) >= 0.5; }

    function centsText(r) {
        var c = Math.round(r.cents);
        return (c > 0 ? "+" : c < 0 ? "-" : "±") .replace("±", "") +
            Math.abs(c) + "c";
    }

    /* ------------------------------------------------ the big note font ---
     * 5x7, one string per row, drawn at an integer scale with fillRect, so
     * the note stays legible from a guitar's length away. Only what a note
     * name needs: A-G, the two accidentals, digits and a minus for octave -1.
     */
    var BIG = {
        A: ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
        B: ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
        C: ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
        D: ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
        E: ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
        F: ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
        G: ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
        "#": ["01010", "01010", "11111", "01010", "11111", "01010", "01010"],
        b: ["10000", "10000", "10110", "11001", "10001", "10001", "11110"],
        "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
        "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
        "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
        "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
        "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
        "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
        "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
        "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
        "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
        "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
        "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    };
    var BIG_W = 5, BIG_H = 7;

    /* Width of `s` at scale `k`, with one scaled column between glyphs. */
    function bigWidth(s, k) {
        return s.length ? s.length * (BIG_W + 1) * k - k : 0;
    }

    function bigPrint(ctx, x, y, s, k, color) {
        for (var n = 0; n < s.length; n++) {
            var g = BIG[s.charAt(n)];
            if (g) {
                for (var row = 0; row < BIG_H; row++) {
                    /* One rect per run of lit pixels in the row. */
                    var bits = g[row], col = 0;
                    while (col < BIG_W) {
                        if (bits.charAt(col) !== "1") { col++; continue; }
                        var start = col;
                        while (col < BIG_W && bits.charAt(col) === "1") col++;
                        ctx.fillRect(x + start * k, y + row * k, (col - start) * k, k, color);
                    }
                }
            }
            x += (BIG_W + 1) * k;
        }
    }

    function outline(ctx, x, y, w, h) {
        ctx.fillRect(x, y, w, 1, 1);
        ctx.fillRect(x, y + h - 1, w, 1, 1);
        ctx.fillRect(x, y, 1, h, 1);
        ctx.fillRect(x + w - 1, y, 1, h, 1);
    }

    /*
     * The needle meter: a frame with a centre mark and ticks at 10 and 25
     * cents, and the needle at the reading (pinned at the end past 50). In
     * tune, the whole centre window fills -- the one picture you can read
     * without looking for the needle.
     */
    function drawMeter(ctx, x, y, w, h, r) {
        if (w < 7 || h < 3) return;
        outline(ctx, x, y, w, h);
        var inner = w - 2;
        var half = Math.floor((inner - 1) / 2);
        var cx = x + 1 + half;
        var locked = r && Math.abs(r.cents) < LOCK_CENTS;

        /* Ticks: notches hanging from the top inner row and standing on the
         * bottom one, so a needle sitting on one still shows it. */
        for (var i = 0; i < TICKS_CENTS.length; i++) {
            var off = Math.round(TICKS_CENTS[i] / RANGE_CENTS * half);
            if (off <= 1 || off >= half) continue;
            var th = h >= 9 ? 2 : 1;
            ctx.fillRect(cx - off, y + 1, 1, th, 1);
            ctx.fillRect(cx + off, y + 1, 1, th, 1);
            ctx.fillRect(cx - off, y + h - 1 - th, 1, th, 1);
            ctx.fillRect(cx + off, y + h - 1 - th, 1, th, 1);
        }

        var win = Math.max(1, Math.round(LOCK_CENTS / RANGE_CENTS * half));
        if (locked) {
            ctx.fillRect(cx - win, y + 1, 2 * win + 1, h - 2, 1);
            return;
        }
        /* The centre mark: a full-height line through the frame. */
        ctx.fillRect(cx, y, 1, h, 1);
        if (!r) return;

        var c = Math.max(-RANGE_CENTS, Math.min(RANGE_CENTS, r.cents));
        var nx = cx + Math.round(c / RANGE_CENTS * half);
        var nw = w >= 40 ? 3 : 1;
        var left = Math.max(x + 1, Math.min(x + w - 1 - nw, nx - (nw >> 1)));
        ctx.fillRect(left, y + 1, nw, h - 2, 1);
    }

    /*
     * The grid cell: the note above a small needle meter. Too short for both
     * and the note goes; too narrow for the note and only the meter stays.
     */
    function drawCell(ctx, o) {
        var group = o && o.group;
        var values = o && o.values;
        var key = group && group.keys && group.keys[0];
        var r = parseReading(key && values ? values[key] : null);

        var w = ctx.width, h = ctx.height;
        if (w < 7 || h < 3) return;

        var meterH = Math.max(3, Math.min(7, Math.round(h / 3)));
        var showText = h >= GLYPH_H + 2 + meterH;
        if (!showText) meterH = Math.min(h, 7);
        if (showText) {
            var s = r ? r.letter + r.acc + r.octave : "--";
            var tw = ctx.textWidth(s);
            if (tw <= w) ctx.print(Math.floor((w - tw) / 2), h - meterH - 2 - GLYPH_H, s, 1);
        }
        /* Inset a little, so the frame doesn't run into the next cell's. */
        var inset = w >= 24 ? 2 : 0;
        drawMeter(ctx, inset, h - meterH, w - 2 * inset, meterH, r);
    }

    /* The input meter: a ladder of segments filling up from the bottom.
     * Segmented rather than a solid bar, so it cannot be mistaken for a
     * scrollbar down the edge of the page. An unlit segment is a dot. */
    function drawLevel(ctx, x, y, w, h, db) {
        var seg = 3, gap = 1;
        var n = Math.floor((h + gap) / (seg + gap));
        if (n < 2) return;
        var f = db === null ? 0 : (db - LEVEL_FLOOR_DB) / -LEVEL_FLOOR_DB;
        var lit = Math.round(n * Math.max(0, Math.min(1, f)));
        var bottom = y + h;
        for (var i = 0; i < n; i++) {
            var sy = bottom - (i + 1) * seg - i * gap;
            if (i < lit) ctx.fillRect(x, sy, w, seg, 1);
            else ctx.fillRect(x + (w >> 1), sy + 1, 1, 1, 1);
        }
    }

    /*
     * Where the parts of a note go at letter scale k. The accidental sits up
     * like a superscript and the octave down like a subscript; stacked in
     * one column they would touch at the larger sizes, so with an accidental
     * the octave then steps right of it.
     */
    function noteLayout(r, k, nx) {
        var ks = Math.max(1, Math.floor(k / 2));
        var h = BIG_H * k;
        var sx = nx + bigWidth(r.letter, k) + k;
        var ox = sx;
        if (r.acc && 2 * BIG_H * ks >= h) ox = sx + bigWidth(r.acc, ks) + ks;
        var right = Math.max(r.acc ? sx + bigWidth(r.acc, ks) : 0, ox + bigWidth(r.octave, ks));
        return { ks: ks, h: h, sx: sx, ox: ox, right: right };
    }

    /* Text in a filled box, drawn in the opposite colour: the MUTED tag. */
    function tag(ctx, x, y, s) {
        var tw = ctx.textWidth(s);
        ctx.fillRect(x, y, tw + 4, GLYPH_H + 3, 1);
        ctx.print(x + 2, y + 2, s, 0);
    }

    /*
     * The Tuner page: the note as large as the band allows, the cents and
     * frequency beside it, the needle meter along the bottom, the input
     * meter down the right edge, and MUTED when the output is muted. The
     * three knobs work here exactly as on the grid page.
     */
    function drawPage(ctx, o) {
        var values = (o && o.values) || {};
        var w = ctx.width, h = ctx.height;
        if (w < 40 || h < 20) return;

        var r = parseReading(values.tuner);
        var muted = isOn(values.mute);
        var lvl = num(values.level, null);

        /* ---- right edge: the input level ---- */
        var lvW = 3;
        drawLevel(ctx, w - lvW, 0, lvW, h, lvl);
        var areaW = w - lvW - 3;

        /* ---- bottom: the needle ---- */
        var meterH = h >= 36 ? 9 : 7;
        var meterY = h - meterH;
        drawMeter(ctx, 0, meterY, areaW, meterH, r);
        var topH = meterY - 2;

        /* ---- MUTED, top right of the reading area ---- */
        if (muted) {
            var words = ["MUTED", "MUTE", "M"];
            for (var m = 0; m < words.length; m++) {
                var mw = ctx.textWidth(words[m]) + 4;
                if (mw <= areaW && GLYPH_H + 3 <= topH) { tag(ctx, areaW - mw, 0, words[m]); break; }
            }
        }

        if (!r) {
            var dots = ".".repeat(1 + Math.floor(num(o && o.nowMs, 0) / 400) % 3);
            var msg = "Listening" + dots;
            if (2 + ctx.textWidth(msg) > areaW) msg = "...".slice(0, dots.length);
            var ly = Math.max(0, Math.floor((topH - GLYPH_H) / 2));
            if (muted && ly < GLYPH_H + 4) ly = GLYPH_H + 4;
            if (ly + GLYPH_H <= topH && 2 + ctx.textWidth(msg) <= areaW) ctx.print(2, ly, msg, 1);
            return;
        }

        /* ---- the note: letter big, accidental and octave at half size ---- */
        var nx = 2;
        var k = Math.max(1, Math.min(4, Math.floor(topH / BIG_H)));
        var L = noteLayout(r, k, nx);
        while (k > 1 && L.right > areaW) L = noteLayout(r, --k, nx);
        if (L.right > areaW) return;   /* not even the smallest note fits */
        var ny = Math.max(0, Math.floor((topH - L.h) / 2));
        bigPrint(ctx, nx, ny, r.letter, k, 1);
        if (r.acc) bigPrint(ctx, L.sx, ny, r.acc, L.ks, 1);
        bigPrint(ctx, L.ox, ny + L.h - BIG_H * L.ks, r.octave, L.ks, 1);
        var noteRight = L.right;

        /* ---- cents and frequency, right-aligned in what is left ---- */
        var cents = centsText(r);
        var hz = r.hz.toFixed(1) + " Hz";
        if (ctx.textWidth(hz) > areaW - noteRight - 4) hz = r.hz.toFixed(0) + "Hz";
        var pitch = GLYPH_H + 3;
        var blockH = 2 * pitch - 3;
        var by = Math.max(muted ? GLYPH_H + 5 : 0, Math.floor((topH - blockH) / 2));
        if (by + blockH > topH) by = Math.max(0, topH - blockH);
        var lines = [cents, hz];
        for (var i = 0; i < lines.length; i++) {
            var tw = ctx.textWidth(lines[i]);
            var tx = areaW - tw - 1;
            if (tx > noteRight + 3 && by + i * pitch + GLYPH_H <= topH) {
                ctx.print(tx, by + i * pitch, lines[i], 1);
            }
        }
    }

    globalThis.canvas_overlay = {
        /* Must match chain_params: "viz": { "kind": "custom:tuner" } */
        widgetKind: "custom:tuner",
        drawCell: drawCell,
        drawPage: drawPage,
        /* Exposed for the offline tests; the host ignores unknown fields. */
        _test: { parseReading: parseReading, centsText: centsText, BIG: BIG },
    };
})();
