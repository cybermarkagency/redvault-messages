// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Photo editing, all on this device: crop, rotate, flip, colour correction, filters and drawing.
//
// An edit is a plain value (EditState). The maths that decides what the result looks like is in pure
// functions (tested without a browser); `renderEdit` draws it on a canvas and returns a fresh JPEG, which,
// like every photo a story posts, carries none of the original file's metadata (location, camera, time).
// Nothing is uploaded anywhere to edit a photo.
import type { StoryColor } from "../storyCrypto.js";

export type Rotation = 0 | 90 | 180 | 270;

/** A rectangle as fractions (0-1) of the image after rotation. */
export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface Adjust {
  /** -100 to 100; 0 changes nothing. */
  brightness: number;
  contrast: number;
  saturation: number;
  /** 0 to 100 (up to about 1% of the image's height of blur). */
  blur: number;
}

export const FILTERS = ["none", "vivid", "warm", "cool", "fade", "mono", "noir", "sepia"] as const;
export type FilterId = (typeof FILTERS)[number];

export const FILTER_LABELS: Record<FilterId, string> = { none: "Original", vivid: "Vivid", warm: "Warm", cool: "Cool", fade: "Fade", mono: "Mono", noir: "Noir", sepia: "Sepia" };

/** Pen colours as real colours (the drawing is baked into the picture), matching the story colour tokens. */
export const PEN_COLORS: Record<StoryColor, string> = {
  white: "#ffffff",
  black: "#111111",
  red: "#ef4444",
  orange: "#f97316",
  yellow: "#facc15",
  green: "#22c55e",
  teal: "#14b8a6",
  blue: "#3b82f6",
  purple: "#a855f7",
  pink: "#ec4899",
};

export const MIN_PEN = 0.004;
export const MAX_PEN = 0.08;
export const MAX_STROKES = 400;
export const MAX_POINTS_PER_STROKE = 2000;

export interface Point {
  x: number;
  y: number;
}

/** A line drawn on the result; points and size are fractions of the finished picture's width and height. */
export interface Stroke {
  color: StoryColor;
  size: number;
  eraser: boolean;
  points: Point[];
}

export interface EditState {
  rotation: Rotation;
  flipX: boolean;
  /** Null: the whole picture. */
  crop: Rect | null;
  adjust: Adjust;
  filter: FilterId;
  strokes: Stroke[];
}

export const NEUTRAL_ADJUST: Adjust = { brightness: 0, contrast: 0, saturation: 0, blur: 0 };
export const NEUTRAL_EDIT: EditState = { rotation: 0, flipX: false, crop: null, adjust: NEUTRAL_ADJUST, filter: "none", strokes: [] };

export const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v));

export function hasEdits(s: EditState): boolean {
  const a = s.adjust;
  return s.rotation !== 0 || s.flipX || s.crop !== null || s.filter !== "none" || a.brightness !== 0 || a.contrast !== 0 || a.saturation !== 0 || a.blur !== 0 || s.strokes.length > 0;
}

// --- Geometry -------------------------------------------------------------------------------------

/** The picture's size after rotating a quarter turn at a time. */
export function rotatedSize(w: number, h: number, rotation: Rotation): { w: number; h: number } {
  return rotation === 90 || rotation === 270 ? { w: h, h: w } : { w, h };
}

export function rotateBy(rotation: Rotation, quarterTurns: number): Rotation {
  return ((((((rotation / 90 + quarterTurns) % 4) + 4) % 4) * 90) as Rotation);
}

export const MIN_CROP = 0.08;

/** A crop kept inside the picture and no smaller than MIN_CROP either way. */
export function clampRect(r: Rect): Rect {
  const w = clamp(r.w, MIN_CROP, 1);
  const h = clamp(r.h, MIN_CROP, 1);
  return { x: clamp(r.x, 0, 1 - w), y: clamp(r.y, 0, 1 - h), w, h };
}

/** The largest centred crop with this width-to-height ratio (null: the whole picture). */
export function aspectCrop(aspect: number | null, imageW: number, imageH: number): Rect | null {
  if (aspect === null || !(aspect > 0) || imageW <= 0 || imageH <= 0) return null;
  const current = imageW / imageH;
  if (Math.abs(current - aspect) < 1e-3) return null;
  if (current > aspect) {
    const w = aspect / current;
    return { x: (1 - w) / 2, y: 0, w, h: 1 };
  }
  const h = current / aspect;
  return { x: 0, y: (1 - h) / 2, w: 1, h };
}

