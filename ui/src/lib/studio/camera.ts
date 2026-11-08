// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The camera and microphone for the story studio. Permission is asked only when someone opens the camera
// or starts a recording, never earlier; if it is refused, or there is no camera, the studio falls back to
// choosing a file and says so.
//
// What a browser cannot do is not faked: there is no slow-motion capture and no AR lens API on the web,
// and zoom and the torch exist only on cameras that report them (mostly phones).

export type Facing = "user" | "environment";

export const MAX_RECORD_MS = 60_000;

export interface ZoomCaps {
  min: number;
  max: number;
  step: number;
}

export interface CameraCaps {
  zoom: ZoomCaps | null;
  torch: boolean;
}

type TrackWithCaps = MediaStreamTrack;

export function constraintsFor(facing: Facing, audio: boolean): MediaStreamConstraints {
  return {
    video: { facingMode: { ideal: facing }, width: { ideal: 1080 }, height: { ideal: 1920 }, frameRate: { ideal: 30 } },
    audio: audio ? { echoCancellation: true, noiseSuppression: true } : false,
  };
}

/** What this camera can do, from what it reports. A browser without `getCapabilities` reports nothing. */
export function readCaps(track: MediaStreamTrack): CameraCaps {
  const t = track as TrackWithCaps;
  // `zoom` and `torch` are not in the DOM typings yet, so what comes back is read as plain fields.
  const caps = (typeof t.getCapabilities === "function" ? t.getCapabilities() : {}) as unknown as Record<string, unknown>;
  const z = caps.zoom as { min?: number; max?: number; step?: number } | undefined;
  const zoom = z && typeof z.min === "number" && typeof z.max === "number" && z.max > z.min ? { min: z.min, max: z.max, step: z.step && z.step > 0 ? z.step : (z.max - z.min) / 20 } : null;
  return { zoom, torch: caps.torch === true };
}

export async function applyZoom(track: MediaStreamTrack, value: number, caps: CameraCaps): Promise<number> {
  if (!caps.zoom) return 1;
  const v = Math.min(caps.zoom.max, Math.max(caps.zoom.min, value));
  await track.applyConstraints({ advanced: [{ zoom: v } as MediaTrackConstraintSet] });
  return v;
}

export async function setTorch(track: MediaStreamTrack, on: boolean, caps: CameraCaps): Promise<boolean> {
  if (!caps.torch) return false;
  await track.applyConstraints({ advanced: [{ torch: on } as MediaTrackConstraintSet] });
  return on;
}

export type CameraProblem = "denied" | "no_device" | "busy" | "insecure" | "unsupported" | "other";

export interface CameraError {
  problem: CameraProblem;
  message: string;
}

/** A plain-words reason, and which kind it was, from whatever the browser threw. */
export function describeCameraError(err: unknown): CameraError {
  const name = (err as { name?: string } | null)?.name ?? "";
  switch (name) {
    case "NotAllowedError":
    case "PermissionDeniedError":
    case "SecurityError":
      return { problem: "denied", message: "Camera access is blocked. Allow it in your browser's site settings, or choose a photo or video file instead." };
    case "NotFoundError":
    case "DevicesNotFoundError":
    case "OverconstrainedError":
      return { problem: "no_device", message: "No camera was found. Choose a photo or video file instead." };
    case "NotReadableError":
    case "TrackStartError":
    case "AbortError":
      return { problem: "busy", message: "The camera is in use by another app. Close it and try again, or choose a file." };
    case "TypeError":
      return { problem: "insecure", message: "The camera needs a secure (https) page." };
    default:
      return { problem: "other", message: "The camera couldn't be opened. Choose a photo or video file instead." };
  }
}

export interface OpenedCamera {
  stream: MediaStream;
  videoTrack: MediaStreamTrack;
  caps: CameraCaps;
  facing: Facing;
  hasAudio: boolean;
}

export class CameraUnavailableError extends Error {
  constructor(readonly problem: CameraProblem, message: string) {
    super(message);
  }
}

/**
 * Opens the camera (and the microphone when `audio`). If the microphone alone is refused or missing, a
 * silent camera is opened rather than none: photos don't need it.
 */
