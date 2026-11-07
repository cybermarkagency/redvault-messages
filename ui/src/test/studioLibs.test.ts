import { afterEach, beforeAll, describe, expect, it, vi } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  addStroke,
  adjustPixels,
  aspectCrop,
  clampRect,
  colorParams,
  cropPixels,
  dragRect,
  extendStroke,
  filterCss,
  hasEdits,
  MAX_STROKES,
  MIN_CROP,
  NEUTRAL_EDIT,
  newStroke,
  outputSize,
  rotateBy,
  rotatedSize,
  withGeometry,
  type EditState,
  type Rect,
} from "../lib/studio/imageEdit.js";
import { canUndo, canRedo, pushHistory, redo, startHistory, undo } from "../lib/studio/history.js";
import { cellRects, COLLAGE_LAYOUTS, drawCollage, fitCover, layoutsFor, type CollageImage } from "../lib/studio/collage.js";
import { applyTemplate, TEXT_TEMPLATES } from "../lib/studio/textTemplates.js";
import {
  applyVoiceFilter,
  decodeWav,
  durationMs,
  encodeWav,
  fade,
  finishVoice,
  gain,
  mixToMono,
  normalize,
  peak,
  resample,
  trim,
  VOICE_FILTERS,
  waveformBars,
  type Mono,
} from "../lib/studio/audioDsp.js";
import { evenCaptions, formatClock, parseClock, parseSrt, toSrt } from "../lib/studio/captions.js";
import { applyZoom, CameraUnavailableError, captureFrame, countdownSteps, describeCameraError, openCamera, otherFacing, readCaps, setTorch, shouldMirror } from "../lib/studio/camera.js";
import { pickRecorderMime, StreamRecorder } from "../lib/studio/recorder.js";
import { captionAt, MAX_CAPTION_END_MS, parseCaptions, parseStickers, type StorySticker } from "../lib/storyCrypto.js";

beforeAll(async () => {
  await crypto.ready();
});

describe("undo and redo", () => {
  it("steps back and forward, and a new step drops what could have been redone", () => {
    let h = startHistory(0);
    expect(canUndo(h)).toBe(false);
    h = pushHistory(h, 1);
    h = pushHistory(h, 2);
    expect(h.present).toBe(2);
    h = undo(h);
    expect(h.present).toBe(1);
    expect(canRedo(h)).toBe(true);
    h = redo(h);
    expect(h.present).toBe(2);
    h = undo(undo(h));
    expect(h.present).toBe(0);
    h = pushHistory(h, 9);
    expect(canRedo(h)).toBe(false);
    expect(undo(h).present).toBe(0);
    expect(undo(undo(h)).present).toBe(0);
  });

  it("an unchanged value is not a step, and history is bounded", () => {
    const h = startHistory({ a: 1 });
    expect(pushHistory(h, h.present)).toBe(h);
    let long = startHistory(0);
    for (let i = 1; i <= 100; i++) long = pushHistory(long, i, 10);
    expect(long.past).toHaveLength(10);
    expect(long.present).toBe(100);
  });
});

