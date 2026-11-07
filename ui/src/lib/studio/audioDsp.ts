// Audio editing, all on this device: decode, trim, fade, normalise, voice filters, a loudness picture and
// WAV output. The signal processing works on plain Float32Array samples (mono), so it is the same code
// everywhere and tested without a browser; only `decodeToMono` needs the browser's decoder.
//
// Nothing leaves the device to be edited. A recording is only ever uploaded after it is encrypted.

export interface Mono {
  samples: Float32Array;
  sampleRate: number;
}

/** What a story keeps: voices don't need CD quality, and the file stays small. */
export const STORY_AUDIO_RATE = 22_050;

export const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v));

export const durationMs = (m: Mono): number => Math.round((m.samples.length / m.sampleRate) * 1000);

// --- Decode (browser) --------------------------------------------------------------------------------

/** Decodes any audio the browser can play into mono samples. */
export async function decodeToMono(blob: Blob): Promise<Mono> {
  const Ctx: typeof AudioContext = (globalThis as unknown as { AudioContext?: typeof AudioContext; webkitAudioContext?: typeof AudioContext }).AudioContext ?? (globalThis as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
  if (!Ctx) throw new Error("This browser can't edit audio.");
  const ctx = new Ctx();
  try {
    const buffer = await ctx.decodeAudioData(await blobBytes(blob));
    return mixToMono(Array.from({ length: buffer.numberOfChannels }, (_, i) => buffer.getChannelData(i)), buffer.sampleRate);
  } finally {
    void ctx.close();
  }
}

function blobBytes(blob: Blob): Promise<ArrayBuffer> {
  if (typeof blob.arrayBuffer === "function") return blob.arrayBuffer();
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result as ArrayBuffer);
    reader.onerror = () => reject(reader.error ?? new Error("Couldn't read the audio"));
    reader.readAsArrayBuffer(blob);
  });
}

export function mixToMono(channels: Float32Array[], sampleRate: number): Mono {
  if (channels.length === 0) return { samples: new Float32Array(0), sampleRate };
  if (channels.length === 1) return { samples: Float32Array.from(channels[0]), sampleRate };
  const length = Math.min(...channels.map((c) => c.length));
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    let sum = 0;
    for (const c of channels) sum += c[i];
    out[i] = sum / channels.length;
  }
  return { samples: out, sampleRate };
}

// --- Editing ----------------------------------------------------------------------------------------------

/** Linear resampling (good enough for speech). */
export function resample(m: Mono, toRate: number): Mono {
  if (toRate === m.sampleRate || m.samples.length === 0) return { samples: Float32Array.from(m.samples), sampleRate: toRate };
  const ratio = m.sampleRate / toRate;
  const length = Math.max(1, Math.floor(m.samples.length / ratio));
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    const pos = i * ratio;
    const i0 = Math.floor(pos);
    const frac = pos - i0;
    const a = m.samples[i0];
    const b = m.samples[Math.min(i0 + 1, m.samples.length - 1)];
    out[i] = a + (b - a) * frac;
  }
  return { samples: out, sampleRate: toRate };
}

/** Keeps `startMs` to `endMs` (clamped to the recording). */
export function trim(m: Mono, startMs: number, endMs: number): Mono {
  const start = clamp(Math.round((startMs / 1000) * m.sampleRate), 0, m.samples.length);
  const end = clamp(Math.round((endMs / 1000) * m.sampleRate), start, m.samples.length);
  return { samples: m.samples.slice(start, end), sampleRate: m.sampleRate };
}

/** Fades the start in and the end out so a cut doesn't click. */
export function fade(m: Mono, inMs: number, outMs: number): Mono {
  const out = Float32Array.from(m.samples);
  const inN = Math.min(out.length, Math.round((inMs / 1000) * m.sampleRate));
  const outN = Math.min(out.length, Math.round((outMs / 1000) * m.sampleRate));
  for (let i = 0; i < inN; i++) out[i] *= i / inN;
  for (let i = 0; i < outN; i++) out[out.length - 1 - i] *= i / outN;
  return { samples: out, sampleRate: m.sampleRate };
}

export function peak(samples: Float32Array): number {
  let p = 0;
  for (let i = 0; i < samples.length; i++) p = Math.max(p, Math.abs(samples[i]));
  return p;
}

