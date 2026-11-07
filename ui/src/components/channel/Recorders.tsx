import { useEffect, useRef, useState } from "react";
import { formatDuration } from "../../lib/channelPosts.js";
import styles from "./Composer.module.css";

const VOICE_MAX_SECONDS = 15 * 60;
const ROUND_MAX_SECONDS = 60;

/** The first recording format this browser can make, from a list of candidates. */
function pickMime(candidates: string[]): string {
  if (typeof MediaRecorder === "undefined") return "";
  return candidates.find((m) => MediaRecorder.isTypeSupported(m)) ?? "";
}

/** Records from the camera and/or microphone until stopped (or the time limit), then hands back the file. */
function useRecorder(constraints: MediaStreamConstraints, mimes: string[], maxSeconds: number, name: string, onDone: (file: File, durationMs: number) => void) {
  const [stream, setStream] = useState<MediaStream | null>(null);
  const [seconds, setSeconds] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const recorder = useRef<MediaRecorder | null>(null);
  const chunks = useRef<Blob[]>([]);
  const started = useRef(0);
  const cancelled = useRef(false);

  useEffect(() => {
    let live: MediaStream | null = null;
    let timer: ReturnType<typeof setInterval> | null = null;
    (async () => {
      if (!navigator.mediaDevices?.getUserMedia || typeof MediaRecorder === "undefined") {
        setError("This browser can't record here.");
        return;
      }
      try {
        live = await navigator.mediaDevices.getUserMedia(constraints);
      } catch {
        setError("Couldn't use the microphone or camera. Allow it in your browser's site settings.");
        return;
      }
      if (cancelled.current) {
        live.getTracks().forEach((t) => t.stop());
        return;
      }
      setStream(live);
      const mime = pickMime(mimes);
      const rec = new MediaRecorder(live, mime ? { mimeType: mime } : undefined);
      recorder.current = rec;
      rec.ondataavailable = (e) => {
        if (e.data.size) chunks.current.push(e.data);
      };
      rec.onstop = () => {
        live?.getTracks().forEach((t) => t.stop());
        if (cancelled.current) return;
        const type = rec.mimeType || mime || "application/octet-stream";
        const ext = type.includes("mp4") ? "mp4" : type.includes("ogg") ? "ogg" : "webm";
        onDone(new File(chunks.current, `${name}.${ext}`, { type: type.split(";")[0] }), Date.now() - started.current);
      };
      started.current = Date.now();
      rec.start(1000);
      timer = setInterval(() => {
        const elapsed = Math.floor((Date.now() - started.current) / 1000);
        setSeconds(elapsed);
        if (elapsed >= maxSeconds && rec.state === "recording") rec.stop();
      }, 250);
    })();
    return () => {
      cancelled.current = true;
      if (timer) clearInterval(timer);
      if (recorder.current?.state === "recording") recorder.current.stop();
      live?.getTracks().forEach((t) => t.stop());
    };
    // Recording starts once, when the recorder opens.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const stop = () => {
    if (recorder.current?.state === "recording") recorder.current.stop();
  };
  return { stream, seconds, error, stop };
}

/** A voice clip: records until you stop (15 minutes at most). */
export function VoiceRecorder({ onDone, onCancel }: { onDone: (file: File, durationMs: number) => void; onCancel: () => void }) {
  const { seconds, error, stop } = useRecorder({ audio: true }, ["audio/webm;codecs=opus", "audio/ogg;codecs=opus", "audio/mp4", "audio/webm"], VOICE_MAX_SECONDS, "voice", onDone);
  return (
    <div className={styles.recorder} role="group" aria-label="Recording a voice clip">
      {error ? (
        <p className={styles.error}>{error}</p>
      ) : (
        <span className={styles.recording}>
          <span className={styles.recDot} aria-hidden="true" /> Recording {formatDuration(seconds)}
        </span>
      )}
      {!error && (
        <button type="button" className={styles.primary} onClick={stop}>
          Stop and attach
        </button>
      )}
      <button type="button" className={styles.secondary} onClick={onCancel}>
        Cancel
      </button>
    </div>
  );
}

/** A round video message: the front camera, square, up to a minute. */
export function RoundRecorder({ onDone, onCancel }: { onDone: (file: File, durationMs: number) => void; onCancel: () => void }) {
  const { stream, seconds, error, stop } = useRecorder(
    { audio: true, video: { facingMode: "user", width: { ideal: 480 }, height: { ideal: 480 }, aspectRatio: 1 } },
    ["video/webm;codecs=vp9,opus", "video/webm;codecs=vp8,opus", "video/mp4", "video/webm"],
    ROUND_MAX_SECONDS,
    "round-video",
    onDone
  );
  const preview = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    if (preview.current && stream) preview.current.srcObject = stream;
  }, [stream]);
  return (
    <div className={styles.recorder} role="group" aria-label="Recording a round video message">
      {error ? (
        <p className={styles.error}>{error}</p>
      ) : (
        <>
          <video ref={preview} className={styles.roundPreview} autoPlay muted playsInline />
          <span className={styles.recording}>
            <span className={styles.recDot} aria-hidden="true" /> {formatDuration(seconds)} / {formatDuration(ROUND_MAX_SECONDS)}
          </span>
          <button type="button" className={styles.primary} onClick={stop}>
            Stop and attach
          </button>
        </>
      )}
      <button type="button" className={styles.secondary} onClick={onCancel}>
        Cancel
      </button>
    </div>
  );
}