describe("photo editing maths", () => {
  it("turns by quarters and swaps the size", () => {
    expect(rotatedSize(400, 300, 90)).toEqual({ w: 300, h: 400 });
    expect(rotatedSize(400, 300, 180)).toEqual({ w: 400, h: 300 });
    expect(rotateBy(0, 1)).toBe(90);
    expect(rotateBy(270, 1)).toBe(0);
    expect(rotateBy(0, -1)).toBe(270);
    expect(rotateBy(90, 4)).toBe(90);
  });

  it("crops to a ratio with the biggest centred rectangle", () => {
    expect(aspectCrop(1, 1000, 500)).toEqual({ x: 0.25, y: 0, w: 0.5, h: 1 });
    const portrait = aspectCrop(9 / 16, 1000, 1000)!;
    expect(portrait.w).toBeCloseTo(0.5625);
    expect(portrait.h).toBe(1);
    expect(portrait.x).toBeCloseTo(0.21875);
    expect(aspectCrop(null, 100, 100)).toBeNull();
    expect(aspectCrop(1, 800, 800)).toBeNull();
  });

  it("keeps any crop inside the picture and above a minimum size", () => {
    const wild: Rect[] = [
      { x: -1, y: -1, w: 5, h: 5 },
      { x: 0.9, y: 0.9, w: 0.5, h: 0.5 },
      { x: 0.5, y: 0.5, w: 0, h: 0 },
    ];
    for (const r of wild) {
      const c = clampRect(r);
      expect(c.w).toBeGreaterThanOrEqual(MIN_CROP);
      expect(c.h).toBeGreaterThanOrEqual(MIN_CROP);
      expect(c.x).toBeGreaterThanOrEqual(0);
      expect(c.y).toBeGreaterThanOrEqual(0);
      expect(c.x + c.w).toBeLessThanOrEqual(1 + 1e-9);
      expect(c.y + c.h).toBeLessThanOrEqual(1 + 1e-9);
    }
  });

  it("dragging handles resizes and moves without ever leaving the picture", () => {
    const start: Rect = { x: 0.2, y: 0.2, w: 0.5, h: 0.5 };
    expect(dragRect(start, "move", 0.1, -0.05)).toEqual({ x: 0.30000000000000004, y: 0.15000000000000002, w: 0.5, h: 0.5 });
    expect(dragRect(start, "move", 5, 5)).toEqual({ x: 0.5, y: 0.5, w: 0.5, h: 0.5 });
    const se = dragRect(start, "se", 0.1, 0.1);
    expect(se).toMatchObject({ x: 0.2, y: 0.2 });
    expect(se.w).toBeCloseTo(0.6);
    const nw = dragRect(start, "nw", 0.1, 0.1);
    expect(nw.x).toBeCloseTo(0.3);
    expect(nw.w).toBeCloseTo(0.4);
    // Past the opposite edge: stops at the minimum.
    expect(dragRect(start, "nw", 5, 5).w).toBeCloseTo(MIN_CROP);
    expect(dragRect(start, "e", 5, 0).x + dragRect(start, "e", 5, 0).w).toBeLessThanOrEqual(1);
    for (const h of ["n", "s", "e", "w", "ne", "sw"] as const) {
      const r = dragRect(start, h, -9, 9);
      expect(r.x).toBeGreaterThanOrEqual(0);
      expect(r.y).toBeGreaterThanOrEqual(0);
      expect(r.x + r.w).toBeLessThanOrEqual(1 + 1e-9);
      expect(r.y + r.h).toBeLessThanOrEqual(1 + 1e-9);
    }
  });

  it("works out the pixels to keep and the finished size", () => {
    const s: EditState = { ...NEUTRAL_EDIT, rotation: 90, crop: { x: 0.25, y: 0, w: 0.5, h: 1 } };
    // A 400x300 picture turned 90 degrees is 300x400; the crop keeps the middle half of its width.
    expect(cropPixels(400, 300, s)).toEqual({ sx: 75, sy: 0, sw: 150, sh: 400 });
    expect(outputSize(400, 300, s, 200)).toEqual({ w: 75, h: 200 });
    expect(outputSize(400, 300, NEUTRAL_EDIT, 9999)).toEqual({ w: 400, h: 300 });
  });

  it("a neutral edit changes nothing, and any change counts", () => {
    expect(hasEdits(NEUTRAL_EDIT)).toBe(false);
    expect(filterCss(NEUTRAL_EDIT)).toBe("");
    for (const change of [{ rotation: 90 as const }, { flipX: true }, { filter: "mono" as const }, { adjust: { ...NEUTRAL_EDIT.adjust, blur: 5 } }, { crop: { x: 0, y: 0, w: 0.5, h: 0.5 } }]) {
      expect(hasEdits({ ...NEUTRAL_EDIT, ...change })).toBe(true);
    }
  });

  it("colour settings become the CSS filter, and presets combine with the sliders", () => {
    const bright: EditState = { ...NEUTRAL_EDIT, adjust: { brightness: 50, contrast: 0, saturation: -100, blur: 0 } };
    const css = filterCss(bright);
    expect(css).toContain("brightness(1.300)");
    expect(css).toContain("saturate(0.000)");
    expect(css).not.toContain("contrast");
    const mono: EditState = { ...NEUTRAL_EDIT, filter: "noir" };
    expect(filterCss(mono)).toContain("grayscale(1.000)");
    expect(colorParams({ ...NEUTRAL_EDIT, adjust: { ...NEUTRAL_EDIT.adjust, blur: 100 } }, 2000).blurPx).toBeCloseTo(20);
    // Slider values outside the range are held at its ends.
    expect(colorParams({ ...NEUTRAL_EDIT, adjust: { brightness: 900, contrast: -900, saturation: 0, blur: -5 } }).contrast).toBeCloseTo(0.2);
    expect(colorParams({ ...NEUTRAL_EDIT, adjust: { brightness: 900, contrast: 0, saturation: 0, blur: -5 } }).blurPx).toBe(0);
  });

  it("the by-hand colour fallback does what the filter names say", () => {
    const px = (r: number, g: number, b: number) => new Uint8ClampedArray([r, g, b, 255]);
    const gray = px(200, 100, 50);
    adjustPixels(gray, { ...colorParams(NEUTRAL_EDIT), grayscale: 1 });
    expect(gray[0]).toBe(gray[1]);
    expect(gray[1]).toBe(gray[2]);
    const dark = px(100, 100, 100);
    adjustPixels(dark, { ...colorParams(NEUTRAL_EDIT), brightness: 0.5 });
    expect(dark[0]).toBe(50);
    const same = px(12, 34, 56);
    adjustPixels(same, colorParams(NEUTRAL_EDIT));
    expect(Array.from(same)).toEqual([12, 34, 56, 255]);
    const washed = px(128, 128, 128);
    adjustPixels(washed, { ...colorParams(NEUTRAL_EDIT), contrast: 0.5 });
    expect(washed[0]).toBe(128);
    const desat = px(255, 0, 0);
    adjustPixels(desat, { ...colorParams(NEUTRAL_EDIT), saturate: 0 });
    expect(desat[0]).toBe(desat[1]);
  });

  it("drawing: strokes grow, stay in bounds, are bounded, and a change of geometry clears them", () => {
    let stroke = newStroke("red", 0.5, false, { x: -1, y: 2 });
    expect(stroke.size).toBeLessThanOrEqual(0.08);
    expect(stroke.points[0]).toEqual({ x: 0, y: 1 });
    stroke = extendStroke(stroke, { x: 0.5, y: 0.5 });
    expect(stroke.points).toHaveLength(2);
    // A point barely moved is skipped.
    expect(extendStroke(stroke, { x: 0.5001, y: 0.5 }).points).toHaveLength(2);
    let s = addStroke(NEUTRAL_EDIT, stroke);
    expect(s.strokes).toHaveLength(1);
    for (let i = 0; i < MAX_STROKES + 20; i++) s = addStroke(s, stroke);
    expect(s.strokes).toHaveLength(MAX_STROKES);
    expect(withGeometry(s, { rotation: 90 }).strokes).toEqual([]);
    expect(withGeometry(s, { flipX: true }).flipX).toBe(true);
    let long = newStroke("blue", 0.01, false, { x: 0, y: 0 });
    for (let i = 0; i < 5000; i++) long = extendStroke(long, { x: (i % 100) / 100, y: (i % 37) / 37 });
    expect(long.points.length).toBeLessThanOrEqual(2000);
  });
});