/** Scales the recording so its loudest point is `target` (never boosts silence into noise). */
export function normalize(m: Mono, target = 0.95): Mono {
  const p = peak(m.samples);
  if (p < 1e-4) return { samples: Float32Array.from(m.samples), sampleRate: m.sampleRate };
  const gain = target / p;
  const out = new Float32Array(m.samples.length);
  for (let i = 0; i < out.length; i++) out[i] = m.samples[i] * gain;
  return { samples: out, sampleRate: m.sampleRate };
}

export function gain(m: Mono, factor: number): Mono {
  const out = new Float32Array(m.samples.length);
  for (let i = 0; i < out.length; i++) out[i] = clamp(m.samples[i] * factor, -1, 1);
  return { samples: out, sampleRate: m.sampleRate };
}

// --- Voice filters -------------------------------------------------------------------------------------------

export const VOICE_FILTERS = ["none", "chipmunk", "deep", "robot", "echo", "radio"] as const;
export type VoiceFilterId = (typeof VOICE_FILTERS)[number];
export const VOICE_FILTER_LABELS: Record<VoiceFilterId, string> = { none: "Normal", chipmunk: "Chipmunk", deep: "Deep", robot: "Robot", echo: "Echo", radio: "Radio" };

/** A pitch change by playing faster or slower, so the length changes with it (a toy effect, on purpose). */
function repitch(m: Mono, factor: number): Mono {
  return { samples: resample({ samples: m.samples, sampleRate: m.sampleRate }, m.sampleRate / factor).samples, sampleRate: m.sampleRate };
}

function ringModulate(m: Mono, hz: number): Mono {
  const out = new Float32Array(m.samples.length);
  for (let i = 0; i < out.length; i++) out[i] = m.samples[i] * Math.sin((2 * Math.PI * hz * i) / m.sampleRate);
  return { samples: out, sampleRate: m.sampleRate };
}

function echo(m: Mono, delayMs: number, feedback: number, mix: number): Mono {
  const delay = Math.max(1, Math.round((delayMs / 1000) * m.sampleRate));
  const tail = delay * 6;
  const out = new Float32Array(m.samples.length + tail);
  const wet = new Float32Array(out.length);
  for (let i = 0; i < out.length; i++) {
    const dry = i < m.samples.length ? m.samples[i] : 0;
    const echoed = i >= delay ? wet[i - delay] * feedback : 0;
    wet[i] = dry + echoed;
    out[i] = dry * (1 - mix * 0.5) + (wet[i] - dry) * mix;
  }
  return { samples: out, sampleRate: m.sampleRate };
}

/** A second-order band limit: a cheap "small speaker" sound. */
function biquad(samples: Float32Array, b0: number, b1: number, b2: number, a1: number, a2: number): Float32Array {
  const out = new Float32Array(samples.length);
  let x1 = 0;
  let x2 = 0;
  let y1 = 0;
  let y2 = 0;
  for (let i = 0; i < samples.length; i++) {
    const x0 = samples[i];
    const y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    out[i] = y0;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
  }
  return out;
}

function lowpassCoefficients(rate: number, hz: number): [number, number, number, number, number] {
  const w0 = (2 * Math.PI * hz) / rate;
  const alpha = Math.sin(w0) / (2 * 0.7071);
  const cos = Math.cos(w0);
  const a0 = 1 + alpha;
  return [(1 - cos) / 2 / a0, (1 - cos) / a0, (1 - cos) / 2 / a0, (-2 * cos) / a0, (1 - alpha) / a0];
}

function highpassCoefficients(rate: number, hz: number): [number, number, number, number, number] {
  const w0 = (2 * Math.PI * hz) / rate;
  const alpha = Math.sin(w0) / (2 * 0.7071);
  const cos = Math.cos(w0);
  const a0 = 1 + alpha;
  return [(1 + cos) / 2 / a0, -(1 + cos) / a0, (1 + cos) / 2 / a0, (-2 * cos) / a0, (1 - alpha) / a0];
}

