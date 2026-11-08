// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { useAppState } from "../state/AppStateContext.js";
import { type GroupTile, useGroupCall } from "../state/GroupCallContext.js";
import { tierAtLeast } from "../types.js";
import { Avatar } from "./Avatar.js";
import styles from "./CallOverlay.module.css";
import grid from "./GroupCallOverlay.module.css";
import {
  ChevronLeftIcon,
  KeyIcon,
  LockIcon,
  MaximizeIcon,
  MicIcon,
  MicOffIcon,
  MonitorUpIcon,
  PhoneIcon,
  PhoneOffIcon,
  UsersIcon,
  VideoIcon,
  VideoOffIcon,
} from "./icons24.js";
import { useDialogFocus } from "./useDialogFocus.js";

function formatDuration(ms: number): string {
  const total = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return `${h > 0 ? `${h}:` : ""}${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

function useNow(active: boolean): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!active) return;
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [active]);
  return now;
}

function TrackVideo({ track, className, label }: { track: MediaStreamTrack; className: string; label: string }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    el.srcObject = new MediaStream([track]);
    el.play?.()?.catch?.(() => {});
    return () => {
      el.srcObject = null;
    };
  }, [track]);
  // Always muted: call audio plays from GroupCallProvider's own elements.
  return <video ref={ref} className={className} autoPlay playsInline muted aria-label={label} />;
}

function Tile({ tile }: { tile: GroupTile }) {
  const name = tile.source === "screen" ? `${tile.handle}'s screen` : tile.handle;
  return (
    <div className={`${grid.tile} ${tile.speaking ? grid.speaking : ""}`}>
      {tile.track ? (
        <TrackVideo
          track={tile.track}
          className={`${grid.video} ${tile.source === "screen" ? grid.screenVideo : ""} ${tile.isSelf && tile.source === "camera" ? grid.mirrored : ""}`}
          label={tile.source === "screen" ? name : `${tile.handle}'s video`}
        />
      ) : (
        <Avatar avatarUrl={tile.avatarUrl} seed={tile.userId ?? tile.key} size={88} />
      )}
      <span className={grid.tileLabel}>
        {tile.source === "screen" ? <MonitorUpIcon size={14} /> : tile.micMuted ? <MicOffIcon size={14} /> : null}
        {name}
      </span>
    </div>
  );
}

function RoundButton({
  label,
  onClick,
  children,
  toggled,
  disabled,
  title,
}: {
  label: string;
  onClick: () => void;
  children: React.ReactNode;
  toggled?: boolean;
  disabled?: boolean;
  title?: string;
}) {
  return (
    <button
      type="button"
      className={`${styles.roundButton} ${toggled ? styles.toggledOn : ""}`}
      onClick={onClick}
      aria-label={label}
      aria-pressed={toggled}
      disabled={disabled}
      title={title ?? label}
    >
      {children}
    </button>
  );
}

/**
 * Everything group-call-related on screen: the incoming pop-up, the call
 * screen (a grid of people, or a shared screen with everyone beside it),
 * the bar shown while minimized, and the closing toast. While the app is
 * locked only a minimal bar shows.
 */
