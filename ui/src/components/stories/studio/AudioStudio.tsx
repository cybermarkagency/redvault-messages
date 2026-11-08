// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { decodeToMono, durationMs as monoDuration, finishVoice, resample, STORY_AUDIO_RATE, VOICE_FILTER_LABELS, VOICE_FILTERS, waveformBars, type Mono, type VoiceFilterId } from "../../../lib/studio/audioDsp.js";
import { describeCameraError, stopStream } from "../../../lib/studio/camera.js";
import { pickRecorderMime, StreamRecorder } from "../../../lib/studio/recorder.js";
import { MAX_AUDIO_MS, prepareAudio, StoryInputError, type PreparedMedia } from "../../../lib/stories.js";
import { STORY_BACKGROUNDS, MAX_STORY_TEXT, type StoryBackground, type StoryCaption } from "../../../lib/storyCrypto.js";
import { CloseIcon } from "../../icons24.js";
import { CaptionEditor } from "./CaptionEditor.js";
import styles from "../Stories.module.css";

export interface AudioResult {
  media: PreparedMedia;
  title: string;
  background: StoryBackground;
  captions: StoryCaption[];
}

interface Props {
  /** A recording already made, to edit again (instead of starting from nothing). */
  source?: Blob;
  /** Start straight in recording or in the file picker. */
  start?: "record" | "file";
  onDone: (result: AudioResult) => void;
  onCancel: () => void;
}

const clock = (ms: number) => `${Math.floor(ms / 60000)}:${String(Math.floor((ms % 60000) / 1000)).padStart(2, "0")}`;

/** A file's length from its own header, before it is decoded (decoding five minutes of stereo takes a lot of memory). */
function probeDuration(file: Blob): Promise<number> {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const a = document.createElement("audio");
    a.preload = "metadata";
    a.onloadedmetadata = () => {
      URL.revokeObjectURL(url);
      resolve(Math.round(a.duration * 1000));
    };
    a.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new StoryInputError("That audio couldn't be opened."));
    };
    a.src = url;
  });
}

/**
 * Voice and audio stories, edited on this device: record or import, cut the start and end, change the voice,
 * add a title and captions. The microphone is asked for only when someone presses Record.
 */