function radio(m: Mono): Mono {
  const hp = highpassCoefficients(m.sampleRate, 400);
  const lp = lowpassCoefficients(m.sampleRate, Math.min(3200, m.sampleRate / 2.5));
  let s = biquad(m.samples, ...hp);
  s = biquad(s, ...lp);
  // A little grit: soft clipping.
  const out = new Float32Array(s.length);
  for (let i = 0; i < out.length; i++) out[i] = Math.tanh(s[i] * 2.2) * 0.8;
  return { samples: out, sampleRate: m.sampleRate };
}

/** Applies a voice filter. The result may be a different length (chipmunk, deep, echo). */
export function applyVoiceFilter(m: Mono, id: VoiceFilterId): Mono {
  switch (id) {
    case "none":
      return { samples: Float32Array.from(m.samples), sampleRate: m.sampleRate };
    case "chipmunk":
      return repitch(m, 1.4);
    case "deep":
      return repitch(m, 0.75);
    case "robot":
      return normalize(ringModulate(m, 55), 0.9);
    case "echo":
      return normalize(echo(m, 240, 0.45, 0.6), 0.95);
    case "radio":
      return radio(m);
  }
}

// --- Picture of the sound ---------------------------------------------------------------------------------------

/** `bars` loudness values, 0-100, from the recording's volume (RMS per slice, scaled to the loudest slice). */
export function waveformBars(samples: Float32Array, bars = 48): number[] {
  if (samples.length === 0) return Array.from({ length: bars }, () => 0);
  const rms: number[] = [];
  for (let b = 0; b < bars; b++) {
    const start = Math.floor((b * samples.length) / bars);
    const end = Math.max(start + 1, Math.floor(((b + 1) * samples.length) / bars));
    let sum = 0;
    for (let i = start; i < end && i < samples.length; i++) sum += samples[i] * samples[i];
    rms.push(Math.sqrt(sum / (end - start)));
  }
  const top = Math.max(...rms, 1e-6);
  return rms.map((v) => Math.round(clamp(v / top, 0, 1) * 100));
}

// --- WAV out ---------------------------------------------------------------------------------------------------------

/** 16-bit mono PCM in a WAV file. */
export function encodeWav(m: Mono): Blob {
  const n = m.samples.length;
  const buffer = new ArrayBuffer(44 + n * 2);
  const view = new DataView(buffer);
  const text = (offset: number, s: string) => {
    for (let i = 0; i < s.length; i++) view.setUint8(offset + i, s.charCodeAt(i));
  };
  text(0, "RIFF");
  view.setUint32(4, 36 + n * 2, true);
  text(8, "WAVE");
  text(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true); // PCM
  view.setUint16(22, 1, true); // mono
  view.setUint32(24, m.sampleRate, true);
  view.setUint32(28, m.sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  text(36, "data");
  view.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) {
    const s = clamp(m.samples[i], -1, 1);
    view.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return new Blob([buffer], { type: "audio/wav" });
}

/** Reads a WAV this module wrote (16-bit mono PCM). Returns null for anything else. */
export function decodeWav(bytes: Uint8Array): Mono | null {
  if (bytes.length < 44) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tag = (o: number) => String.fromCharCode(bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]);
  if (tag(0) !== "RIFF" || tag(8) !== "WAVE" || tag(12) !== "fmt " || tag(36) !== "data") return null;
  if (view.getUint16(20, true) !== 1 || view.getUint16(22, true) !== 1 || view.getUint16(34, true) !== 16) return null;
  const n = Math.min(view.getUint32(40, true) / 2, Math.floor((bytes.length - 44) / 2));
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const v = view.getInt16(44 + i * 2, true);
    samples[i] = v < 0 ? v / 0x8000 : v / 0x7fff;
  }
  return { samples, sampleRate: view.getUint32(24, true) };
}

/** The whole chain for a story: trim, filter, even out, fade the cut, bring the rate down, write a WAV. */
export function finishVoice(m: Mono, opts: { startMs: number; endMs: number; filter: VoiceFilterId }): { blob: Blob; durationMs: number; waveform: number[] } {
  let work = trim(m, opts.startMs, opts.endMs);
  work = applyVoiceFilter(work, opts.filter);
  work = resample(work, Math.min(work.sampleRate, STORY_AUDIO_RATE));
  work = fade(normalize(work, 0.92), 40, 120);
  return { blob: encodeWav(work), durationMs: durationMs(work), waveform: waveformBars(work.samples, 48) };
}