export async function openCamera(devices: Pick<MediaDevices, "getUserMedia"> | undefined, facing: Facing, audio: boolean): Promise<OpenedCamera> {
  if (!devices || typeof devices.getUserMedia !== "function") throw new CameraUnavailableError("unsupported", "This browser can't use the camera here. Choose a photo or video file instead.");
  let stream: MediaStream;
  let hasAudio = audio;
  try {
    stream = await devices.getUserMedia(constraintsFor(facing, audio));
  } catch (err) {
    const first = describeCameraError(err);
    if (audio && first.problem !== "no_device") {
      // Maybe only the microphone is the trouble: try the camera alone.
      try {
        stream = await devices.getUserMedia(constraintsFor(facing, false));
        hasAudio = false;
      } catch (err2) {
        const e = describeCameraError(err2);
        throw new CameraUnavailableError(e.problem, e.message);
      }
    } else {
      throw new CameraUnavailableError(first.problem, first.message);
    }
  }
  const videoTrack = stream.getVideoTracks()[0];
  if (!videoTrack) {
    stopStream(stream);
    throw new CameraUnavailableError("no_device", "No camera was found. Choose a photo or video file instead.");
  }
  return { stream, videoTrack, caps: readCaps(videoTrack), facing, hasAudio: hasAudio && stream.getAudioTracks().length > 0 };
}

export function stopStream(stream: MediaStream | null | undefined): void {
  stream?.getTracks().forEach((t) => t.stop());
}

export const otherFacing = (f: Facing): Facing => (f === "user" ? "environment" : "user");

/** The seconds to show while counting down to a capture: 3, 2, 1. Empty for no timer. */
export function countdownSteps(seconds: number): number[] {
  if (!(seconds > 0)) return [];
  return Array.from({ length: Math.floor(seconds) }, (_, i) => Math.floor(seconds) - i);
}

export const TIMER_CHOICES = [0, 3, 10] as const;
export type TimerChoice = (typeof TIMER_CHOICES)[number];

/** The front camera is shown (and saved) the way a mirror would: flipped. */
export const shouldMirror = (facing: Facing): boolean => facing === "user";

/** Draws the current frame to a JPEG, mirrored for the front camera, with the chosen look applied. */
export async function captureFrame(video: HTMLVideoElement, opts: { mirror: boolean; filterCss: string; maxSide?: number }): Promise<{ blob: Blob; width: number; height: number }> {
  const vw = video.videoWidth;
  const vh = video.videoHeight;
  if (!vw || !vh) throw new Error("The camera isn't ready yet.");
  const scale = Math.min(1, (opts.maxSide ?? 1920) / Math.max(vw, vh));
  const w = Math.round(vw * scale);
  const h = Math.round(vh * scale);
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d")!;
  if (opts.filterCss && "filter" in ctx) ctx.filter = opts.filterCss;
  if (opts.mirror) {
    ctx.translate(w, 0);
    ctx.scale(-1, 1);
  }
  ctx.drawImage(video, 0, 0, w, h);
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.9));
  if (!blob) throw new Error("That photo couldn't be taken.");
  return { blob, width: w, height: h };
}

export interface FilteredStream {
  stream: MediaStream;
  stop: () => void;
}

/**
 * A recording that matches what the preview shows: the camera's picture drawn to a canvas with the look
 * applied (and mirrored for the front camera), whose stream is recorded together with the microphone.
 * Used only when there is something to apply; otherwise the camera's own stream is recorded directly.
 */
export function startFilteredStream(video: HTMLVideoElement, opts: { filterCss: string; mirror: boolean; audioTracks: MediaStreamTrack[]; maxSide?: number }): FilteredStream {
  const vw = video.videoWidth || 1080;
  const vh = video.videoHeight || 1920;
  const scale = Math.min(1, (opts.maxSide ?? 1280) / Math.max(vw, vh));
  const w = Math.round(vw * scale);
  const h = Math.round(vh * scale);
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d")!;
  let raf = 0;
  const draw = () => {
    ctx.save();
    if (opts.filterCss && "filter" in ctx) ctx.filter = opts.filterCss;
    if (opts.mirror) {
      ctx.translate(w, 0);
      ctx.scale(-1, 1);
    }
    ctx.drawImage(video, 0, 0, w, h);
    ctx.restore();
    raf = requestAnimationFrame(draw);
  };
  draw();
  const stream = canvas.captureStream(30);
  for (const t of opts.audioTracks) stream.addTrack(t);
  return {
    stream,
    stop: () => {
      cancelAnimationFrame(raf);
      // The picture's own tracks are ours to stop; the microphone track belongs to the camera stream.
      stream.getVideoTracks().forEach((t) => t.stop());
    },
  };
}