export function GroupCallOverlay({ locked }: { locked: boolean }) {
  const { state } = useAppState();
  const groupCall = useGroupCall();
  const { call } = groupCall;
  const now = useNow(call?.phase === "active" || call?.phase === "reconnecting");
  const overlayRef = useRef<HTMLDivElement>(null);
  const popupRef = useRef<HTMLDivElement>(null);
  const isIncoming = call?.phase === "incoming";
  useDialogFocus(popupRef, null, isIncoming && !locked);

  if (!call) return null;

  const isVideoCall = call.kind === "video";
  const kindLabel = isVideoCall ? "group video call" : "group voice call";
  const people = call.tiles.filter((t) => t.source === "camera").length;
  const elapsed = formatDuration(now - (call.connectedAt ?? now));
  const status =
    call.phase === "connecting"
      ? "Connecting…"
      : call.phase === "reconnecting"
        ? "Reconnecting…"
        : call.phase === "active"
          ? `${people} ${people === 1 ? "person" : "people"} · ${elapsed}`
          : "";

  if (call.phase === "ended") {
    return (
      <div className={styles.toast} role="status">
        <span>{call.endedMessage ?? "Call ended"}</span>
        <button type="button" className={styles.toastClose} onClick={groupCall.dismiss} aria-label="Dismiss">
          ×
        </button>
      </div>
    );
  }

  if (locked) {
    return (
      <div className={styles.lockedBar} role="status">
        <span className={styles.lockedTitle}>{isIncoming ? "Incoming group call" : "Group call in progress"}</span>
        {!isIncoming && <span className={styles.lockedStatus}>{status}</span>}
        <div className={styles.lockedActions}>
          {isIncoming && (
            <button type="button" className={`${styles.smallRound} ${styles.accept}`} onClick={groupCall.accept} aria-label="Join">
              <PhoneIcon size={18} />
            </button>
          )}
          <button
            type="button"
            className={`${styles.smallRound} ${styles.danger}`}
            onClick={isIncoming ? groupCall.decline : groupCall.leave}
            aria-label={isIncoming ? "Decline" : "Leave call"}
          >
            <PhoneOffIcon size={18} />
          </button>
        </div>
      </div>
    );
  }

  if (isIncoming) {
    return (
      <div ref={popupRef} className={styles.popup} role="alertdialog" aria-modal="false" aria-labelledby="group-call-title" aria-describedby="group-call-detail">
        <div className={styles.popupTop}>
          <span className={styles.e2ePill}>
            <LockIcon size={12} /> End-to-end encrypted
          </span>
        </div>
        <div className={styles.popupIdentity}>
          <span className={styles.popupAvatar}>
            <Avatar avatarUrl={null} seed={call.conversationId} size={72} />
          </span>
          <span className={styles.popupText}>
            <span id="group-call-title" className={`${styles.popupName} type-display`}>
              {call.groupLabel}
            </span>
            <span id="group-call-detail" className={styles.popupDetail}>
              {call.hostHandle ? `${call.hostHandle} started a ${kindLabel}` : `Incoming ${kindLabel}…`}
            </span>
          </span>
        </div>
        <div className={styles.popupActions}>
          <button type="button" className={`${styles.pill} ${styles.pillDanger}`} onClick={groupCall.decline} aria-label="Decline">
            <PhoneOffIcon size={18} /> Decline
          </button>
          <button type="button" className={`${styles.pill} ${styles.pillAccept}`} onClick={groupCall.accept} aria-label="Join">
            {isVideoCall ? <VideoIcon size={18} /> : <PhoneIcon size={18} />} Join
          </button>
        </div>
      </div>
    );
  }

  if (call.minimized) {
    return (
      <div className={styles.miniBar} role="status">
        <span className={styles.liveDot} />
        <span className={styles.miniText}>
          {call.groupLabel} · {status}
        </span>
        <button type="button" className={styles.miniReturn} onClick={() => groupCall.setMinimized(false)}>
          Return to call
        </button>
        <button type="button" className={`${styles.smallRound} ${styles.danger}`} onClick={groupCall.leave} aria-label="Leave call">
          <PhoneOffIcon size={18} />
        </button>
      </div>
    );
  }

  const canShare = !!state.identity && tierAtLeast(state.identity.subscriptionTier, "premium");
  const shareDisabled = !canShare || !call.keyReady || typeof navigator.mediaDevices?.getDisplayMedia !== "function";
  const shareTitle = canShare ? (call.sharingScreen ? "Stop sharing" : "Share your screen") : "Screen sharing is a Premium feature";
  const screens = call.tiles.filter((t) => t.source === "screen");
  // Someone else's shared screen first: yours you can already see.
  const spotlight = screens.find((t) => !t.isSelf) ?? screens[0];
  const others = call.tiles.filter((t) => t !== spotlight);

  return (
    <div ref={overlayRef} className={styles.overlay} role="dialog" aria-modal="true" aria-label={`${kindLabel} in ${call.groupLabel}`}>
      <div className={styles.stage}>
        <div className={styles.videoTop}>
          <button type="button" className={styles.squareButton} aria-label="Back to chat" onClick={() => groupCall.setMinimized(true)}>
            <ChevronLeftIcon />
          </button>
          <div className={styles.nameChip}>
            <span className={styles.nameChipName}>
              <UsersIcon size={14} /> {call.groupLabel}
            </span>
            <span className={styles.nameChipStatus}>
              <LockIcon size={12} /> Encrypted · {status}
            </span>
          </div>
          <span className={styles.flex} />
          <button
            type="button"
            className={styles.squareButton}
            aria-label="Full screen"
            onClick={() => (document.fullscreenElement ? document.exitFullscreen() : overlayRef.current?.requestFullscreen?.())?.catch?.(() => {})}
          >
            <MaximizeIcon />
          </button>
        </div>

        <div className={grid.body}>
          {call.sharingScreen && (
            <div className={styles.shareBanner} role="status">
              <MonitorUpIcon size={16} /> You're sharing your screen
              <button type="button" onClick={groupCall.toggleScreenShare}>
                Stop
              </button>
            </div>
          )}
          {call.phase !== "connecting" && !call.keyReady && (
            <div className={grid.waiting} role="status">
              <KeyIcon size={14} /> Waiting for the call key… nothing is sent until it arrives
            </div>
          )}
          {call.notice && (
            <div className={grid.warning} role="alert">
              {call.notice}
            </div>
          )}
          {spotlight ? (
            <div className={grid.spotlight}>
              <div className={grid.spotlightMain}>
                <Tile tile={spotlight} />
              </div>
              <div className={grid.strip}>
                {others.map((t) => (
                  <Tile key={t.key} tile={t} />
                ))}
              </div>
            </div>
          ) : (
            <div className={grid.grid}>
              {call.tiles.map((t) => (
                <Tile key={t.key} tile={t} />
              ))}
            </div>
          )}
        </div>

        {call.sas && (
          <div className={styles.videoSas}>
            <div className={styles.sas}>
              <KeyIcon size={18} />
              <span className={styles.sasText}>Everyone in the call should see these same words</span>
              <span className={styles.sasWords}>
                {call.sas.map((w) => (
                  <span key={w} className={styles.sasWord}>
                    {w}
                  </span>
                ))}
              </span>
            </div>
          </div>
        )}

        <div className={styles.dockWrap}>
          <div className={styles.dock}>
            <RoundButton label={call.muted ? "Unmute" : "Mute"} onClick={groupCall.toggleMute} toggled={call.muted} disabled={!call.keyReady}>
              {call.muted ? <MicOffIcon /> : <MicIcon />}
            </RoundButton>
            {isVideoCall && (
              <RoundButton
                label={call.cameraOff ? "Turn camera on" : "Turn camera off"}
                onClick={groupCall.toggleCamera}
                toggled={call.cameraOff}
                disabled={!call.keyReady}
              >
                {call.cameraOff ? <VideoOffIcon /> : <VideoIcon />}
              </RoundButton>
            )}
            <RoundButton
              label={call.sharingScreen ? "Stop sharing your screen" : "Share your screen"}
              onClick={groupCall.toggleScreenShare}
              toggled={call.sharingScreen}
              disabled={shareDisabled}
              title={shareTitle}
            >
              <MonitorUpIcon />
            </RoundButton>
            <span className={styles.dockDivider} />
            <button type="button" className={styles.endPill} onClick={groupCall.leave} aria-label="Leave call">
              <PhoneOffIcon size={20} /> Leave
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