describe("collage", () => {
  it("has layouts for two, three and four photos, with every cell inside the picture and no overlap", () => {
    expect(layoutsFor(1)).toEqual([]);
    expect(layoutsFor(2).length).toBeGreaterThan(0);
    expect(layoutsFor(3).length).toBeGreaterThan(0);
    expect(layoutsFor(4).length).toBeGreaterThan(0);
    for (const layout of COLLAGE_LAYOUTS) {
      let area = 0;
      for (const c of layout.cells) {
        expect(c.x).toBeGreaterThanOrEqual(0);
        expect(c.y).toBeGreaterThanOrEqual(0);
        expect(c.x + c.w).toBeLessThanOrEqual(1 + 1e-9);
        expect(c.y + c.h).toBeLessThanOrEqual(1 + 1e-9);
        area += c.w * c.h;
      }
      // The cells tile the picture exactly.
      expect(area).toBeCloseTo(1);
      for (let i = 0; i < layout.cells.length; i++) {
        for (let j = i + 1; j < layout.cells.length; j++) {
          const a = layout.cells[i];
          const b = layout.cells[j];
          const overlapW = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x);
          const overlapH = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y);
          expect(overlapW <= 1e-9 || overlapH <= 1e-9, `${layout.id} ${i}/${j}`).toBe(true);
        }
      }
    }
  });

  it("leaves a gap between photos and around the edge", () => {
    const rects = cellRects(COLLAGE_LAYOUTS.find((l) => l.id === "side2")!, 1000, 2000, 20);
    expect(rects[0]).toEqual({ x: 10, y: 10, w: 480, h: 1980 });
    expect(rects[1].x - (rects[0].x + rects[0].w)).toBe(20);
  });

  it("fills a cell without stretching: the middle of the picture, cropped", () => {
    const wide = fitCover(2000, 1000, 500, 500);
    expect(wide).toEqual({ sx: 500, sy: 0, sw: 1000, sh: 1000 });
    const tall = fitCover(1000, 2000, 500, 500);
    expect(tall).toEqual({ sx: 0, sy: 500, sw: 1000, sh: 1000 });
    const exact = fitCover(800, 600, 400, 300);
    expect(exact.sw / exact.sh).toBeCloseTo(4 / 3);
  });
});

describe("text templates", () => {
  it("each one makes valid stickers with fresh ids every time", () => {
    for (const t of TEXT_TEMPLATES) {
      const a = applyTemplate(t);
      const b = applyTemplate(t);
      expect(a.stickers).toHaveLength(t.stickers.length);
      expect(parseStickers(a.stickers)).toEqual(a.stickers);
      if (a.stickers.length) expect(a.stickers[0].id).not.toBe(b.stickers[0].id);
      expect(a.text.length).toBeLessThanOrEqual(700);
    }
    expect(TEXT_TEMPLATES.map((t) => t.id)).toContain("this-or-that");
  });
});

/** jsdom's Blob can't be read through Response or arrayBuffer(): FileReader can. */
const blobBytes = (blob: Blob): Promise<Uint8Array> =>
  new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.readAsArrayBuffer(blob);
  });

const tone = (hz: number, seconds: number, rate = 8000, amp = 0.5): Mono => {
  const samples = new Float32Array(Math.round(seconds * rate));
  for (let i = 0; i < samples.length; i++) samples[i] = amp * Math.sin((2 * Math.PI * hz * i) / rate);
  return { samples, sampleRate: rate };
};