export function AudioStudio({ source, start, onDone, onCancel }: Props) {
  const [phase, setPhase] = useState<"choose" | "recording" | "edit">("choose");
  const [mono, setMono] = useState<Mono | null>(null);
  const [trimStart, setTrimStart] = useState(0);
  const [trimEnd, setTrimEnd] = useState(0);
  const [filter, setFilter] = useState<VoiceFilterId>("none");
  const [title, setTitle] = useState("");
  const [background, setBackground] = useState<StoryBackground>("night");
  const [captions, setCaptions] = useState<StoryCaption[]>([]);
  const [captionsOpen, setCaptionsOpen] = useState(false);
  const [level, setLevel] = useState(0);
  const [elapsed, setElapsed] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [playing, setPlaying] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);
  const recorderRef = useRef<StreamRecorder | null>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const audioCtxRef = useRef<AudioContext | null>(null);
  const playerRef = useRef<{ audio: HTMLAudioElement; url: string } | null>(null);
  const startedRef = useRef(false);

  const total = mono ? monoDuration(mono) : 0;

  useEffect(
    () => () => {
      recorderRef.current?.cancel();
      stopStream(streamRef.current);
      void audioCtxRef.current?.close().catch(() => {});
      stopPlaying();
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    []
  );

  useEffect(() => {
    if (startedRef.current) return;
    if (source) {
      startedRef.current = true;
      void load(source);
      return;
    }
    if (!start) return;
    startedRef.current = true;
    if (start === "record") void record();
    else fileRef.current?.click();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [start]);

  function stopPlaying() {
    const p = playerRef.current;
    if (!p) return;
    p.audio.pause();
    URL.revokeObjectURL(p.url);
    playerRef.current = null;
    setPlaying(false);
  }

  async function load(blob: Blob) {
    setBusy(true);
    setError(null);
    try {
      const decoded = await decodeToMono(blob);
      // Voices don't need CD quality, and it keeps what is held in memory small.
      const small = decoded.sampleRate > STORY_AUDIO_RATE ? resample(decoded, STORY_AUDIO_RATE) : decoded;
      const ms = monoDuration(small);
      if (ms < 300) throw new StoryInputError("That recording is too short.");
      setMono(small);
      setTrimStart(0);
      setTrimEnd(Math.min(ms, MAX_AUDIO_MS));
      setPhase("edit");
    } catch (err) {
      setError(err instanceof StoryInputError ? err.message : "That audio couldn't be opened or edited in this browser.");
      setPhase("choose");
    } finally {
      setBusy(false);
    }
  }

  async function record() {
    setError(null);
    if (!navigator.mediaDevices?.getUserMedia) return setError("This browser can't use the microphone here. Choose an audio file instead.");
    const mime = pickRecorderMime("audio");
    if (!mime) return setError("This browser can't record audio here. Choose an audio file instead.");
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true } });
      streamRef.current = stream;
      const rec = new StreamRecorder(stream, "audio", MAX_AUDIO_MS, mime);
      recorderRef.current = rec;
      // A level meter, so it is plain that the microphone is hearing something.
      const Ctx = window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
      let analyser: AnalyserNode | null = null;
      if (Ctx) {
        const ctx = new Ctx();
        audioCtxRef.current = ctx;
        analyser = ctx.createAnalyser();
        analyser.fftSize = 512;
        ctx.createMediaStreamSource(stream).connect(analyser);
      }
      rec.start();
      setPhase("recording");
      const data = new Uint8Array(analyser?.fftSize ?? 0);
      const tick = setInterval(() => {
        setElapsed(rec.elapsedMs());
        if (analyser) {
          analyser.getByteTimeDomainData(data);
          let sum = 0;
          for (const v of data) sum += ((v - 128) / 128) ** 2;
          setLevel(Math.min(1, Math.sqrt(sum / data.length) * 4));
        }
      }, 80);
      try {
        const done = await rec.done;
        clearInterval(tick);
        stopStream(stream);
        streamRef.current = null;
        setLevel(0);
        await load(done.blob);
      } catch {
        clearInterval(tick);
        setError("The recording failed. Try again, or choose a file.");
        setPhase("choose");
      }
    } catch (err) {
      setError(describeCameraError(err).message.replace(/camera/gi, "microphone"));
      setPhase("choose");
    }
  }

  async function pickFile(file: File | undefined) {
    if (!file) return;
    setError(null);
    setBusy(true);
    try {
      const ms = await probeDuration(file);
      if (ms > MAX_AUDIO_MS + 500) throw new StoryInputError("Audio stories can be up to 5 minutes long. Trim the file first.");
      await load(file);
    } catch (err) {
      setError(err instanceof StoryInputError ? err.message : "That audio couldn't be opened.");
      setBusy(false);
    }
  }

  function listen() {
    if (playing) return stopPlaying();
    if (!mono) return;
    const done = finishVoice(mono, { startMs: trimStart, endMs: trimEnd, filter });
    const url = URL.createObjectURL(done.blob);
    const audio = new Audio(url);
    audio.onended = stopPlaying;
    playerRef.current = { audio, url };
    setPlaying(true);
    void audio.play().catch(stopPlaying);
  }

  async function finish() {
    if (!mono) return;
    stopPlaying();
    setBusy(true);
    setError(null);
    try {
      const done = finishVoice(mono, { startMs: trimStart, endMs: trimEnd, filter });
      const media = await prepareAudio(done.blob, done.durationMs, done.waveform);
      const clipped = captions.filter((c) => c.startMs < done.durationMs).map((c) => ({ ...c, endMs: Math.min(c.endMs, done.durationMs) }));
      onDone({ media, title: title.trim().slice(0, MAX_STORY_TEXT), background, captions: clipped });
    } catch (err) {
      setError(err instanceof StoryInputError ? err.message : "That audio couldn't be prepared.");
      setBusy(false);
    }
  }

  const bars = mono ? waveformBars(mono.samples, 48) : [];
  const from = total ? (trimStart / total) * bars.length : 0;
  const to = total ? (trimEnd / total) * bars.length : 0;

  return (
    <div className={styles.editor} role="group" aria-label="Voice and audio">
      <div className={styles.editorTop}>
        <button type="button" className={styles.chromeButton} aria-label="Cancel" onClick={onCancel}>
          <CloseIcon size={20} />
        </button>
        <strong className={styles.editorTitle}>Voice and audio</strong>
        <span className={styles.headerSpacer} />
        {phase === "edit" && (
          <button type="button" className={styles.sheetButtonPrimary} disabled={busy} onClick={finish}>
            {busy ? "Preparing…" : "Use this"}
          </button>
        )}
      </div>

      <div className={styles.editorStage}>
        {phase === "choose" && (
          <div className={styles.studioChoices}>
            <button type="button" className={styles.pickButton} disabled={busy} onClick={() => void record()}>
              🎙 Record your voice
            </button>
            <button type="button" className={styles.pickButton} disabled={busy} onClick={() => fileRef.current?.click()}>
              🎵 Choose an audio file
            </button>
            <p className={styles.pickNote}>Edited here on your device. Only the encrypted result is ever uploaded. Use audio you have the right to share.</p>
          </div>
        )}
        {phase === "recording" && (
          <div className={styles.studioChoices} role="status">
            <div className={styles.levelMeter} aria-label="Microphone level">
              <span style={{ width: `${Math.round(level * 100)}%` }} />
            </div>
            <p className={styles.recordClock}>
              <span className={styles.recordDot} aria-hidden="true" /> {clock(elapsed)} / {clock(MAX_AUDIO_MS)}
            </p>
            <button type="button" className={styles.sheetButtonPrimary} onClick={() => void recorderRef.current?.stop()}>
              Stop
            </button>
          </div>
        )}
        {phase === "edit" && mono && (
          <div className={styles.audioEdit}>
            <div className={styles.waveform} role="img" aria-label={`Recording, ${clock(total)} long`}>
              {bars.map((v, i) => (
                <span key={i} className={i >= from - 0.5 && i < to ? styles.waveformPlayed : undefined} style={{ height: `${8 + v * 0.9}%` }} />
              ))}
            </div>
            <p className={styles.sheetNote}>
              Keeping {clock(trimStart)} to {clock(trimEnd)} of {clock(total)}
            </p>
          </div>
        )}
      </div>

      <input ref={fileRef} type="file" accept="audio/*,.mp3,.m4a,.wav,.ogg,.webm" hidden onChange={(e) => { void pickFile(e.target.files?.[0]); e.target.value = ""; }} />

      {phase === "edit" && mono && (
        <div className={styles.editorTools}>
          <label className={styles.sliderRow}>
            <span>Start</span>
            <input type="range" min={0} max={Math.max(0, total - 500)} step={100} value={trimStart} aria-label="Start of the part to keep" onChange={(e) => { const v = Number(e.target.value); setTrimStart(v); if (trimEnd < v + 500) setTrimEnd(Math.min(total, v + 500)); stopPlaying(); }} />
            <output>{clock(trimStart)}</output>
          </label>
          <label className={styles.sliderRow}>
            <span>End</span>
            <input type="range" min={500} max={total} step={100} value={trimEnd} aria-label="End of the part to keep" onChange={(e) => { const v = Number(e.target.value); setTrimEnd(v); if (trimStart > v - 500) setTrimStart(Math.max(0, v - 500)); stopPlaying(); }} />
            <output>{clock(trimEnd)}</output>
          </label>
          <div className={styles.editorRow} role="radiogroup" aria-label="Voice">
            {VOICE_FILTERS.map((f) => (
              <button key={f} type="button" role="radio" aria-checked={filter === f} className={`${styles.sheetButton} ${filter === f ? styles.editorPicked : ""}`} onClick={() => { setFilter(f); stopPlaying(); }}>
                {VOICE_FILTER_LABELS[f]}
              </button>
            ))}
          </div>
          <div className={styles.editorRow}>
            <button type="button" className={styles.sheetButton} onClick={listen}>
              {playing ? "Stop listening" : "Listen"}
            </button>
            <button type="button" className={styles.sheetButton} onClick={() => setBackground(STORY_BACKGROUNDS[(STORY_BACKGROUNDS.indexOf(background) + 1) % STORY_BACKGROUNDS.length])}>
              Background: {background}
            </button>
            <button type="button" className={styles.sheetButton} onClick={() => setCaptionsOpen(true)}>
              Captions{captions.length ? ` (${captions.length})` : ""}
            </button>
          </div>
          <input className={styles.sheetInput} placeholder="Add a title (optional)" aria-label="Title" maxLength={120} value={title} onChange={(e) => setTitle(e.target.value)} />
        </div>
      )}

      {error && (
        <p className={styles.sheetError} role="alert">
          {error}
        </p>
      )}
      {captionsOpen && <CaptionEditor captions={captions} durationMs={Math.max(1000, trimEnd - trimStart)} onChange={setCaptions} onClose={() => setCaptionsOpen(false)} />}
    </div>
  );
}