export type Handle = "move" | "nw" | "ne" | "sw" | "se" | "n" | "s" | "e" | "w";

/** Drags a crop by a handle; `dx` and `dy` are fractions of the picture. Always returns a valid crop. */
export function dragRect(r: Rect, handle: Handle, dx: number, dy: number): Rect {
  let { x, y, w, h } = r;
  if (handle === "move") return clampRect({ x: clamp(x + dx, 0, 1 - w), y: clamp(y + dy, 0, 1 - h), w, h });
  if (handle.includes("w")) {
    const nx = clamp(x + dx, 0, x + w - MIN_CROP);
    w += x - nx;
    x = nx;
  }
  if (handle.includes("e")) w = clamp(w + dx, MIN_CROP, 1 - x);
  if (handle.includes("n")) {
    const ny = clamp(y + dy, 0, y + h - MIN_CROP);
    h += y - ny;
    y = ny;
  }
  if (handle.includes("s")) h = clamp(h + dy, MIN_CROP, 1 - y);
  return clampRect({ x, y, w, h });
}

/** Which part of the rotated picture ends up in the result, in pixels of the rotated picture. */
export function cropPixels(imageW: number, imageH: number, s: EditState): { sx: number; sy: number; sw: number; sh: number } {
  const rot = rotatedSize(imageW, imageH, s.rotation);
  const c = s.crop ?? { x: 0, y: 0, w: 1, h: 1 };
  return { sx: Math.round(c.x * rot.w), sy: Math.round(c.y * rot.h), sw: Math.max(1, Math.round(c.w * rot.w)), sh: Math.max(1, Math.round(c.h * rot.h)) };
}

/** The finished picture's size, scaled down so its long side is at most `maxSide`. */
export function outputSize(imageW: number, imageH: number, s: EditState, maxSide: number): { w: number; h: number } {
  const { sw, sh } = cropPixels(imageW, imageH, s);
  const scale = Math.min(1, maxSide / Math.max(sw, sh));
  return { w: Math.max(1, Math.round(sw * scale)), h: Math.max(1, Math.round(sh * scale)) };
}

// --- Colour -----------------------------------------------------------------------------------------

/** What a preset does, as the same terms the CSS filter functions use (1 = unchanged). */
export interface ColorParams {
  brightness: number;
  contrast: number;
  saturate: number;
  grayscale: number;
  sepia: number;
  hueRotate: number;
  blurPx: number;
}

const PRESETS: Record<FilterId, Partial<ColorParams>> = {
  none: {},
  vivid: { saturate: 1.45, contrast: 1.1 },
  warm: { sepia: 0.25, saturate: 1.2, hueRotate: -8, brightness: 1.03 },
  cool: { hueRotate: 12, saturate: 0.95, brightness: 1.02, contrast: 1.04 },
  fade: { contrast: 0.85, brightness: 1.08, saturate: 0.8 },
  mono: { grayscale: 1 },
  noir: { grayscale: 1, contrast: 1.3, brightness: 0.92 },
  sepia: { sepia: 0.9, contrast: 1.05 },
};

/** The preset and the sliders together. Sliders run -100..100 around 1, blur 0..100 up to 1% of the height. */
export function colorParams(s: EditState, outputHeight = 1000): ColorParams {
  const p = PRESETS[s.filter];
  const a = s.adjust;
  // The slider is held to its range first, then scaled: brightness and contrast move less than saturation.
  const slider = (v: number, strength = 1) => 1 + (clamp(v, -100, 100) * strength) / 100;
  return {
    brightness: (p.brightness ?? 1) * slider(a.brightness, 0.6),
    contrast: (p.contrast ?? 1) * slider(a.contrast, 0.8),
    saturate: (p.saturate ?? 1) * slider(a.saturation),
    grayscale: p.grayscale ?? 0,
    sepia: p.sepia ?? 0,
    hueRotate: p.hueRotate ?? 0,
    blurPx: (clamp(a.blur, 0, 100) / 100) * (outputHeight * 0.01),
  };
}