describe("audio editing", () => {
  it("writes a WAV that reads back the same", () => {
    const m = tone(440, 0.25);
    const wav = encodeWav(m);
    expect(wav.type).toBe("audio/wav");
    expect(wav.size).toBe(44 + m.samples.length * 2);
  });

  it("a WAV round trip keeps the samples to within 16-bit precision", async () => {
    const m = tone(440, 0.1);
    const read = await blobBytes(encodeWav(m));
    const view = new DataView(read.buffer, read.byteOffset, read.byteLength);
    expect(String.fromCharCode(read[0], read[1], read[2], read[3])).toBe("RIFF");
    expect(String.fromCharCode(read[8], read[9], read[10], read[11])).toBe("WAVE");
    expect(view.getUint32(24, true)).toBe(8000);
    expect(view.getUint16(22, true)).toBe(1);
    expect(view.getUint16(34, true)).toBe(16);
    expect(view.getUint32(40, true)).toBe(m.samples.length * 2);
    const back = decodeWav(read)!;
    expect(back.sampleRate).toBe(8000);
    expect(back.samples).toHaveLength(m.samples.length);
    for (let i = 0; i < m.samples.length; i += 37) expect(Math.abs(back.samples[i] - m.samples[i])).toBeLessThan(1e-3);
    expect(decodeWav(new Uint8Array(10))).toBeNull();
    const notPcm = read.slice();
    notPcm[20] = 3;
    expect(decodeWav(notPcm)).toBeNull();
  });

  it("clips out-of-range samples instead of wrapping around", async () => {
    const loud: Mono = { samples: Float32Array.from([2, -2, 0.5]), sampleRate: 8000 };
    const back = decodeWav(await blobBytes(encodeWav(loud)))!;
    expect(back.samples[0]).toBeCloseTo(1, 3);
    expect(back.samples[1]).toBeCloseTo(-1, 3);
  });

  it("mixes channels down to mono and keeps mono as it is", () => {
    const l = Float32Array.from([1, 0, 0.5]);
    const r = Float32Array.from([0, 1, 0.5]);
    expect(Array.from(mixToMono([l, r], 8000).samples)).toEqual([0.5, 0.5, 0.5]);
    expect(Array.from(mixToMono([l], 8000).samples)).toEqual([1, 0, 0.5]);
    expect(mixToMono([], 8000).samples).toHaveLength(0);
  });

  it("trims to a span, clamped to the recording", () => {
    const m = tone(200, 2);
    expect(durationMs(trim(m, 500, 1500))).toBe(1000);
    expect(durationMs(trim(m, -100, 99_999))).toBe(2000);
    expect(trim(m, 1500, 500).samples).toHaveLength(0);
    expect(trim(m, 3000, 4000).samples).toHaveLength(0);
  });

  it("fades the ends so a cut doesn't click, and evens out loudness", () => {
    const m = { samples: new Float32Array(8000).fill(0.5), sampleRate: 8000 };
    const f = fade(m, 100, 100);
    expect(f.samples[0]).toBe(0);
    expect(f.samples[f.samples.length - 1]).toBe(0);
    expect(f.samples[4000]).toBe(0.5);
    expect(peak(normalize(tone(300, 0.2, 8000, 0.1), 0.9).samples)).toBeCloseTo(0.9, 2);
    // Silence is left alone, not blown up into noise.
    expect(peak(normalize({ samples: new Float32Array(100), sampleRate: 8000 }).samples)).toBe(0);
    expect(peak(gain(tone(300, 0.1, 8000, 0.8), 5).samples)).toBe(1);
  });

  it("resamples to the length the new rate implies", () => {
    const m = tone(100, 1, 16000);
    expect(resample(m, 8000).samples).toHaveLength(8000);
    expect(resample(m, 32000).samples).toHaveLength(32000);
    expect(resample(m, 16000).samples).toHaveLength(16000);
    expect(resample({ samples: new Float32Array(0), sampleRate: 8000 }, 4000).samples).toHaveLength(0);
  });

  it("voice filters do what they say, and every one of them keeps the audio sound", () => {
    const m = tone(220, 1, 8000, 0.4);
    expect(applyVoiceFilter(m, "none").samples).toEqual(m.samples);
    expect(applyVoiceFilter(m, "chipmunk").samples.length).toBeLessThan(m.samples.length);
    expect(applyVoiceFilter(m, "deep").samples.length).toBeGreaterThan(m.samples.length);
    expect(applyVoiceFilter(m, "echo").samples.length).toBeGreaterThan(m.samples.length);
    expect(applyVoiceFilter(m, "robot").samples).toHaveLength(m.samples.length);
    expect(applyVoiceFilter(m, "radio").samples).toHaveLength(m.samples.length);
    for (const id of VOICE_FILTERS) {
      const out = applyVoiceFilter(m, id);
      expect(out.sampleRate).toBe(8000);
      let finite = true;
      for (let i = 0; i < out.samples.length; i++) if (!Number.isFinite(out.samples[i]) || Math.abs(out.samples[i]) > 1.5) finite = false;
      expect(finite, id).toBe(true);
    }
    // The echo repeats the sound after its delay: after the original ends there is still sound.
    const echoed = applyVoiceFilter(m, "echo");
    expect(peak(echoed.samples.subarray(m.samples.length + 200))).toBeGreaterThan(0.01);
    // The radio takes the low end out: a 60 Hz hum is much quieter, a 1 kHz tone mostly stays.
    const hum = applyVoiceFilter(tone(60, 1, 8000, 0.5), "radio");
    const mid = applyVoiceFilter(tone(1000, 1, 8000, 0.2), "radio");
    expect(peak(hum.samples.subarray(2000))).toBeLessThan(peak(mid.samples.subarray(2000)));
  });

  it("draws a picture of the loudness: bounded, and following the sound", () => {
    const quietThenLoud = new Float32Array(4800);
    for (let i = 2400; i < 4800; i++) quietThenLoud[i] = 0.8 * Math.sin(i / 5);
    const bars = waveformBars(quietThenLoud, 48);
    expect(bars).toHaveLength(48);
    expect(bars.every((b) => Number.isInteger(b) && b >= 0 && b <= 100)).toBe(true);
    expect(Math.max(...bars)).toBe(100);
    expect(bars.slice(0, 20).every((b) => b === 0)).toBe(true);
    expect(bars.slice(30).every((b) => b > 30)).toBe(true);
    expect(waveformBars(new Float32Array(0), 16)).toEqual(new Array(16).fill(0));
  });

  it("finishes a voice story: cut, filtered, levelled, small, with its length and picture", async () => {
    const m = tone(300, 3, 44100, 0.3);
    const done = finishVoice(m, { startMs: 500, endMs: 2500, filter: "none" });
    expect(done.durationMs).toBe(2000);
    expect(done.waveform).toHaveLength(48);
    expect(done.blob.type).toBe("audio/wav");
    // Brought down to 22.05 kHz, 16-bit mono: about 44 KB a second.
    expect(done.blob.size).toBeLessThan(2.2 * 44100 * 2 / 2 + 100);
    const echoed = finishVoice(m, { startMs: 0, endMs: 1000, filter: "echo" });
    expect(echoed.durationMs).toBeGreaterThan(1000);
  });
});

