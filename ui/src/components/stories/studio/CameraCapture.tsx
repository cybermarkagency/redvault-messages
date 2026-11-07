import { useCallback, useEffect, useRef, useState } from "react";
import {
  applyZoom,
  captureFrame,
  CameraUnavailableError,
  countdownSteps,
  MAX_RECORD_MS,
  openCamera,
  otherFacing,
  setTorch,
  shouldMirror,
  startFilteredStream,
  stopStream,
  TIMER_CHOICES,
  type CameraProblem,
  type Facing,
  type FilteredStream,
  type OpenedCamera,
  type TimerChoice,
} from "../../../lib/studio/camera.js";
import { FILTER_LABELS, FILTERS, filterCss, NEUTRAL_EDIT, type FilterId } from "../../../lib/studio/imageEdit.js";
import { pickRecorderMime, StreamRecorder } from "../../../lib/studio/recorder.js";
import { prepareRecordedVideo, StoryInputError, type PreparedMedia } from "../../../lib/stories.js";
import { CloseIcon, RefreshIcon } from "../../icons24.js";
import styles from "../Stories.module.css";

interface Props {
  onPhoto: (media: PreparedMedia) => void;
  onVideo: (media: PreparedMedia) => void;
  onClose: () => void;
  /** Camera blocked or missing: choose a file instead. */
  onChooseFile: () => void;
}

const clock = (ms: number) => `${Math.floor(ms / 60000)}:${String(Math.floor((ms % 60000) / 1000)).padStart(2, "0")}`;

/**
 * The camera: photos and video with a live look, the front or rear camera, zoom and torch where the camera has
 * them, and a timer with a countdown. The camera and microphone are asked for only here, when someone opens it.
 */
