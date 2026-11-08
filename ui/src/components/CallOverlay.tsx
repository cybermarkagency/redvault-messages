// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { readPrefs } from "../lib/localPrefs.js";
import { useAppState } from "../state/AppStateContext.js";
import { type CallView, type MediaDeviceOption, useCall } from "../state/CallContext.js";
import { type DMConversation, tierAtLeast } from "../types.js";
import { Avatar } from "./Avatar.js";
import styles from "./CallOverlay.module.css";
import {
  ChatIcon,
  ChevronDownIcon,
  ChevronLeftIcon,
  HeadphonesIcon,
  KeyIcon,
  LockIcon,
  MaximizeIcon,
  MicIcon,
  MicOffIcon,
  MonitorUpIcon,
  PhoneIcon,
  PhoneOffIcon,
  ShieldCheckIcon,
  SignalIcon,
  SlidersIcon,
  SpeakerIcon,
  VideoIcon,
  VideoOffIcon,
} from "./icons24.js";
import { MiniChat } from "./MiniChat.js";
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

function StreamVideo({ stream, version, className, label }: { stream: MediaStream | null; version: number; className: string; label: string }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    if (el.srcObject !== stream) el.srcObject = stream;
    el.play?.()?.catch?.(() => {});
  }, [stream, version]);
  // Always muted: call audio plays from CallProvider's own element.
  return <video ref={ref} className={className} autoPlay playsInline muted aria-label={label} />;
}

function ControlButton({
  label,
  onClick,
  children,
  toggled,
  danger,
  disabled,
  title,
  showLabel,
}: {
  label: string;
  onClick: () => void;
  children: React.ReactNode;
  toggled?: boolean;
  danger?: boolean;
  disabled?: boolean;
  title?: string;
  showLabel?: string;
}) {
  return (
    <div className={styles.control}>
      <button
        type="button"
        className={`${styles.roundButton} ${toggled ? styles.toggledOn : ""} ${danger ? styles.danger : ""}`}
        onClick={onClick}
        aria-label={label}
        aria-pressed={toggled}
        disabled={disabled}
        title={title ?? label}
      >
        {children}
      </button>
      {showLabel && <span className={styles.controlLabel}>{showLabel}</span>}
    </div>
  );
}

function DevicePicker({
  icon,
  label,
  options,
  value,
  onChange,
}: {
  icon: React.ReactNode;
  label: string;
  options: MediaDeviceOption[];
  value: string | null;
  onChange: (id: string) => void;
}) {
  if (options.length === 0) return null;
  const current = options.find((o) => o.deviceId === value) ?? options[0];
  return (
    <label className={styles.picker}>
      {icon}
      <span className={styles.pickerLabel}>
        {label}: {current.label}
      </span>
      <ChevronDownIcon size={16} />
      <select value={current.deviceId} onChange={(e) => onChange(e.target.value)} aria-label={label}>
        {options.map((o) => (
          <option key={o.deviceId} value={o.deviceId}>
            {o.label}
          </option>
        ))}
      </select>
    </label>
  );
}

function SecurityLine({ call, contact }: { call: CallView; contact: DMConversation | undefined }) {
  if (call.linkId) {
    return (
      <span className={styles.securityWarn}>
        <LockIcon size={14} /> Call link · sealed with the link's secret
      </span>
    );
  }
  if (contact?.keyChanged) return <span className={styles.securityWarn}>Safety number changed: verify before trusting this call</span>;
  if (contact?.verified)
    return (
      <span className={styles.securityOk}>
        <ShieldCheckIcon size={14} /> Safety number verified
      </span>
    );
  return <span className={styles.securityWarn}>Not verified: compare safety numbers</span>;
}

/**
 * Everything call-related on screen: the incoming pop-up (non-blocking,
 * bottom right), the full-window voice and video call screens, the bar
 * shown while a call is minimized, and the closing toast. While the app
 * is locked only a minimal bar shows (no name, no video).
 */