describe("captions", () => {
  it("times read and write in the forms people use", () => {
    expect(formatClock(83_400)).toBe("1:23.4");
    expect(formatClock(0)).toBe("0:00.0");
    expect(parseClock("83.4")).toBe(83_400);
    expect(parseClock("1:23.4")).toBe(83_400);
    expect(parseClock("0:01:23,400")).toBe(83_400);
    expect(parseClock(" 5 ")).toBe(5000);
    for (const bad of ["", "abc", "1:2:3:4", "-3", "1:xx", "99999"]) expect(parseClock(bad), bad).toBeNull();
  });

  it("reads SubRip, drops its markup, and leaves out cues that don't fit", () => {
    const srt = [
      "1",
      "00:00:01,000 --> 00:00:03,500",
      "<i>Hello</i> there",
      "",
      "2",
      "00:00:04,000 --> 00:00:06,000",
      "Second line",
      "continues {\\an8}here",
      "",
      "3",
      "00:00:09,000 --> 00:00:08,000",
      "Backwards",
      "",
      "4",
      "not a time --> 00:00:08,000",
      "Junk",
    ].join("\r\n");
    const cues = parseSrt(srt);
    expect(cues).toEqual([
      { startMs: 1000, endMs: 3500, text: "Hello there" },
      { startMs: 4000, endMs: 6000, text: "Second line continues here" },
    ]);
    expect(parseSrt("")).toEqual([]);
  });

  it("writes SubRip that reads back the same", () => {
    const cues = [
      { startMs: 0, endMs: 1500, text: "One" },
      { startMs: 3_723_004, endMs: 3_725_000, text: "Two" },
    ];
    const out = toSrt(cues);
    expect(out).toContain("00:00:00,000 --> 00:00:01,500");
    expect(out).toContain("01:02:03,004 --> 01:02:05,000");
    // 3,725 s is past what a story keeps; the first line survives a read-back, the second doesn't fit.
    expect(parseSrt(out)).toEqual([cues[0]]);
  });

  it("limits, orders and cleans cues the way every viewer does", () => {
    const cues = parseCaptions([
      { startMs: 5000, endMs: 6000, text: "later" },
      { startMs: 1000, endMs: 2000, text: "first‮" },
      { startMs: 1000, endMs: 1000, text: "empty span" },
      { startMs: 0, endMs: 5000, text: "" },
      { startMs: 0, endMs: 5000, text: "x".repeat(121) },
      "nope",
      { startMs: "a", endMs: 2, text: "bad" },
    ]);
    expect(cues.map((c) => c.text)).toEqual(["first", "later"]);
    expect(parseCaptions(Array.from({ length: 200 }, (_, i) => ({ startMs: i * 100, endMs: i * 100 + 50, text: "line" })))).toHaveLength(60);
    expect(captionAt(cues, 1500)).toBe("first");
    expect(captionAt(cues, 2000)).toBeNull();
    expect(captionAt(cues, 5999)).toBe("later");
  });

  it("spreads typed lines across the recording", () => {
    const cues = evenCaptions(["a", " ", "b", "c"], 9000);
    expect(cues).toEqual([
      { startMs: 0, endMs: 3000, text: "a" },
      { startMs: 3000, endMs: 6000, text: "b" },
      { startMs: 6000, endMs: 9000, text: "c" },
    ]);
    expect(evenCaptions([], 1000)).toEqual([]);
    expect(evenCaptions(["a"], 0)).toEqual([]);
  });
});

// --- Camera ---------------------------------------------------------------------------------------------------------

function fakeTrack(caps: Record<string, unknown> = {}, kind = "video") {
  const applied: unknown[] = [];
  const track = {
    kind,
    stopped: false,
    stop() {
      this.stopped = true;
    },
    getCapabilities: () => caps,
    applyConstraints: vi.fn(async (c: unknown) => void applied.push(c)),
    applied,
  };
  return track as unknown as MediaStreamTrack & { stopped: boolean; applied: unknown[]; applyConstraints: ReturnType<typeof vi.fn> };
}
function fakeStream(video: MediaStreamTrack | null, audio: MediaStreamTrack | null = null): MediaStream {
  const tracks = [video, audio].filter(Boolean) as MediaStreamTrack[];
  return { getVideoTracks: () => (video ? [video] : []), getAudioTracks: () => (audio ? [audio] : []), getTracks: () => tracks } as unknown as MediaStream;
}
const domError = (name: string) => Object.assign(new Error(name), { name });

