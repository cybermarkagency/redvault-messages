// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces studio_vectors.json with the web app's own studio code (lib/studio/captions.ts, audioDsp.ts, imageEdit.ts), so the
// Android app's subtitle, voice and photo-editing maths are pinned to it. Not part of any build: copy it into ui/src/test/ as
// makeAndroidStudioVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeAndroidStudioVectors.test.ts
//
// and delete the copy. The output lands next to this file (android/app/src/test/resources/vectors/).
//
// Everything is deterministic. Sound is stored as raw 32-bit floats (little endian, base64) so the Android test starts from the very same
// samples; results are stored the same way and compared with a small tolerance (JavaScript and the JVM may differ in the last bit of a sine).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as dsp from "../lib/studio/audioDsp.js";
import * as cap from "../lib/studio/captions.js";
import * as img from "../lib/studio/imageEdit.js";
import * as col from "../lib/studio/collage.js";
import * as tpl from "../lib/studio/textTemplates.js";

const OUT = resolve(process.cwd(), "../android/app/src/test/resources/vectors/studio_vectors.json");

const f32 = (a: Float32Array) => Buffer.from(a.buffer, a.byteOffset, a.byteLength).toString("base64");
const mono = (m: dsp.Mono) => ({ rate: m.sampleRate, samples: f32(m.samples) });
const u8 = (a: Uint8Array | Uint8ClampedArray) => Buffer.from(a.buffer, a.byteOffset, a.byteLength).toString("base64");

