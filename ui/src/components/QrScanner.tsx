// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import jsQR from "jsqr";
import { useEffect, useRef, useState } from "react";
import styles from "./QrScanner.module.css";

/**
 * Camera viewfinder that reports the first QR code it reads. Frames are
 * decoded in this tab (jsQR) and never leave it.
 */
export function QrScanner({ onResult, onCancel }: { onResult: (text: string) => void; onCancel: () => void }) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const [error, setError] = useState<string | null>(null);
  const onResultRef = useRef(onResult);
  onResultRef.current = onResult;

  useEffect(() => {
    let stream: MediaStream | null = null;
    let timer: ReturnType<typeof setInterval> | null = null;
    let stopped = false;
    const canvas = document.createElement("canvas");

    (async () => {
      try {
        stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "environment" }, audio: false });
      } catch {
        setError("Couldn't open the camera. Allow camera access, or compare the numbers instead.");
        return;
      }
      if (stopped) {
        stream.getTracks().forEach((t) => t.stop());
        return;
      }
      const video = videoRef.current!;
      video.srcObject = stream;
      await video.play().catch(() => {});
      timer = setInterval(() => {
        if (!video.videoWidth) return;
        canvas.width = video.videoWidth;
        canvas.height = video.videoHeight;
        const ctx = canvas.getContext("2d", { willReadFrequently: true });
        if (!ctx) return;
        ctx.drawImage(video, 0, 0);
        const frame = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const code = jsQR(frame.data, frame.width, frame.height, { inversionAttempts: "dontInvert" });
        if (code?.data) onResultRef.current(code.data);
      }, 250);
    })();

    return () => {
      stopped = true;
      if (timer) clearInterval(timer);
      stream?.getTracks().forEach((t) => t.stop());
    };
  }, []);

  return (
    <div className={styles.scanner}>
      {error ? (
        <p className={`${styles.error} type-secondary`}>{error}</p>
      ) : (
        <video ref={videoRef} className={styles.video} muted playsInline aria-label="Camera viewfinder" />
      )}
      <button type="button" className={styles.cancel} onClick={onCancel}>
        Cancel scan
      </button>
    </div>
  );
}
