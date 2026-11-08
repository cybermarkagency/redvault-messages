// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Collages: two to four photos in one picture, laid out on this device. The result is a normal story
// photo (a fresh JPEG), so nothing about it needs special handling afterwards.

/** A cell as fractions (0-1) of the whole picture. */
export interface Cell {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface CollageLayout {
  id: string;
  label: string;
  cells: Cell[];
}

export const MAX_COLLAGE_PHOTOS = 4;

const grid = (cols: number, rows: number): Cell[] => {
  const out: Cell[] = [];
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) out.push({ x: c / cols, y: r / rows, w: 1 / cols, h: 1 / rows });
  return out;
};

export const COLLAGE_LAYOUTS: CollageLayout[] = [
  { id: "stack2", label: "Two, stacked", cells: grid(1, 2) },
  { id: "side2", label: "Two, side by side", cells: grid(2, 1) },
  { id: "stack3", label: "Three, stacked", cells: grid(1, 3) },
  {
    id: "big1small2",
    label: "One big, two small",
    cells: [
      { x: 0, y: 0, w: 1, h: 0.6 },
      { x: 0, y: 0.6, w: 0.5, h: 0.4 },
      { x: 0.5, y: 0.6, w: 0.5, h: 0.4 },
    ],
  },
  {
    id: "left1right2",
    label: "One tall, two short",
    cells: [
      { x: 0, y: 0, w: 0.5, h: 1 },
      { x: 0.5, y: 0, w: 0.5, h: 0.5 },
      { x: 0.5, y: 0.5, w: 0.5, h: 0.5 },
    ],
  },
  { id: "grid4", label: "Four", cells: grid(2, 2) },
];

/** The layouts that take exactly `count` photos. */
export function layoutsFor(count: number): CollageLayout[] {
  return COLLAGE_LAYOUTS.filter((l) => l.cells.length === count);
}

export interface PixelRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** Cells in pixels, each pulled in by half the gap on every side so neighbours are `gap` apart (and the edges are too). */
export function cellRects(layout: CollageLayout, width: number, height: number, gap: number): PixelRect[] {
  const half = gap / 2;
  return layout.cells.map((c) => {
    const x = c.x * width + half;
    const y = c.y * height + half;
    const w = Math.max(1, c.w * width - gap);
    const h = Math.max(1, c.h * height - gap);
    return { x, y, w, h };
  });
}

/** The part of a picture to draw so it fills a cell without stretching (the middle, cropped to fit). */
export function fitCover(imageW: number, imageH: number, cellW: number, cellH: number): { sx: number; sy: number; sw: number; sh: number } {
  const cellAspect = cellW / cellH;
  const imageAspect = imageW / imageH;
  if (imageAspect > cellAspect) {
    const sw = imageH * cellAspect;
    return { sx: (imageW - sw) / 2, sy: 0, sw, sh: imageH };
  }
  const sh = imageW / cellAspect;
  return { sx: 0, sy: (imageH - sh) / 2, sw: imageW, sh };
}

export type CollageImage = ImageBitmap | HTMLImageElement | HTMLCanvasElement;

const size = (img: CollageImage): { w: number; h: number } =>
  "naturalWidth" in img && img.naturalWidth ? { w: img.naturalWidth, h: img.naturalHeight } : { w: (img as ImageBitmap).width, h: (img as ImageBitmap).height };

export interface CollageOptions {
  width?: number;
  height?: number;
  gap?: number;
  background?: string;
}

/** The photos drawn into the layout on a canvas (the editor's live preview uses a smaller one). */
export function drawCollage(images: CollageImage[], layout: CollageLayout, options: CollageOptions = {}): HTMLCanvasElement {
  if (images.length !== layout.cells.length) throw new Error("This layout needs " + layout.cells.length + " photos.");
  const width = options.width ?? 1080;
  const height = options.height ?? 1920;
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d")!;
  ctx.fillStyle = options.background ?? "#111111";
  ctx.fillRect(0, 0, width, height);
  const rects = cellRects(layout, width, height, options.gap ?? 12);
  images.forEach((img, i) => {
    const r = rects[i];
    const dim = size(img);
    const f = fitCover(dim.w, dim.h, r.w, r.h);
    ctx.drawImage(img, f.sx, f.sy, f.sw, f.sh, r.x, r.y, r.w, r.h);
  });
  return canvas;
}

/** Draws the photos into the layout and returns a JPEG. */
export async function renderCollage(images: CollageImage[], layout: CollageLayout, options: CollageOptions = {}): Promise<{ blob: Blob; width: number; height: number }> {
  const canvas = drawCollage(images, layout, options);
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.9));
  if (!blob) throw new Error("That collage couldn't be made.");
  return { blob, width: canvas.width, height: canvas.height };
}