/** A CSS `filter` value (for the live preview and for canvases that support it). Empty when nothing changes. */
export function filterCss(s: EditState, outputHeight = 1000): string {
  const c = colorParams(s, outputHeight);
  const parts: string[] = [];
  const near = (v: number, to: number) => Math.abs(v - to) < 1e-3;
  if (!near(c.brightness, 1)) parts.push(`brightness(${c.brightness.toFixed(3)})`);
  if (!near(c.contrast, 1)) parts.push(`contrast(${c.contrast.toFixed(3)})`);
  if (!near(c.saturate, 1)) parts.push(`saturate(${c.saturate.toFixed(3)})`);
  if (c.grayscale > 0) parts.push(`grayscale(${c.grayscale.toFixed(3)})`);
  if (c.sepia > 0) parts.push(`sepia(${c.sepia.toFixed(3)})`);
  if (c.hueRotate !== 0) parts.push(`hue-rotate(${c.hueRotate}deg)`);
  if (c.blurPx > 0.05) parts.push(`blur(${c.blurPx.toFixed(2)}px)`);
  return parts.join(" ");
}

/**
 * The same colour changes done by hand on pixels, for browsers whose canvas has no `filter` (older
 * Safari). Brightness, contrast, saturation, grayscale and sepia only: blur and hue rotation are left out
 * there. Changes `data` in place (RGBA, 8 bits).
 */
export function adjustPixels(data: Uint8ClampedArray, c: ColorParams): void {
  const gray = c.grayscale;
  const sep = c.sepia;
  for (let i = 0; i < data.length; i += 4) {
    let r = data[i];
    let g = data[i + 1];
    let b = data[i + 2];
    if (c.saturate !== 1) {
      const l = 0.299 * r + 0.587 * g + 0.114 * b;
      r = l + (r - l) * c.saturate;
      g = l + (g - l) * c.saturate;
      b = l + (b - l) * c.saturate;
    }
    if (gray > 0) {
      const l = 0.299 * r + 0.587 * g + 0.114 * b;
      r += (l - r) * gray;
      g += (l - g) * gray;
      b += (l - b) * gray;
    }
    if (sep > 0) {
      const tr = 0.393 * r + 0.769 * g + 0.189 * b;
      const tg = 0.349 * r + 0.686 * g + 0.168 * b;
      const tb = 0.272 * r + 0.534 * g + 0.131 * b;
      r += (tr - r) * sep;
      g += (tg - g) * sep;
      b += (tb - b) * sep;
    }
    if (c.contrast !== 1) {
      r = (r - 128) * c.contrast + 128;
      g = (g - 128) * c.contrast + 128;
      b = (b - 128) * c.contrast + 128;
    }
    if (c.brightness !== 1) {
      r *= c.brightness;
      g *= c.brightness;
      b *= c.brightness;
    }
    data[i] = r;
    data[i + 1] = g;
    data[i + 2] = b;
  }
}

// --- Drawing ------------------------------------------------------------------------------------------

/** Adds a point to a stroke, keeping it within bounds and a sensible size. */
export function extendStroke(stroke: Stroke, p: Point): Stroke {
  if (stroke.points.length >= MAX_POINTS_PER_STROKE) return stroke;
  const last = stroke.points[stroke.points.length - 1];
  const next = { x: clamp(p.x, 0, 1), y: clamp(p.y, 0, 1) };
  // Skip points too close to the last one: they only make the file heavier.
  if (last && Math.hypot(next.x - last.x, next.y - last.y) < 0.0015) return stroke;
  return { ...stroke, points: [...stroke.points, next] };
}

export function newStroke(color: StoryColor, size: number, eraser: boolean, at: Point): Stroke {
  return { color, size: clamp(size, MIN_PEN, MAX_PEN), eraser, points: [{ x: clamp(at.x, 0, 1), y: clamp(at.y, 0, 1) }] };
}

export function addStroke(s: EditState, stroke: Stroke): EditState {
  return { ...s, strokes: [...s.strokes, stroke].slice(-MAX_STROKES) };
}

/** Drawing is on the finished picture, so a change of crop, turn or flip would leave it in the wrong place: it is cleared. */
export function withGeometry(s: EditState, change: Partial<Pick<EditState, "rotation" | "flipX" | "crop">>): EditState {
  return { ...s, ...change, strokes: [] };
}