describe("the camera", () => {
  it("reads what the camera can do", () => {
    expect(readCaps(fakeTrack({}))).toEqual({ zoom: null, torch: false });
    expect(readCaps(fakeTrack({ zoom: { min: 1, max: 5, step: 0.1 }, torch: true }))).toEqual({ zoom: { min: 1, max: 5, step: 0.1 }, torch: true });
    // A zoom that can't zoom is no zoom.
    expect(readCaps(fakeTrack({ zoom: { min: 1, max: 1 } })).zoom).toBeNull();
    expect(readCaps({} as MediaStreamTrack)).toEqual({ zoom: null, torch: false });
  });

  it("zooms within what it reports and does nothing where there is no zoom", async () => {
    const track = fakeTrack({ zoom: { min: 1, max: 4 } });
    const caps = readCaps(track);
    expect(await applyZoom(track, 9, caps)).toBe(4);
    expect(await applyZoom(track, 0, caps)).toBe(1);
    expect(track.applyConstraints).toHaveBeenCalledTimes(2);
    const none = fakeTrack({});
    expect(await applyZoom(none, 3, readCaps(none))).toBe(1);
    expect(none.applyConstraints).not.toHaveBeenCalled();
    expect(await setTorch(none, true, readCaps(none))).toBe(false);
    const lamp = fakeTrack({ torch: true });
    expect(await setTorch(lamp, true, readCaps(lamp))).toBe(true);
  });

  it("opens the camera and microphone, with the front camera asked for by name", async () => {
    const v = fakeTrack({ zoom: { min: 1, max: 3 } });
    const a = fakeTrack({}, "audio");
    const getUserMedia = vi.fn(async () => fakeStream(v, a));
    const cam = await openCamera({ getUserMedia }, "user", true);
    expect(cam.hasAudio).toBe(true);
    expect(cam.caps.zoom).not.toBeNull();
    expect(JSON.stringify(getUserMedia.mock.calls[0])).toContain('"facingMode":{"ideal":"user"}');
  });

  it("falls back to a silent camera when only the microphone is refused", async () => {
    const getUserMedia = vi.fn().mockRejectedValueOnce(domError("NotAllowedError")).mockResolvedValueOnce(fakeStream(fakeTrack()));
    const cam = await openCamera({ getUserMedia }, "environment", true);
    expect(cam.hasAudio).toBe(false);
    expect(getUserMedia).toHaveBeenCalledTimes(2);
  });

  it("says why it can't, in each case", async () => {
    const attempt = (devices: Pick<MediaDevices, "getUserMedia"> | undefined): Promise<CameraUnavailableError> =>
      openCamera(devices, "user", false).then(
        () => {
          throw new Error("the camera should not have opened");
        },
        (e: CameraUnavailableError) => e
      );
    const denied = await attempt({ getUserMedia: async () => Promise.reject(domError("NotAllowedError")) });
    expect(denied).toBeInstanceOf(CameraUnavailableError);
    expect(denied.problem).toBe("denied");
    expect(denied.message).toMatch(/blocked/);
    expect((await attempt({ getUserMedia: async () => Promise.reject(domError("NotFoundError")) })).problem).toBe("no_device");
    expect((await attempt({ getUserMedia: async () => Promise.reject(domError("NotReadableError")) })).problem).toBe("busy");
    expect((await attempt(undefined)).problem).toBe("unsupported");
    expect(describeCameraError(domError("Weird")).problem).toBe("other");
    expect(describeCameraError(null).problem).toBe("other");
    // A stream without a camera in it is closed again and reported.
    const empty = fakeTrack({}, "audio");
    const stream = fakeStream(null, empty);
    const noVideo = await attempt({ getUserMedia: async () => stream });
    expect(noVideo.problem).toBe("no_device");
    expect(empty.stopped).toBe(true);
  });

  it("counts down and mirrors the selfie camera", () => {
    expect(countdownSteps(0)).toEqual([]);
    expect(countdownSteps(3)).toEqual([3, 2, 1]);
    expect(countdownSteps(10)).toHaveLength(10);
    expect(otherFacing("user")).toBe("environment");
    expect(otherFacing("environment")).toBe("user");
    expect(shouldMirror("user")).toBe(true);
    expect(shouldMirror("environment")).toBe(false);
  });
});

// --- Recorder --------------------------------------------------------------------------------------------------------

class FakeRecorder {
  static supported = new Set<string>();
  static isTypeSupported = (t: string) => FakeRecorder.supported.has(t);
  state: "inactive" | "recording" = "inactive";
  mimeType: string;
  ondataavailable: ((e: { data: Blob }) => void) | null = null;
  onstop: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(_stream: MediaStream, options?: { mimeType?: string }) {
    this.mimeType = options?.mimeType ?? "";
  }
  start() {
    this.state = "recording";
  }
  stop() {
    if (this.state === "inactive") throw new Error("InvalidStateError: the recorder isn't recording");
    this.state = "inactive";
    this.ondataavailable?.({ data: new Blob(["abc"], { type: this.mimeType }) });
    this.onstop?.();
  }
}

