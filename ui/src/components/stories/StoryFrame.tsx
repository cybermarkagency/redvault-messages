// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { CSSProperties, ReactNode, RefObject } from "react";
import type { StoryColor, StoryMeta, StoryOverlay } from "../../lib/storyCrypto.js";
import styles from "./Stories.module.css";

const LIGHT: StoryColor[] = ["white", "yellow", "orange", "green", "teal"];

export const storyColor = (c: StoryColor) => `var(--story-color-${c})`;

/** Where an overlay sits and how it's drawn: the same on every screen size (sizes are in container units). */
export function overlayStyle(o: StoryOverlay): CSSProperties {
  const style: CSSProperties = {
    left: `${o.x * 100}%`,
    top: `${o.y * 100}%`,
    transform: `translate(-50%, -50%) rotate(${o.rotation}deg) scale(${o.scale})`,
  };
  if (o.type === "text") {
    if (o.boxed) {
      style.background = storyColor(o.color);
      style.color = storyColor(LIGHT.includes(o.color) ? "black" : "white");
    } else {
      style.color = storyColor(o.color);
    }
  }
  return style;
}

export function overlayClass(o: StoryOverlay): string {
  const motion = o.anim ? ` ${styles[`anim_${o.anim}`]}` : "";
  if (o.type === "emoji") return `${styles.overlay} ${styles.overlayEmoji}${motion}`;
  return `${styles.overlay} ${styles.overlayText} ${styles[`font_${o.font}`]} ${o.boxed ? styles.overlayBoxed : ""}${motion}`;
}

/**
 * An overlay's motion lives on an inner element, so the placement transform (position, turn, size) on the
 * outer one isn't overwritten by the animation's own transform.
 */
export function OverlayView({ overlay }: { overlay: StoryOverlay }) {
  return (
    <div className={overlayClass({ ...overlay, anim: undefined })} style={overlayStyle(overlay)}>
      {overlay.anim ? <span className={styles[`anim_${overlay.anim}`]}>{overlay.text}</span> : overlay.text}
    </div>
  );
}

/** The loudness bars of an audio story; the part already played is lit. */
export function Waveform({ bars, progress = 0 }: { bars?: number[]; progress?: number }) {
  const data = bars && bars.length ? bars : Array.from({ length: 40 }, (_, i) => 30 + ((i * 37) % 50));
  return (
    <div className={styles.waveform} aria-hidden="true">
      {data.map((v, i) => (
        <span key={i} className={i / data.length < progress ? styles.waveformPlayed : undefined} style={{ height: `${8 + v * 0.9}%` }} />
      ))}
    </div>
  );
}

export interface StoryFrameProps {
  meta: StoryMeta;
  /** Decrypted photo, video or audio (blob: URL); null while loading. */
  mediaUrl: string | null;
  /** The video or audio element, so the viewer can play, pause and read its time. */
  mediaRef?: RefObject<HTMLMediaElement>;
  muted?: boolean;
  onVideoEnded?: () => void;
  onMediaReady?: () => void;
  /** The composer draws its own (editable) text and stickers. */
  editing?: boolean;
  /** Audio: how much has played, 0 to 1. */
  audioProgress?: number;
  /** The subtitle to show now. */
  caption?: string | null;
  children?: ReactNode;
  label?: string;
}

/** One story, drawn from its decrypted details: the background or media, then text and stickers on top. */
export function StoryFrame({ meta, mediaUrl, mediaRef, muted = true, onVideoEnded, onMediaReady, editing, audioProgress = 0, caption, children, label }: StoryFrameProps) {
  const lookBackground = meta.kind === "text" || meta.kind === "audio";
  const bg = lookBackground ? { background: `var(--story-bg-${meta.background ?? (meta.kind === "audio" ? "night" : "sunset")})` } : undefined;
  return (
    <div className={styles.frame} style={bg} role="img" aria-label={label}>
      {meta.kind === "image" && mediaUrl && (
        <>
          <img className={styles.backdrop} src={mediaUrl} alt="" aria-hidden="true" />
          <img className={styles.media} src={mediaUrl} alt="" onLoad={onMediaReady} draggable={false} />
        </>
      )}
      {meta.kind === "video" && mediaUrl && (
        <video
          ref={mediaRef as RefObject<HTMLVideoElement> | undefined}
          className={styles.media}
          src={mediaUrl}
          playsInline
          autoPlay
          muted={muted}
          onLoadedData={onMediaReady}
          onEnded={onVideoEnded}
          disablePictureInPicture
          controlsList="nodownload noplaybackrate"
        />
      )}
      {meta.kind === "audio" && (
        <div className={styles.audioFace}>
          {meta.text && <div className={`${styles.audioTitle} ${styles[`font_${meta.font ?? "classic"}`]}`}>{meta.text}</div>}
          <Waveform bars={meta.waveform} progress={audioProgress} />
          {mediaUrl && (
            <audio
              ref={mediaRef as RefObject<HTMLAudioElement> | undefined}
              src={mediaUrl}
              autoPlay
              muted={muted}
              onLoadedData={onMediaReady}
              onEnded={onVideoEnded}
              controlsList="nodownload noplaybackrate"
            />
          )}
        </div>
      )}
      {(meta.kind === "image" || meta.kind === "video") && !mediaUrl && <div className={styles.loading} aria-label="Loading" />}
      {meta.kind === "text" && !editing && <div className={`${styles.storyText} ${styles[`font_${meta.font ?? "classic"}`]}`}>{meta.text}</div>}
      {!editing && meta.overlays.map((o, i) => <OverlayView key={i} overlay={o} />)}
      {caption && (
        <div className={styles.caption} role="status" aria-live="polite">
          {caption}
        </div>
      )}
      {children}
    </div>
  );
}