/** Draws strokes onto a 2D context the size of the finished picture. */
export function drawStrokes(ctx: CanvasRenderingContext2D, strokes: readonly Stroke[], w: number, h: number): void {
  ctx.save();
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  for (const s of strokes) {
    ctx.globalCompositeOperation = s.eraser ? "destination-out" : "source-over";
    ctx.strokeStyle = PEN_COLORS[s.color];
    ctx.fillStyle = PEN_COLORS[s.color];
    ctx.lineWidth = s.size * w;
    if (s.points.length === 1) {
      ctx.beginPath();
      ctx.arc(s.points[0].x * w, s.points[0].y * h, (s.size * w) / 2, 0, Math.PI * 2);
      ctx.fill();
      continue;
    }
    ctx.beginPath();
    ctx.moveTo(s.points[0].x * w, s.points[0].y * h);
    for (let i = 1; i < s.points.length; i++) ctx.lineTo(s.points[i].x * w, s.points[i].y * h);
    ctx.stroke();
  }
  ctx.restore();
}

// --- Rendering ------------------------------------------------------------------------------------------

export type ImageSource = ImageBitmap | HTMLImageElement | HTMLCanvasElement;

const sourceSize = (src: ImageSource): { w: number; h: number } =>
  "naturalWidth" in src && src.naturalWidth ? { w: src.naturalWidth, h: src.naturalHeight } : { w: (src as ImageBitmap).width, h: (src as ImageBitmap).height };

/** Draws the rotated, flipped picture into a canvas the size of the rotated picture. */
function drawRotated(src: ImageSource, s: EditState): HTMLCanvasElement {
  const { w, h } = sourceSize(src);
  const rot = rotatedSize(w, h, s.rotation);
  const canvas = document.createElement("canvas");
  canvas.width = rot.w;
  canvas.height = rot.h;
  const ctx = canvas.getContext("2d")!;
  ctx.translate(rot.w / 2, rot.h / 2);
  // The flip mirrors the finished picture left to right, so it is applied after the turn (transforms
  // compose backwards: the one set last acts first on the picture).
  if (s.flipX) ctx.scale(-1, 1);
  ctx.rotate((s.rotation * Math.PI) / 180);
  ctx.drawImage(src, -w / 2, -h / 2, w, h);
  return canvas;
}

/**
 * The edited picture on a canvas: rotation, flip and crop first, then colour, then the drawing. `maxSide`
 * limits the long side (stories use 1920; the editor's live preview uses less).
 */
export function drawEdit(src: ImageSource, s: EditState, maxSide: number): HTMLCanvasElement {
  const { w, h } = sourceSize(src);
  const turned = drawRotated(src, s);
  const crop = cropPixels(w, h, s);
  const out = outputSize(w, h, s, maxSide);
  const canvas = document.createElement("canvas");
  canvas.width = out.w;
  canvas.height = out.h;
  const ctx = canvas.getContext("2d")!;
  const css = filterCss(s, out.h);
  if (css && "filter" in ctx) {
    ctx.filter = css;
    ctx.drawImage(turned, crop.sx, crop.sy, crop.sw, crop.sh, 0, 0, out.w, out.h);
    ctx.filter = "none";
  } else {
    ctx.drawImage(turned, crop.sx, crop.sy, crop.sw, crop.sh, 0, 0, out.w, out.h);
    if (css) {
      const pixels = ctx.getImageData(0, 0, out.w, out.h);
      adjustPixels(pixels.data, colorParams(s, out.h));
      ctx.putImageData(pixels, 0, 0);
    }
  }
  if (s.strokes.length) {
    // The drawing is on its own layer so the eraser removes drawing, never the photo.
    const layer = document.createElement("canvas");
    layer.width = out.w;
    layer.height = out.h;
    drawStrokes(layer.getContext("2d")!, s.strokes, out.w, out.h);
    ctx.drawImage(layer, 0, 0);
  }
  return canvas;
}

/** The edited picture as a JPEG (a fresh file: none of the original's metadata survives). */
export async function renderEdit(src: ImageSource, s: EditState, maxSide: number): Promise<Blob> {
  const canvas = drawEdit(src, s, maxSide);
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.9));
  if (!blob) throw new Error("That photo couldn't be prepared.");
  return blob;
}