describe("recording", () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it("picks the first type the browser can record", () => {
    expect(pickRecorderMime("video", (t) => t === "video/mp4")).toBe("video/mp4");
    expect(pickRecorderMime("video", () => true)).toBe("video/webm;codecs=vp9,opus");
    expect(pickRecorderMime("audio", (t) => t.startsWith("audio/webm"))).toBe("audio/webm;codecs=opus");
    expect(pickRecorderMime("audio", () => false)).toBeUndefined();
  });

  it("records, and reports how long it ran", async () => {
    vi.stubGlobal("MediaRecorder", FakeRecorder);
    let clock = 1000;
    const rec = new StreamRecorder({} as MediaStream, "audio", 60_000, "audio/webm;codecs=opus", () => clock);
    rec.start();
    clock = 4200;
    expect(rec.elapsedMs()).toBe(3200);
    const done = await rec.stop();
    expect(done.durationMs).toBe(3200);
    expect(done.blob.size).toBe(3);
    // Stopping twice is harmless and gives the same recording.
    expect(await rec.stop()).toBe(done);
  });

  it("stops itself at the limit (hands-free)", async () => {
    vi.useFakeTimers();
    vi.stubGlobal("MediaRecorder", FakeRecorder);
    const rec = new StreamRecorder({} as MediaStream, "video", 5000, "video/webm");
    rec.start();
    vi.advanceTimersByTime(5001);
    const done = await rec.done;
    expect(done.blob.type).toBe("video/webm");
  });

  it("explains itself where recording isn't possible", () => {
    vi.stubGlobal("MediaRecorder", undefined);
    expect(() => new StreamRecorder({} as MediaStream, "video", 1000)).toThrow(/can't record/);
  });
});

// --- Edges found by breaking the code on purpose -----------------------------------------------------------------------
// Each of these failed to fail when one rule was removed in a mutation pass (README, "Mutation checks"), so they are here.

describe("edges the first tests missed", () => {
  afterEach(() => vi.restoreAllMocks());

  it("a redone step can be undone again", () => {
    let h = pushHistory(pushHistory(startHistory(0), 1), 2);
    h = redo(undo(h));
    expect(h.present).toBe(2);
    expect(undo(h).present).toBe(1);
  });

  it("typed lines run to the very end even when the time doesn't divide evenly", () => {
    const out = evenCaptions(["a", "b", "c"], 1000);
    expect(out.map((c) => c.startMs)).toEqual([0, 333, 666]);
    expect(out[0].endMs).toBe(333);
    expect(out[2].endMs).toBe(1000);
  });

  it("reads times of at most three parts", () => {
    expect(parseClock("0:0:0:1")).toBeNull();
    expect(parseClock("0:01:02.5")).toBe(62_500);
  });

  it("a caption can't end after the longest story", () => {
    expect(parseCaptions([{ startMs: 0, endMs: MAX_CAPTION_END_MS + 1, text: "x" }])).toEqual([]);
    expect(parseCaptions([{ startMs: 0, endMs: MAX_CAPTION_END_MS, text: "x" }])).toHaveLength(1);
  });

  it("offers only the layouts that take exactly that many photos", () => {
    for (const n of [2, 3, 4]) {
      expect(layoutsFor(n).length).toBeGreaterThan(0);
      for (const l of layoutsFor(n)) expect(l.cells).toHaveLength(n);
    }
    expect(layoutsFor(1)).toEqual([]);
    expect(layoutsFor(5)).toEqual([]);
  });

  it("refuses a collage with the wrong number of photos", () => {
    const layout = COLLAGE_LAYOUTS.find((l) => l.id === "side2")!;
    const photo = { width: 10, height: 10 } as unknown as CollageImage;
    expect(() => drawCollage([photo], layout)).toThrow(/needs 2 photos/);
    expect(() => drawCollage([photo, photo, photo], layout)).toThrow(/needs 2 photos/);
  });

  it("speeding a recording up keeps its last sample a real number", () => {
    const up = resample({ samples: Float32Array.from([0, 0.5, 1, 0.25]), sampleRate: 100 }, 200);
    expect(up.samples).toHaveLength(8);
    expect([...up.samples].every(Number.isFinite)).toBe(true);
    expect(up.samples[7]).toBeCloseTo(0.25);
  });

  it("an end before the start, or before zero, keeps nothing", () => {
    const m: Mono = { samples: new Float32Array(1000).fill(0.5), sampleRate: 1000 };
    expect(trim(m, 800, 200).samples).toHaveLength(0);
    expect(trim(m, 0, -500).samples).toHaveLength(0);
    expect(trim(m, 200, 99_000).samples).toHaveLength(800);
  });

  it("writes full scale in both directions", async () => {
    const bytes = await blobBytes(encodeWav({ samples: Float32Array.from([-1, 1, 0]), sampleRate: 8000 }));
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    expect(view.getInt16(44, true)).toBe(-32768);
    expect(view.getInt16(46, true)).toBe(32767);
    expect(view.getInt16(48, true)).toBe(0);
  });

  it("only reads back a mono 16-bit WAV", async () => {
    const bytes = await blobBytes(encodeWav(tone(440, 0.05)));
    expect(decodeWav(bytes)).not.toBeNull();
    const stereo = Uint8Array.from(bytes);
    new DataView(stereo.buffer).setUint16(22, 2, true);
    expect(decodeWav(stereo)).toBeNull();
    const eightBit = Uint8Array.from(bytes);
    new DataView(eightBit.buffer).setUint16(34, 8, true);
    expect(decodeWav(eightBit)).toBeNull();
  });

  it("a finished voice starts and ends at silence, so a cut doesn't click", async () => {
    const steady: Mono = { samples: new Float32Array(22_050).fill(0.5), sampleRate: 22_050 };
    const done = finishVoice(steady, { startMs: 0, endMs: 1000, filter: "none" });
    const read = decodeWav(await blobBytes(done.blob))!;
    expect(Math.abs(read.samples[0])).toBeLessThan(0.001);
    expect(Math.abs(read.samples[read.samples.length - 1])).toBeLessThan(0.001);
    // And the middle is at the evened-out level.
    expect(Math.abs(read.samples[11_000])).toBeGreaterThan(0.85);
  });

  it("a quarter turn either way swaps the sides, a half turn doesn't", () => {
    expect(rotatedSize(100, 50, 0)).toEqual({ w: 100, h: 50 });
    expect(rotatedSize(100, 50, 90)).toEqual({ w: 50, h: 100 });
    expect(rotatedSize(100, 50, 180)).toEqual({ w: 100, h: 50 });
    expect(rotatedSize(100, 50, 270)).toEqual({ w: 50, h: 100 });
  });

  it("the left edge of a crop stops a minimum crop short of the right edge, which stays where it was", () => {
    const r = dragRect({ x: 0.2, y: 0, w: 0.5, h: 1 }, "w", 2, 0);
    expect(r.x).toBeCloseTo(0.7 - MIN_CROP);
    expect(r.w).toBeCloseTo(MIN_CROP);
  });

  it("the right edge of a crop stops at the border of the picture", () => {
    const r = dragRect({ x: 0.5, y: 0, w: 0.3, h: 1 }, "e", 0.9, 0);
    expect(r.x).toBeCloseTo(0.5);
    expect(r.w).toBeCloseTo(0.5);
  });

  it("a template whose sticker is broken loses just that sticker, and each use gets its own overlays", () => {
    const poll = TEXT_TEMPLATES.find((t) => t.id === "this-or-that")!;
    const broken = { ...poll, stickers: [() => ({ ...poll.stickers[0](), options: ["only one"] }) as unknown as StorySticker, poll.stickers[0]] };
    expect(applyTemplate(broken).stickers).toHaveLength(1);

    const announcement = TEXT_TEMPLATES.find((t) => t.id === "announcement")!;
    const first = applyTemplate(announcement);
    first.overlays[0].x = 0.9;
    expect(announcement.overlays[0].x).toBe(0.5);
    expect(applyTemplate(announcement).overlays[0].x).toBe(0.5);
  });
});