function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolveBlob, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolveBlob(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

/** A made-up voice: two tones that fade out, with a little noise. Fixed, so the sound is the same every run. */
function signal(rate: number, ms: number): dsp.Mono {
  const n = Math.round((rate * ms) / 1000);
  const s = new Float32Array(n);
  let seed = 12345;
  for (let i = 0; i < n; i++) {
    seed = (seed * 48271) % 2147483647;
    const noise = (seed / 2147483647 - 0.5) * 0.1;
    const env = Math.exp(-i / (n * 0.6));
    s[i] = (0.5 * Math.sin((2 * Math.PI * 220 * i) / rate) + 0.25 * Math.sin((2 * Math.PI * 660 * i) / rate) + noise) * env;
  }
  return { samples: s, sampleRate: rate };
}

it("writes the vectors", async () => {
  // --- captions ---------------------------------------------------------------------------------------------------------
  const clockTexts = ["83.4", "1:23.4", "0:01:23,400", "00:01:23.4", " 5 ", "", "abc", "1:2:3:4", "-1", "1e3", "600", "600.001", "10:00", "10:00.001", "0:0:0.5", "1,5", "1:60", "0", "00:00:00,000", "5.", ".5", "1::2", "1:", ":1", "01:02:03", "9:59.9995", "0.0004", "0.0005", "12345678"];
  const clockMs = Array.from({ length: 600 }, (_, i) => i * 37).concat([0, 1, 49, 50, 51, 59_949, 59_950, 59_999, 60_000, 60_049, 119_950, 600_000, 1_234_567, 59_950 + 60_000]);
  const srtTexts = [
    "1\n00:00:01,000 --> 00:00:03,500\nHello there\n\n2\n00:00:04,000 --> 00:00:06,000\nSecond line\nwraps over two\n\n3\n00:00:07,000 --> 00:00:08,000\n<i>Markup</i> {\\an8}is dropped",
    "1\r\n00:00:01,000 --> 00:00:02,000\r\nWindows line ends\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000\r\nStill fine\r\n",
    "00:00:05,000 --> 00:00:06,000 position:50%\nno number line, a setting after the time",
    "1\n00:00:09,000 --> 00:00:08,000\nends before it starts\n\n2\n00:00:00,000 --> 00:00:00,000\nzero length\n\n3\n00:00:01,000 --> 00:00:02,000\n\n\n4\n00:00:02,000 --> 00:00:03,000\nkept",
    "1\nnot a time --> 00:00:02,000\nbad start\n\n2\n00:00:01,000 --> nope\nbad end\n\n3\n00:00:01,000\nno arrow",
    "1\n00:10:00,000 --> 00:10:00,001\nover ten minutes\n\n2\n00:09:59,000 --> 00:10:00,000\nexactly ten minutes",
    "2\n00:00:20,000 --> 00:00:21,000\nlater first\n\n1\n00:00:10,000 --> 00:00:11,000\nearlier second",
    "1\n00:00:01,000 --> 00:00:02,000\n" + "x".repeat(121) + "\n\n2\n00:00:03,000 --> 00:00:04,000\n" + "y".repeat(120),
    Array.from({ length: 70 }, (_, i) => `${i + 1}\n00:00:${String(i % 60).padStart(2, "0")},000 --> 00:00:${String(i % 60).padStart(2, "0")},500\nline ${i}`).join("\n\n"),
    "",
    "just words\nand nothing else",
    "1\n0:01.5 --> 0:03\nshort clocks\n\n2\n1:02:03.456 --> 1:02:04\nhour clocks",
  ];
  const sampleCaptions = [
    { startMs: 0, endMs: 1500, text: "first" },
    { startMs: 1500, endMs: 3000, text: "second line" },
    { startMs: 3_600_000 > 600_000 ? 500_000 : 0, endMs: 600_000, text: "late one" },
  ];
  const evenCases: [string[], number][] = [
    [["one", "two", "three"], 9000],
    [["  one  ", "", "two"], 1000],
    [[], 5000],
    [["only"], 0],
    [["a", "b", "c", "d", "e", "f", "g"], 7],
    [["x".repeat(200), "short"], 4000],
    [Array.from({ length: 80 }, (_, i) => `line ${i}`), 120_000],
    [["tail rounds"], 1001],
  ];

  // --- sound --------------------------------------------------------------------------------------------------------------
  const A = signal(44100, 120);
  const B = signal(22050, 160);
  const silent: dsp.Mono = { samples: new Float32Array(2000), sampleRate: 22050 };
  const tiny: dsp.Mono = { samples: Float32Array.from([0.1, -0.2, 0.3, -0.4, 0.5]), sampleRate: 8000 };
  const empty: dsp.Mono = { samples: new Float32Array(0), sampleRate: 22050 };
  const named: Record<string, dsp.Mono> = { A, B, silent, tiny, empty };
  const resamples = [
    ["A", 22050], ["A", 16000], ["A", 44100], ["B", 22050], ["B", 8000], ["B", 44100], ["tiny", 16000], ["tiny", 3000], ["empty", 8000], ["silent", 11025],
  ] as const;
  const trims = [
    ["A", 0, 100], ["A", 50, 250], ["A", 250, 1000], ["A", 100, 100], ["A", -10, 20], ["A", 400, 500], ["B", 0, 400], ["B", 123, 321], ["empty", 0, 10],
  ] as const;
  const fades = [["A", 40, 120], ["A", 0, 0], ["A", 1000, 1000], ["B", 10, 300], ["tiny", 100, 100], ["empty", 40, 120]] as const;
  const norms = [["A", undefined], ["B", 0.5], ["silent", undefined], ["tiny", 0.95], ["empty", 0.5]] as const;
  const gains = [["A", 0.5], ["A", 3], ["A", 0], ["B", -1]] as const;
  const bars = [["A", 48], ["A", 16], ["A", 96], ["B", 48], ["tiny", 48], ["empty", 48], ["silent", 48], ["B", 7]] as const;
  const wavSource = dsp.trim(B, 0, 60);
  const wav = await readBlob(dsp.encodeWav(wavSource));
  const finishes = [] as { input: string; startMs: number; endMs: number; filter: string; durationMs: number; waveform: number[]; wav: string }[];
  const finishRuns: [("A" | "B"), dsp.VoiceFilterId][] = [...dsp.VOICE_FILTERS.map((f) => ["B", f] as ["B", dsp.VoiceFilterId]), ["A", "none"], ["A", "radio"], ["A", "deep"]];
  for (const [input, filter] of finishRuns) {
    {
      const done = dsp.finishVoice(named[input], { startMs: 20, endMs: input === "A" ? 110 : 150, filter });
      finishes.push({ input, startMs: 20, endMs: input === "A" ? 110 : 150, filter, durationMs: done.durationMs, waveform: done.waveform, wav: u8(await readBlob(done.blob)) });
    }
  }

  // --- photo editing ---------------------------------------------------------------------------------------------------------
  const rects: img.Rect[] = [
    { x: 0.1, y: 0.1, w: 0.5, h: 0.5 },
    { x: -0.2, y: 1.4, w: 2, h: 0.01 },
    { x: 0.9, y: 0.9, w: 0.5, h: 0.5 },
    { x: 0, y: 0, w: 1, h: 1 },
    { x: 0.5, y: 0.5, w: 0.08, h: 0.08 },
  ];
  const aspects: [number | null, number, number][] = [[1, 100, 50], [null, 100, 50], [9 / 16, 1920, 1080], [1, 100, 100], [0.5, 40, 80], [16 / 9, 100, 200], [4 / 5, 1000, 1000], [0, 10, 10], [-1, 10, 10], [1, 0, 10], [3 / 4, 3000, 2000]];
  const handles: img.Handle[] = ["move", "nw", "ne", "sw", "se", "n", "s", "e", "w"];
  const base: img.Rect = { x: 0.2, y: 0.3, w: 0.4, h: 0.4 };
  const drags = handles.flatMap((h) => [[0.1, 0.05], [-0.5, -0.5], [0.9, 0.9], [-0.05, 0.3]].map(([dx, dy]) => ({ rect: base, handle: h, dx, dy, result: img.dragRect(base, h, dx, dy) })));
  const states: img.EditState[] = [
    img.NEUTRAL_EDIT,
    { ...img.NEUTRAL_EDIT, rotation: 90 },
    { ...img.NEUTRAL_EDIT, rotation: 270, crop: { x: 0.25, y: 0.1, w: 0.5, h: 0.8 } },
    { ...img.NEUTRAL_EDIT, flipX: true, crop: { x: 0, y: 0, w: 0.333, h: 0.777 } },
    { ...img.NEUTRAL_EDIT, rotation: 180, crop: { x: 0.1, y: 0.2, w: 0.3, h: 0.4 }, filter: "warm", adjust: { brightness: 40, contrast: -20, saturation: 60, blur: 30 } },
    { ...img.NEUTRAL_EDIT, filter: "noir" },
    { ...img.NEUTRAL_EDIT, adjust: { brightness: 100, contrast: 100, saturation: -100, blur: 100 } },
    { ...img.NEUTRAL_EDIT, adjust: { brightness: -250, contrast: 250, saturation: 0, blur: -5 } },
    { ...img.NEUTRAL_EDIT, strokes: [img.newStroke("red", 0.02, false, { x: 0.5, y: 0.5 })] },
  ];
  const colorStates = [
    ...img.FILTERS.map((f) => ({ ...img.NEUTRAL_EDIT, filter: f })),
    { ...img.NEUTRAL_EDIT, adjust: { brightness: 40, contrast: -20, saturation: 60, blur: 30 } },
    { ...img.NEUTRAL_EDIT, filter: "vivid" as const, adjust: { brightness: -60, contrast: 90, saturation: -40, blur: 100 } },
    { ...img.NEUTRAL_EDIT, adjust: { brightness: 300, contrast: -300, saturation: 300, blur: 300 } },
  ];
  // An 8 by 4 picture of made-up colours, so the pixel maths is compared on all channels.
  const pixels = new Uint8ClampedArray(8 * 4 * 4);
  for (let i = 0; i < 32; i++) {
    pixels[i * 4] = (i * 53 + 7) & 255;
    pixels[i * 4 + 1] = (i * 97 + 31) & 255;
    pixels[i * 4 + 2] = (i * 29 + 200) & 255;
    pixels[i * 4 + 3] = i % 5 === 0 ? 128 : 255;
  }
  const pixelRuns = colorStates.map((s) => {
    const params = img.colorParams(s, 800);
    const data = Uint8ClampedArray.from(pixels);
    img.adjustPixels(data, params);
    return { state: s, params, result: u8(data) };
  });
  // Strokes: the points are the web's: close points are skipped, everything is kept in the picture, and a stroke stops at its limit.
  let stroke = img.newStroke("blue", 0.2, false, { x: -1, y: 2 });
  const strokeSteps: { point: { x: number; y: number }; stroke: img.Stroke }[] = [];
  for (const p of [{ x: 0.0001, y: 1 }, { x: 0.5, y: 0.5 }, { x: 0.5005, y: 0.5005 }, { x: 0.52, y: 0.5 }, { x: 3, y: -3 }, { x: 0.9, y: 0.1 }]) {
    stroke = img.extendStroke(stroke, p);
    strokeSteps.push({ point: p, stroke });
  }
  let long = img.newStroke("white", 0.001, true, { x: 0, y: 0 });
  for (let i = 1; i < 2100; i++) long = img.extendStroke(long, { x: (i % 700) / 700, y: Math.floor(i / 700) / 3 });
  let manyStrokes = img.NEUTRAL_EDIT;
  for (let i = 0; i < 410; i++) manyStrokes = img.addStroke(manyStrokes, img.newStroke("green", 0.01, i % 2 === 0, { x: (i % 10) / 10, y: 0.5 }));

  // --- collage and templates -------------------------------------------------------------------------------------------------
  const collageSizes: [number, number][] = [[1080, 1920], [360, 640], [100, 50], [3, 2], [1, 1]];
  const collageGaps = [0, 12, 40, 3.5, 4];
  const collageRects = col.COLLAGE_LAYOUTS.flatMap((layout) => collageSizes.flatMap(([w, h]) => collageGaps.map((gap) => ({ layout: layout.id, w, h, gap, result: col.cellRects(layout, w, h, gap) }))));
  const coverCases: [number, number, number, number][] = [[1000, 500, 100, 100], [500, 1000, 100, 100], [100, 100, 100, 100], [4000, 3000, 540, 960], [3000, 4000, 1080, 640], [1, 1, 1, 1], [1920, 1080, 0.5, 100], [1080, 1920, 1068, 1908], [640, 480, 534, 1908]];
  const collage = {
    max: col.MAX_COLLAGE_PHOTOS,
    layouts: col.COLLAGE_LAYOUTS,
    layoutsFor: [0, 1, 2, 3, 4, 5].map((count) => ({ count, ids: col.layoutsFor(count).map((l) => l.id) })),
    cellRects: collageRects,
    fitCover: coverCases.map(([iw, ih, cw, ch]) => ({ imageW: iw, imageH: ih, cellW: cw, cellH: ch, result: col.fitCover(iw, ih, cw, ch) })),
  };
  // A template makes its stickers with a random id and a "a week from now" time: fix both so the output is the same every run.
  const realNow = Date.now;
  Date.now = () => 1_800_000_000_000;
  const templates = tpl.TEXT_TEMPLATES.map((t) => {
    const a = tpl.applyTemplate(t);
    return { id: t.id, label: t.label, text: a.text, background: a.background, font: a.font, overlays: a.overlays, stickers: a.stickers.map((s) => ({ ...s, id: "fixedid00000" })) };
  });
  Date.now = realNow;

  const out = {
    note: "Made by android/app/src/test/resources/vectors/make_android_studio_vectors.ts with the web app's own studio code.",
    captions: {
      parseClock: clockTexts.map((t) => ({ text: t, ms: cap.parseClock(t) })),
      formatClock: clockMs.map((ms) => ({ ms, text: cap.formatClock(ms) })),
      parseSrt: srtTexts.map((t) => ({ srt: t, captions: cap.parseSrt(t) })),
      toSrt: { captions: sampleCaptions, srt: cap.toSrt(sampleCaptions) },
      evenCaptions: evenCases.map(([lines, ms]) => ({ lines, durationMs: ms, captions: cap.evenCaptions(lines, ms) })),
    },
    audio: {
      inputs: Object.fromEntries(Object.entries(named).map(([k, m]) => [k, mono(m)])),
      resample: resamples.map(([k, rate]) => ({ input: k, rate, result: mono(dsp.resample(named[k], rate)) })),
      trim: trims.map(([k, a, b]) => ({ input: k, startMs: a, endMs: b, result: mono(dsp.trim(named[k], a, b)) })),
      fade: fades.map(([k, a, b]) => ({ input: k, inMs: a, outMs: b, result: mono(dsp.fade(named[k], a, b)) })),
      normalize: norms.map(([k, t]) => ({ input: k, target: t ?? null, result: mono(t === undefined ? dsp.normalize(named[k]) : dsp.normalize(named[k], t)) })),
      gain: gains.map(([k, g]) => ({ input: k, factor: g, result: mono(dsp.gain(named[k], g)) })),
      // Every voice on the 22 kHz sound; the 44 kHz one only where the rate matters (a pitch change, the radio's band limits).
      filters: [...dsp.VOICE_FILTERS.map((f) => ["B", f] as const), ["A", "chipmunk"] as const, ["A", "radio"] as const].map(([k, f]) => ({ input: k, filter: f, result: mono(dsp.applyVoiceFilter(named[k], f)) })),
      waveform: bars.map(([k, n]) => ({ input: k, bars: n, result: dsp.waveformBars(named[k].samples, n) })),
      duration: Object.entries(named).map(([k, m]) => ({ input: k, ms: dsp.durationMs(m) })),
      mixToMono: { channels: [f32(Float32Array.from([1, 0.5, -1])), f32(Float32Array.from([0, 0.5, 1, 0.25]))], rate: 8000, result: mono(dsp.mixToMono([Float32Array.from([1, 0.5, -1]), Float32Array.from([0, 0.5, 1, 0.25])], 8000)) },
      wav: { source: mono(wavSource), bytes: u8(wav), decoded: (() => { const d = dsp.decodeWav(wav)!; return mono(d); })() },
      notAWav: [dsp.decodeWav(new Uint8Array(10)), dsp.decodeWav(Uint8Array.from(wav.slice(0, 43)))].map((x) => x === null),
      finish: finishes,
    },
    image: {
      rotatedSize: [0, 90, 180, 270].map((r) => ({ rotation: r, w: 100, h: 50, result: img.rotatedSize(100, 50, r as img.Rotation) })),
      rotateBy: ([0, 90, 180, 270] as img.Rotation[]).flatMap((r) => [-5, -1, 0, 1, 2, 3, 7].map((q) => ({ rotation: r, quarterTurns: q, result: img.rotateBy(r, q) }))),
      clampRect: rects.map((r) => ({ rect: r, result: img.clampRect(r) })),
      aspectCrop: aspects.map(([a, w, h]) => ({ aspect: a, w, h, result: img.aspectCrop(a, w, h) })),
      dragRect: drags,
      cropPixels: states.map((s) => ({ state: s, w: 1000, h: 750, result: img.cropPixels(1000, 750, s) })),
      outputSize: states.flatMap((s) => [1920, 500].map((m) => ({ state: s, w: 3000, h: 2000, maxSide: m, result: img.outputSize(3000, 2000, s, m) }))),
      colorParams: colorStates.map((s) => ({ state: s, outputHeight: 800, result: img.colorParams(s, 800) })),
      pixels: { width: 8, height: 4, rgba: u8(pixels), runs: pixelRuns },
      hasEdits: states.map((s) => ({ state: s, result: img.hasEdits(s) })),
      strokeSteps,
      longStroke: { points: long.points.length, first: long.points[0], last: long.points[long.points.length - 1] },
      manyStrokes: { count: manyStrokes.strokes.length, firstEraser: manyStrokes.strokes[0].eraser, lastEraser: manyStrokes.strokes[manyStrokes.strokes.length - 1].eraser },
      newStroke: [[0.0001, "red", false], [5, "black", true], [-1, "pink", false]].map(([size, color, eraser]) => ({ size, color, eraser, result: img.newStroke(color as never, size as number, eraser as boolean, { x: 0.3, y: 0.7 }) })),
      withGeometry: { before: { ...img.NEUTRAL_EDIT, strokes: [img.newStroke("red", 0.02, false, { x: 0.5, y: 0.5 })], rotation: 90 }, change: { rotation: 180, flipX: true }, result: img.withGeometry({ ...img.NEUTRAL_EDIT, strokes: [img.newStroke("red", 0.02, false, { x: 0.5, y: 0.5 })], rotation: 90 }, { rotation: 180, flipX: true }) },
      constants: { minCrop: img.MIN_CROP, minPen: img.MIN_PEN, maxPen: img.MAX_PEN, maxStrokes: img.MAX_STROKES, maxPoints: img.MAX_POINTS_PER_STROKE, filters: img.FILTERS, penColors: img.PEN_COLORS },
    },
    collage,
    templates,
    /** The time the templates were made at (Date.now), so a countdown's end can be compared. */
    templatesMadeAt: 1_800_000_000_000,
  };
  expect(out.audio.finish).toHaveLength(9);
  writeFileSync(OUT, JSON.stringify(out) + "\n");
});