export function CallOverlay({ locked }: { locked: boolean }) {
  const { state, dispatch, contactsById } = useAppState();
  const calls = useCall();
  const { call, localStream, remoteStream, remoteVersion, devices } = calls;
  const now = useNow(call?.phase === "active" || call?.phase === "reconnecting");
  const [settingsOpen, setSettingsOpen] = useState(false);
  const overlayRef = useRef<HTMLDivElement>(null);
  const popupRef = useRef<HTMLDivElement>(null);
  const isIncoming = call?.phase === "incoming";
  useDialogFocus(popupRef, null, isIncoming && !locked);

  if (!call) return null;

  const contact = call.conversationId ? contactsById.get(call.conversationId) : undefined;
  const handle = call.peerHandle;
  const firstName = handle.split(/[\s.·]/)[0];
  const isVideoCall = call.kind === "video";
  const canShare = !!state.identity && tierAtLeast(state.identity.subscriptionTier, "premium");
  const inCall = call.phase === "connecting" || call.phase === "active" || call.phase === "reconnecting";
  const kindLabel = isVideoCall ? "video call" : "voice call";
  const elapsed = formatDuration(now - (call.connectedAt ?? now));
  const status =
    call.phase === "outgoing"
      ? call.linkId
        ? `Waiting for ${firstName} to let you in…`
        : "Calling…"
      : call.phase === "connecting"
        ? "Connecting…"
        : call.phase === "reconnecting"
          ? "Reconnecting…"
          : call.phase === "active"
            ? elapsed
            : "";
  const prefsRaw = readPrefs(state.identity?.userId);

  if (call.phase === "ended") {
    return (
      <div className={styles.toast} role="status">
        <span>{call.endedMessage ?? "Call ended"}</span>
        <button type="button" className={styles.toastClose} onClick={calls.dismiss} aria-label="Dismiss">
          ×
        </button>
      </div>
    );
  }

  if (locked) {
    return (
      <div className={styles.lockedBar} role="status">
        <span className={styles.lockedTitle}>{isIncoming ? "Incoming call" : "Call in progress"}</span>
        {!isIncoming && <span className={styles.lockedStatus}>{status}</span>}
        <div className={styles.lockedActions}>
          {isIncoming && (
            <button type="button" className={`${styles.smallRound} ${styles.accept}`} onClick={() => calls.accept()} aria-label="Answer">
              <PhoneIcon size={18} />
            </button>
          )}
          <button
            type="button"
            className={`${styles.smallRound} ${styles.danger}`}
            onClick={isIncoming ? calls.decline : calls.hangUp}
            aria-label={isIncoming ? "Decline" : "Hang up"}
          >
            <PhoneOffIcon size={18} />
          </button>
        </div>
      </div>
    );
  }

  // Incoming: a pop-up in the corner that doesn't block the app.
  if (isIncoming) {
    const asking = call.linkId !== null;
    return (
      <div
        ref={popupRef}
        className={styles.popup}
        role="alertdialog"
        aria-modal="false"
        aria-labelledby="incoming-call-title"
        aria-describedby="incoming-call-detail"
      >
        <div className={styles.popupTop}>
          <span className={styles.e2ePill}>
            <LockIcon size={12} /> End-to-end encrypted
          </span>
          {call.conversationId && (
            <button
              type="button"
              className={styles.textButton}
              onClick={() => {
                calls.decline();
                dispatch({ type: "OPEN_THREAD", conversationId: call.conversationId! });
              }}
            >
              Message instead
            </button>
          )}
        </div>
        <div className={styles.popupIdentity}>
          <span className={styles.popupAvatar}>
            <Avatar avatarUrl={call.peerAvatarUrl} seed={handle} size={72} />
          </span>
          <span className={styles.popupText}>
            <span id="incoming-call-title" className={`${styles.popupName} type-display`}>
              {handle}
            </span>
            <span id="incoming-call-detail" className={styles.popupDetail}>
              {asking ? `Asks to join “${call.linkName ?? "your call link"}” · ${kindLabel}` : `Incoming ${kindLabel}…`}
            </span>
            <SecurityLine call={call} contact={contact} />
          </span>
        </div>
        <div className={styles.popupActions}>
          <button type="button" className={`${styles.pill} ${styles.pillDanger}`} onClick={calls.decline} aria-label={asking ? "Deny" : "Decline"}>
            <PhoneOffIcon size={18} /> {asking ? "Deny" : "Decline"}
          </button>
          {isVideoCall && (
            <button type="button" className={styles.pill} onClick={() => calls.accept({ audioOnly: true })} aria-label="Answer with audio only">
              <PhoneIcon size={18} /> Audio
            </button>
          )}
          <button type="button" className={`${styles.pill} ${styles.pillAccept}`} onClick={() => calls.accept()} aria-label={asking ? "Admit" : "Answer"}>
            {isVideoCall ? <VideoIcon size={18} /> : <PhoneIcon size={18} />} {asking ? "Admit" : "Accept"}
          </button>
        </div>
      </div>
    );
  }

  // Minimized ("Back to chat"): a bar that brings the call back.
  if (call.minimized) {
    return (
      <div className={styles.miniBar} role="status">
        <span className={styles.liveDot} />
        <span className={styles.miniText}>
          {isVideoCall ? "Video" : "Voice"} call with {handle} · {status}
        </span>
        <button type="button" className={styles.miniReturn} onClick={() => calls.setMinimized(false)}>
          Return to call
        </button>
        <button type="button" className={`${styles.smallRound} ${styles.danger}`} onClick={calls.hangUp} aria-label="Hang up">
          <PhoneOffIcon size={18} />
        </button>
      </div>
    );
  }

  const showRemoteVideo = inCall && (isVideoCall || call.remoteSharing);
  const showLocalPreview = inCall && ((isVideoCall && !call.cameraOff) || call.sharingScreen);
  const chat = call.chatOpen && contact ? <MiniChat conversation={contact} onClose={calls.toggleChat} /> : null;
  const shareDisabled = !canShare || call.phase !== "active" || typeof navigator.mediaDevices?.getDisplayMedia !== "function";
  const shareTitle = canShare ? (call.sharingScreen ? "Stop sharing" : "Share your screen") : "Screen sharing is a Premium feature";

  const pickers = (
    <div className={styles.pickers}>
      <DevicePicker
        icon={<HeadphonesIcon size={16} />}
        label="Microphone"
        options={devices.microphones}
        value={prefsRaw.microphoneId ?? null}
        onChange={calls.setMicrophone}
      />
      <DevicePicker icon={<SpeakerIcon size={16} />} label="Speaker" options={devices.speakers} value={prefsRaw.speakerId ?? null} onChange={calls.setSpeaker} />
      {isVideoCall && (
        <DevicePicker icon={<VideoIcon size={16} />} label="Camera" options={devices.cameras} value={prefsRaw.cameraId ?? null} onChange={calls.setCamera} />
      )}
    </div>
  );

  const sas = call.sas && (
    <div className={styles.sas}>
      <KeyIcon size={18} />
      <span className={styles.sasText}>Say these words aloud to confirm the call is secure</span>
      <span className={styles.sasWords}>
        {call.sas.map((w) => (
          <span key={w} className={styles.sasWord}>
            {w}
          </span>
        ))}
      </span>
    </div>
  );

  // Video call (or a voice call where the other side shares their screen).
  if (showRemoteVideo) {
    return (
      <div ref={overlayRef} className={styles.overlay} role="dialog" aria-modal="true" aria-label={`${kindLabel} with ${handle}`}>
        <div className={styles.stage}>
          <StreamVideo stream={remoteStream} version={remoteVersion} className={styles.remoteVideo} label={`${handle}'s video`} />
          <div className={styles.videoTop}>
            <button type="button" className={styles.squareButton} aria-label="Back to chat" onClick={() => calls.setMinimized(true)}>
              <ChevronLeftIcon />
            </button>
            <div className={styles.nameChip}>
              <span className={styles.nameChipName}>{handle}</span>
              <span className={styles.nameChipStatus}>
                <LockIcon size={12} /> Encrypted · {status}
              </span>
            </div>
            <span className={styles.flex} />
            <span className={styles.squareButton} title="Connection: relayed, end-to-end encrypted" role="img" aria-label="Connection: relayed, end-to-end encrypted">
              <SignalIcon />
            </span>
            <button
              type="button"
              className={styles.squareButton}
              aria-label="Full screen"
              onClick={() => (document.fullscreenElement ? document.exitFullscreen() : overlayRef.current?.requestFullscreen?.())?.catch?.(() => {})}
            >
              <MaximizeIcon />
            </button>
          </div>
          {call.sharingScreen && (
            <div className={styles.shareBanner} role="status">
              <MonitorUpIcon size={16} /> You're sharing your screen
              <button type="button" onClick={calls.toggleScreenShare}>
                Stop
              </button>
            </div>
          )}
          {call.remoteSharing && <div className={styles.shareBanner}>{firstName} is sharing their screen</div>}
          {showLocalPreview && <StreamVideo stream={localStream} version={0} className={styles.localVideo} label="Your camera" />}
          {call.sas && <div className={styles.videoSas}>{sas}</div>}
          <div className={styles.dockWrap}>
            {settingsOpen && <div className={styles.settingsPopover}>{pickers}</div>}
            <div className={styles.dock}>
              <ControlButton label={call.muted ? "Unmute" : "Mute"} onClick={calls.toggleMute} toggled={call.muted}>
                {call.muted ? <MicOffIcon /> : <MicIcon />}
              </ControlButton>
              {isVideoCall && (
                <ControlButton label={call.cameraOff ? "Turn camera on" : "Turn camera off"} onClick={calls.toggleCamera} toggled={call.cameraOff}>
                  {call.cameraOff ? <VideoOffIcon /> : <VideoIcon />}
                </ControlButton>
              )}
              <ControlButton label={call.sharingScreen ? "Stop sharing your screen" : "Share your screen"} onClick={calls.toggleScreenShare} toggled={call.sharingScreen} disabled={shareDisabled} title={shareTitle}>
                <MonitorUpIcon />
              </ControlButton>
              {contact && (
                <ControlButton label={call.chatOpen ? "Close chat" : "Open chat"} onClick={calls.toggleChat} toggled={call.chatOpen}>
                  <ChatIcon />
                </ControlButton>
              )}
              <ControlButton label="Audio and video settings" onClick={() => setSettingsOpen((v) => !v)} toggled={settingsOpen}>
                <SlidersIcon />
              </ControlButton>
              <span className={styles.dockDivider} />
              <button type="button" className={styles.endPill} onClick={calls.hangUp} aria-label="Hang up">
                <PhoneOffIcon size={20} /> End
              </button>
            </div>
          </div>
        </div>
        {chat}
      </div>
    );
  }

  // Voice call (and calling / connecting for both kinds).
  return (
    <div ref={overlayRef} className={styles.overlay} role="dialog" aria-modal="true" aria-label={`${kindLabel} with ${handle}`}>
      <div className={styles.stage}>
        <div className={styles.voiceTop}>
          <button type="button" className={styles.backButton} onClick={() => calls.setMinimized(true)}>
            <ChevronLeftIcon size={18} /> Back to chat
          </button>
          <span className={styles.statusPill}>
            <LockIcon size={13} /> Encrypted {kindLabel}
            {call.phase === "active" ? ` · ${elapsed}` : ""}
          </span>
          <span className={styles.voiceTopRight} />
        </div>
        <div className={styles.voiceCenter}>
          <div className={`${styles.rings} ${call.phase === "active" ? styles.ringsActive : ""}`}>
            <Avatar avatarUrl={call.peerAvatarUrl} seed={handle} size={172} />
          </div>
          <div className={`${styles.voiceName} type-display`}>{handle}</div>
          <div className={styles.voiceStatus}>
            {call.phase === "active" ? "Connected · end-to-end encrypted · relayed" : status}
            {call.linkName ? ` · ${call.linkName}` : ""}
          </div>
          <SecurityLine call={call} contact={contact} />
          {sas}
          {call.remoteSharing === false && call.endedMessage && <div className={styles.notice}>{call.endedMessage}</div>}
        </div>
        <div className={styles.voiceBottom}>
          <div className={styles.controls}>
            <ControlButton label={call.muted ? "Unmute" : "Mute"} onClick={calls.toggleMute} toggled={call.muted} showLabel={call.muted ? "Unmute" : "Mute"}>
              {call.muted ? <MicOffIcon /> : <MicIcon />}
            </ControlButton>
            {isVideoCall ? (
              <ControlButton label={call.cameraOff ? "Turn camera on" : "Turn camera off"} onClick={calls.toggleCamera} toggled={call.cameraOff} showLabel="Video">
                {call.cameraOff ? <VideoOffIcon /> : <VideoIcon />}
              </ControlButton>
            ) : (
              <ControlButton label="Video" onClick={() => {}} disabled title="This is a voice call: start a video call to use your camera" showLabel="Video">
                <VideoIcon />
              </ControlButton>
            )}
            <ControlButton
              label={call.sharingScreen ? "Stop sharing your screen" : "Share your screen"}
              onClick={calls.toggleScreenShare}
              toggled={call.sharingScreen}
              disabled={shareDisabled}
              title={shareTitle}
              showLabel="Share"
            >
              <MonitorUpIcon />
            </ControlButton>
            {contact && (
              <ControlButton label={call.chatOpen ? "Close chat" : "Open chat"} onClick={calls.toggleChat} toggled={call.chatOpen} showLabel="Chat">
                <ChatIcon />
              </ControlButton>
            )}
            <ControlButton label="Hang up" onClick={calls.hangUp} danger showLabel="End">
              <PhoneOffIcon />
            </ControlButton>
          </div>
          {!canShare && inCall && <div className={styles.hint}>Screen sharing is included in Premium.</div>}
          {pickers}
        </div>
      </div>
      {chat}
    </div>
  );
}