describe("taking a photo", () => {
  function fakeCanvas() {
    const calls: string[] = [];
    const ctx = {
      filter: "",
      translate: (...a: number[]) => calls.push(`translate ${a.join(",")}`),
      scale: (...a: number[]) => calls.push(`scale ${a.join(",")}`),
      drawImage: () => calls.push("draw"),
    };
    vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(ctx as unknown as CanvasRenderingContext2D);
    vi.spyOn(HTMLCanvasElement.prototype, "toBlob").mockImplementation((cb) => cb(new Blob(["jpeg"], { type: "image/jpeg" })));
    return { calls, ctx };
  }
  const video = (w: number, h: number) => ({ videoWidth: w, videoHeight: h }) as HTMLVideoElement;
  afterEach(() => vi.restoreAllMocks());

  it("mirrors the selfie, and only the selfie", async () => {
    const { calls } = fakeCanvas();
    await captureFrame(video(200, 100), { mirror: true, filterCss: "" });
    expect(calls).toEqual(["translate 200,0", "scale -1,1", "draw"]);
    calls.length = 0;
    await captureFrame(video(200, 100), { mirror: false, filterCss: "" });
    expect(calls).toEqual(["draw"]);
  });

  it("applies the look, scales big frames down and never scales small ones up", async () => {
    const { ctx } = fakeCanvas();
    const big = await captureFrame(video(4000, 2000), { mirror: false, filterCss: "saturate(1.4)" });
    expect(big).toMatchObject({ width: 1920, height: 960 });
    expect(ctx.filter).toBe("saturate(1.4)");
    expect(await captureFrame(video(300, 200), { mirror: false, filterCss: "" })).toMatchObject({ width: 300, height: 200 });
  });

  it("won't photograph a camera that isn't ready", async () => {
    fakeCanvas();
    await expect(captureFrame(video(0, 0), { mirror: false, filterCss: "" })).rejects.toThrow(/isn't ready/);
  });
});

describe("recording, the unusual endings", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("stopping after the recorder ended by itself doesn't throw", async () => {
    vi.stubGlobal("MediaRecorder", FakeRecorder);
    const rec = new StreamRecorder({} as MediaStream, "audio", 60_000, "audio/webm");
    rec.start();
    // The stream ended on its own (a device unplugged): the recorder is already inactive.
    const inner = (rec as unknown as { recorder: FakeRecorder }).recorder;
    inner.state = "inactive";
    inner.onstop?.();
    await expect(rec.stop()).resolves.toMatchObject({ blob: expect.any(Blob) });
  });

  it("a cancelled recording keeps none of its sound, not even the last piece", async () => {
    vi.stubGlobal("MediaRecorder", FakeRecorder);
    const rec = new StreamRecorder({} as MediaStream, "audio", 60_000, "audio/webm");
    rec.start();
    (rec as unknown as { recorder: FakeRecorder }).recorder.ondataavailable?.({ data: new Blob(["earlier"]) });
    rec.cancel();
    expect((await rec.done).blob.size).toBe(0);
  });
});