export function CameraCapture({ onPhoto, onVideo, onClose, onChooseFile }: Props) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const camRef = useRef<OpenedCamera | null>(null);
  const recorderRef = useRef<StreamRecorder | null>(null);
  const pipelineRef = useRef<FilteredStream | null>(null);
  const countdownRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const [cam, setCam] = useState<OpenedCamera | null>(null);
  const [problem, setProblem] = useState<{ kind: CameraProblem; message: string } | null>(null);
  const [facing, setFacing] = useState<Facing>("user");
  const [mode, setMode] = useState<"photo" | "video">("photo");
  const [timer, setTimer] = useState<TimerChoice>(0);
  const [look, setLook] = useState<FilterId>("none");
  const [count, setCount] = useState<number | null>(null);
  const [recording, setRecording] = useState(false);
  const [elapsed, setElapsed] = useState(0);
  const [zoom, setZoom] = useState(1);
  const [torch, setTorchOn] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const css = filterCss({ ...NEUTRAL_EDIT, filter: look });
  const mirror = shouldMirror(facing);

  // Open (or re-open for the other camera, or to add the microphone for video).
  useEffect(() => {
    let alive = true;
    setCam(null);
    setProblem(null);
    openCamera(navigator.mediaDevices, facing, mode === "video").then(
      (opened) => {
        if (!alive) return stopStream(opened.stream);
        camRef.current = opened;
        setCam(opened);
        setZoom(opened.caps.zoom?.min ?? 1);
        setTorchOn(false);
      },
      (err: unknown) => {
        if (alive) setProblem({ kind: err instanceof CameraUnavailableError ? err.problem : "other", message: err instanceof Error ? err.message : "The camera couldn't be opened." });
      }
    );
    return () => {
      alive = false;
      stopStream(camRef.current?.stream);
      camRef.current = null;
    };
  }, [facing, mode]);

  useEffect(() => {
    const v = videoRef.current;
    if (v && cam) {
      v.srcObject = cam.stream;
      void v.play().catch(() => {});
    }
  }, [cam]);

  // Leaving: stop everything that is still running.
  useEffect(
    () => () => {
      if (countdownRef.current) clearInterval(countdownRef.current);
      recorderRef.current?.cancel();
      pipelineRef.current?.stop();
      stopStream(camRef.current?.stream);
    },
    []
  );

  const afterCountdown = useCallback((then: () => void) => {
    const steps = countdownSteps(timer);
    if (steps.length === 0) return then();
    let i = 0;
    setCount(steps[0]);
    countdownRef.current = setInterval(() => {
      i++;
      if (i >= steps.length) {
        if (countdownRef.current) clearInterval(countdownRef.current);
        setCount(null);
        then();
      } else setCount(steps[i]);
    }, 1000);
  }, [timer]);

  const takePhoto = () =>
    afterCountdown(async () => {
      const v = videoRef.current;
      if (!v) return;
      setBusy(true);
      try {
        const shot = await captureFrame(v, { mirror, filterCss: css });
        onPhoto({ kind: "image", blob: shot.blob, mime: "image/jpeg", width: shot.width, height: shot.height });
      } catch (err) {
        setError(err instanceof Error ? err.message : "That photo couldn't be taken.");
      } finally {
        setBusy(false);
      }
    });

  const startRecording = () =>
    afterCountdown(() => {
      const v = videoRef.current;
      const opened = camRef.current;
      if (!v || !opened) return;
      const mime = pickRecorderMime("video");
      if (!mime) return setError("This browser can't record video here. Choose a video file instead.");
      try {
        let stream = opened.stream;
        // A look or the selfie mirror is recorded from a canvas so the file matches the preview.
        if (css || mirror) {
          pipelineRef.current = startFilteredStream(v, { filterCss: css, mirror, audioTracks: opened.stream.getAudioTracks() });
          stream = pipelineRef.current.stream;
        }
        const rec = new StreamRecorder(stream, "video", MAX_RECORD_MS, mime);
        recorderRef.current = rec;
        rec.start();
        setElapsed(0);
        setRecording(true);
        const tick = setInterval(() => setElapsed(rec.elapsedMs()), 250);
        // It stops itself at the limit; either way `done` settles and the file is handed on.
        void finishWhenStopped(rec, tick, v.videoWidth, v.videoHeight);
      } catch (err) {
        setError(err instanceof Error ? err.message : "The recording couldn't start.");
      }
    });

  /** Waits for the recorder to finish (the button, or the limit) and hands on the file. */
  async function finishWhenStopped(rec: StreamRecorder, tick: ReturnType<typeof setInterval>, width: number, height: number) {
    try {
      const done = await rec.done;
      clearInterval(tick);
      setRecording(false);
      pipelineRef.current?.stop();
      pipelineRef.current = null;
      onVideo(prepareRecordedVideo(done.blob, done.durationMs, width, height));
    } catch (err) {
      clearInterval(tick);
      setRecording(false);
      setError(err instanceof StoryInputError || err instanceof Error ? err.message : "That video couldn't be used.");
    }
  }

  const stopRecording = () => void recorderRef.current?.stop();

  const flip = () => {
    if (recording || count !== null) return;
    setFacing(otherFacing(facing));
  };

  const onZoom = async (value: number) => {
    setZoom(value);
    if (cam) await applyZoom(cam.videoTrack, value, cam.caps).catch(() => {});
  };
  const onTorch = async () => {
    if (!cam) return;
    const on = await setTorch(cam.videoTrack, !torch, cam.caps).catch(() => torch);
    setTorchOn(on);
  };
  const cycleLook = () => setLook(FILTERS[(FILTERS.indexOf(look) + 1) % FILTERS.length]);
  const cycleTimer = () => setTimer(TIMER_CHOICES[(TIMER_CHOICES.indexOf(timer) + 1) % TIMER_CHOICES.length]);

  if (problem) {
    return (
      <div className={styles.studioPanel} role="group" aria-label="Camera">
        <p className={styles.studioMessage} role="alert">
          {problem.message}
        </p>
        <div className={styles.textEditorRow}>
          <button type="button" className={styles.sheetButton} onClick={onClose}>
            Back
          </button>
          <span className={styles.headerSpacer} />
          <button type="button" className={styles.sheetButtonPrimary} onClick={onChooseFile}>
            Choose a file
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className={styles.camera} role="group" aria-label="Camera">
      <video
        ref={videoRef}
        className={styles.cameraVideo}
        style={{ filter: css || undefined, transform: mirror ? "scaleX(-1)" : undefined }}
        playsInline
        muted
        autoPlay
        aria-label="Camera preview"
      />
      {!cam && <p className={styles.studioMessage}>Opening the camera…</p>}
      <div className={styles.cameraTop}>
        <button type="button" className={styles.chromeButton} aria-label="Close the camera" onClick={onClose} disabled={recording}>
          <CloseIcon size={20} />
        </button>
        <span className={styles.headerSpacer} />
        {cam?.caps.torch && (
          <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-pressed={torch} aria-label={torch ? "Turn the flash off" : "Turn the flash on"} onClick={onTorch}>
            {torch ? "Flash on" : "Flash"}
          </button>
        )}
        <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-label={`Timer: ${timer === 0 ? "off" : `${timer} seconds`}`} disabled={recording} onClick={cycleTimer}>
          {timer === 0 ? "Timer" : `${timer}s`}
        </button>
        <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-label={`Look: ${FILTER_LABELS[look]}. Change look`} onClick={cycleLook}>
          {FILTER_LABELS[look]}
        </button>
        <button type="button" className={styles.chromeButton} aria-label="Switch camera" disabled={recording || count !== null} onClick={flip}>
          <RefreshIcon size={18} />
        </button>
      </div>
      {count !== null && (
        <div className={styles.countdown} role="timer" aria-live="assertive">
          {count}
        </div>
      )}
      {recording && (
        <div className={styles.recording} role="status">
          <span className={styles.recordDot} aria-hidden="true" /> {clock(elapsed)} / {clock(MAX_RECORD_MS)}
        </div>
      )}
      <div className={styles.cameraBottom}>
        {cam?.caps.zoom && (
          <label className={styles.zoomRow}>
            <span>Zoom {zoom.toFixed(1)}×</span>
            <input
              type="range"
              min={cam.caps.zoom.min}
              max={cam.caps.zoom.max}
              step={cam.caps.zoom.step}
              value={zoom}
              aria-label="Zoom"
              onChange={(e) => void onZoom(Number(e.target.value))}
            />
          </label>
        )}
        {error && (
          <p className={styles.sheetError} role="alert">
            {error}
          </p>
        )}
        <div className={styles.shutterRow}>
          <div className={styles.audience} role="radiogroup" aria-label="Capture mode">
            <button type="button" role="radio" aria-checked={mode === "photo"} className={mode === "photo" ? styles.audienceOn : ""} disabled={recording} onClick={() => setMode("photo")}>
              Photo
            </button>
            <button type="button" role="radio" aria-checked={mode === "video"} className={mode === "video" ? styles.audienceOn : ""} disabled={recording} onClick={() => setMode("video")}>
              Video
            </button>
          </div>
          {mode === "photo" ? (
            <button type="button" className={styles.shutter} aria-label="Take a photo" disabled={!cam || busy || count !== null} onClick={takePhoto} />
          ) : recording ? (
            <button type="button" className={`${styles.shutter} ${styles.shutterStop}`} aria-label="Stop recording" onClick={stopRecording} />
          ) : (
            <button type="button" className={`${styles.shutter} ${styles.shutterRecord}`} aria-label="Start recording" disabled={!cam || count !== null} onClick={startRecording} />
          )}
          <button type="button" className={styles.linkButton} onClick={onChooseFile} disabled={recording}>
            Choose a file
          </button>
        </div>
        <p className={styles.pickNote}>
          {mode === "video" && cam && !cam.hasAudio ? "No microphone: this video will be silent. " : ""}Up to 60 seconds. Slow motion and AR lenses need a phone app; the browser can't do them.
        </p>
      </div>
    </div>
  );
}
