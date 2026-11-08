// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Recording a camera or microphone stream with the browser's MediaRecorder. The result is an ordinary
// file blob; the story code encrypts it like any other media before it leaves the device.

export type RecordKind = "video" | "audio";

const CANDIDATES: Record<RecordKind, string[]> = {
  video: ["video/webm;codecs=vp9,opus", "video/webm;codecs=vp8,opus", "video/webm", "video/mp4"],
  audio: ["audio/webm;codecs=opus", "audio/webm", "audio/mp4", "audio/ogg;codecs=opus"],
};

/** The first type this browser can record that stories accept, or undefined if there is none. */
export function pickRecorderMime(kind: RecordKind, isSupported?: (type: string) => boolean): string | undefined {
  const check = isSupported ?? ((t: string) => typeof MediaRecorder !== "undefined" && typeof MediaRecorder.isTypeSupported === "function" && MediaRecorder.isTypeSupported(t));
  return CANDIDATES[kind].find(check);
}

export interface Recording {
  blob: Blob;
  durationMs: number;
}

/** A recording in progress. `stop()` resolves with the file and how long it ran (a recorded WebM has no length in its header). */
export class StreamRecorder {
  private recorder: MediaRecorder;
  private chunks: Blob[] = [];
  private startedAt = 0;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private finished: Promise<Recording>;
  private resolveFinished!: (r: Recording) => void;
  private rejectFinished!: (e: Error) => void;
  private stopped = false;
  private cancelled = false;

  constructor(
    stream: MediaStream,
    readonly kind: RecordKind,
    readonly maxMs: number,
    mimeType: string | undefined = pickRecorderMime(kind),
    private readonly now: () => number = () => performance.now()
  ) {
    if (typeof MediaRecorder === "undefined") throw new Error("This browser can't record here. Choose a file instead.");
    this.recorder = new MediaRecorder(stream, mimeType ? { mimeType } : undefined);
    this.finished = new Promise<Recording>((resolve, reject) => {
      this.resolveFinished = resolve;
      this.rejectFinished = reject;
    });
    this.recorder.ondataavailable = (e: BlobEvent) => {
      // After a cancel the final chunk the browser flushes on stop is dropped too.
      if (!this.cancelled && e.data && e.data.size > 0) this.chunks.push(e.data);
    };
    this.recorder.onerror = () => this.rejectFinished(new Error("The recording failed."));
    this.recorder.onstop = () => {
      if (this.timer) clearTimeout(this.timer);
      const type = this.recorder.mimeType || mimeType || (kind === "video" ? "video/webm" : "audio/webm");
      this.resolveFinished({ blob: new Blob(this.chunks, { type }), durationMs: Math.max(1, Math.round(this.now() - this.startedAt)) });
    };
  }

  start(): void {
    this.startedAt = this.now();
    this.recorder.start(1000);
    // Hands-free: it stops itself at the limit.
    this.timer = setTimeout(() => this.stop(), this.maxMs);
  }

  /** Settles when the recording ends, whether by `stop()` or by reaching the limit. */
  get done(): Promise<Recording> {
    return this.finished;
  }

  elapsedMs(): number {
    return this.startedAt ? Math.round(this.now() - this.startedAt) : 0;
  }

  stop(): Promise<Recording> {
    if (!this.stopped) {
      this.stopped = true;
      if (this.timer) clearTimeout(this.timer);
      if (this.recorder.state !== "inactive") this.recorder.stop();
    }
    return this.finished;
  }

  /** Stops and throws the recording away. */
  cancel(): void {
    this.cancelled = true;
    this.chunks = [];
    void this.stop().catch(() => {});
  }
}
