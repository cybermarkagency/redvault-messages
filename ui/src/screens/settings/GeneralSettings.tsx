// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { Avatar } from "../../components/Avatar.js";
import { OpenSourceNotice } from "../../components/OpenSourceNotice.js";
import { HeadphonesIcon, LockIcon, MicIcon, SpeakerIcon, VideoIcon } from "../../components/icons24.js";
import { ToggleSwitch } from "../../components/ToggleSwitch.js";
import { toggleIn, useLocalPrefs } from "../../lib/localPrefs.js";
import { startRingtone } from "../../lib/ringtone.js";
import { useAppState } from "../../state/AppStateContext.js";
import { type DMConversation, tierAtLeast } from "../../types.js";
import styles from "./SettingsPages.module.css";

/** Ringtone and muted contacts. Nothing here reaches the server. */
export function NotificationsSettings() {
  const { state } = useAppState();
  const [prefs, setPrefs] = useLocalPrefs(state.identity?.userId);
  const muted = prefs.muted
    .map((id) => state.conversations.find((c): c is DMConversation => c.kind === "dm" && c.userId === id))
    .filter((c): c is DMConversation => !!c);

  return (
    <>
      <p className={styles.lead}>
        The web app doesn't use push notifications: nothing about your messages or calls is ever handed to a browser push service. It rings
        and shows unread counts while it's open.
      </p>
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Ring for incoming calls</span>
            <span className={styles.rowDetail}>Play a ringtone while a call is ringing</span>
          </span>
          <ToggleSwitch label="Ring for incoming calls" checked={prefs.ringtone} onChange={(v) => setPrefs({ ringtone: v })} />
        </div>
        <button
          type="button"
          className={styles.row}
          onClick={() => {
            const stop = startRingtone("incoming");
            setTimeout(stop, 2200);
          }}
        >
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Play the ringtone</span>
          </span>
        </button>
      </div>
      <h3 className={`${styles.subhead} type-caption`}>MUTED CONTACTS</h3>
      <div className={styles.group}>
        {muted.length === 0 ? (
          <div className={styles.row}>
            <span className={styles.rowDetail}>Nobody's muted. Mute someone from their chat details or contact page: their calls won't ring and their chat won't count as unread.</span>
          </div>
        ) : (
          muted.map((c) => (
            <div key={c.id} className={styles.row}>
              <span className={styles.person}>
                <Avatar avatarUrl={c.avatarUrl} seed={c.handle} size={32} />
                {c.handle}
              </span>
              <button type="button" className={styles.smallButton} onClick={() => setPrefs({ muted: toggleIn(prefs.muted, c.userId!) })}>
                Unmute
              </button>
            </div>
          ))
        )}
      </div>
    </>
  );
}

function useDevices() {
  const [devices, setDevices] = useState<MediaDeviceInfo[]>([]);
  const refresh = () => {
    navigator.mediaDevices
      ?.enumerateDevices?.()
      .then(setDevices)
      .catch(() => {});
  };
  useEffect(() => {
    refresh();
    navigator.mediaDevices?.addEventListener?.("devicechange", refresh);
    return () => navigator.mediaDevices?.removeEventListener?.("devicechange", refresh);
  }, []);
  return { devices, refresh };
}

function DeviceSelect({ label, icon, kind, value, onChange, devices }: {
  label: string;
  icon: React.ReactNode;
  kind: MediaDeviceKind;
  value: string | null;
  onChange: (id: string | null) => void;
  devices: MediaDeviceInfo[];
}) {
  const options = devices.filter((d) => d.kind === kind && d.deviceId);
  return (
    <label className={styles.row}>
      <span className={styles.person}>
        {icon}
        <span className={styles.rowTitle}>{label}</span>
      </span>
      <select className={styles.select} value={value ?? ""} onChange={(e) => onChange(e.target.value || null)} aria-label={label}>
        <option value="">System default</option>
        {options.map((d, i) => (
          <option key={d.deviceId} value={d.deviceId}>
            {d.label || `${label} ${i + 1}`}
          </option>
        ))}
      </select>
    </label>
  );
}

/** Which calls your plan includes, the devices calls use, and a microphone / camera check. */
export function CallsAudioSettings() {
  const { state } = useAppState();
  const [prefs, setPrefs] = useLocalPrefs(state.identity?.userId);
  const { devices, refresh } = useDevices();
  const tier = state.identity?.subscriptionTier ?? "free";
  const [testing, setTesting] = useState(false);
  const [level, setLevel] = useState(0);
  const [testError, setTestError] = useState<string | null>(null);
  const videoRef = useRef<HTMLVideoElement>(null);
  const streamRef = useRef<MediaStream | null>(null);

  useEffect(() => () => streamRef.current?.getTracks().forEach((t) => t.stop()), []);

  async function toggleTest() {
    if (testing) {
      streamRef.current?.getTracks().forEach((t) => t.stop());
      streamRef.current = null;
      setTesting(false);
      setLevel(0);
      return;
    }
    setTestError(null);
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: prefs.microphoneId ? { deviceId: { exact: prefs.microphoneId } } : true,
        video: prefs.cameraId ? { deviceId: { exact: prefs.cameraId } } : true,
      });
      streamRef.current = stream;
      setTesting(true);
      refresh(); // labels appear once access is granted
      if (videoRef.current) videoRef.current.srcObject = stream;
      const ctx = new AudioContext();
      const analyser = ctx.createAnalyser();
      ctx.createMediaStreamSource(stream).connect(analyser);
      const data = new Uint8Array(analyser.frequencyBinCount);
      const tick = () => {
        if (!streamRef.current) {
          ctx.close().catch(() => {});
          return;
        }
        analyser.getByteTimeDomainData(data);
        const peak = data.reduce((m, v) => Math.max(m, Math.abs(v - 128)), 0);
        setLevel(Math.min(1, peak / 64));
        requestAnimationFrame(tick);
      };
      tick();
    } catch {
      setTestError("Couldn't open your microphone or camera. Check the browser's permission for this site.");
    }
  }

  return (
    <>
      <div className={styles.planCard}>
        <span className={styles.rowTitle}>Your plan: {tier === "premium" ? "Premium" : tier === "pro" ? "Pro" : "Free"}</span>
        <span className={styles.rowDetail}>
          {tier === "premium"
            ? "One-to-one voice and video calls, screen sharing, and voice and video call links."
            : tier === "pro"
              ? "One-to-one voice and video calls, and voice call links. Video links and screen sharing need Premium."
              : "One-to-one voice and video calls are free. Call links need Pro; screen sharing needs Premium."}
        </span>
        <span className={styles.secure}>
          <LockIcon size={13} /> Every call is end-to-end encrypted and relayed, so nobody learns anyone's IP address.
        </span>
      </div>

      <div className={styles.group}>
        <DeviceSelect label="Microphone" icon={<MicIcon size={19} />} kind="audioinput" value={prefs.microphoneId} onChange={(id) => setPrefs({ microphoneId: id })} devices={devices} />
        <DeviceSelect label="Speaker" icon={<SpeakerIcon size={19} />} kind="audiooutput" value={prefs.speakerId} onChange={(id) => setPrefs({ speakerId: id })} devices={devices} />
        {tierAtLeast(tier, "premium") && (
          <DeviceSelect label="Camera" icon={<VideoIcon size={19} />} kind="videoinput" value={prefs.cameraId} onChange={(id) => setPrefs({ cameraId: id })} devices={devices} />
        )}
      </div>

      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.person}>
            <HeadphonesIcon size={19} />
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>Check microphone and camera</span>
              <span className={styles.rowDetail}>Nothing is recorded or sent: it only plays back here.</span>
            </span>
          </span>
          <button type="button" className={styles.smallButton} onClick={toggleTest}>
            {testing ? "Stop" : "Start"}
          </button>
        </div>
        {testing && (
          <div className={styles.testArea}>
            <video ref={videoRef} className={styles.preview} autoPlay playsInline muted aria-label="Camera preview" />
            <div className={styles.meter} role="meter" aria-label="Microphone level" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(level * 100)}>
              <div className={styles.meterFill} style={{ width: `${Math.round(level * 100)}%` }} />
            </div>
          </div>
        )}
        {testError && (
          <div className={styles.row}>
            <span className={styles.error}>{testError}</span>
          </div>
        )}
      </div>
    </>
  );
}

export function AboutSettings() {
  return (
    <>
      <div className={styles.planCard}>
        <span className={styles.rowTitle}>Redvault Messages · web</span>
        <span className={styles.rowDetail}>
          End-to-end encrypted messages, files and calls. Keys are made and kept on your devices; the server only ever relays ciphertext.
        </span>
      </div>
      <div className={styles.group}>
        <a className={styles.row} href="privacy.html" target="_blank" rel="noopener noreferrer">
          <span className={styles.rowTitle}>Privacy policy</span>
        </a>
        <a className={styles.row} href="terms.html" target="_blank" rel="noopener noreferrer">
          <span className={styles.rowTitle}>Terms of Service</span>
        </a>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>No tracking</span>
            <span className={styles.rowDetail}>No analytics, no third-party scripts or fonts, no IP or device details kept.</span>
          </span>
        </div>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>What stays on this device</span>
            <span className={styles.rowDetail}>
              Call history (encrypted), favorites, muted contacts, which safety numbers you verified, and your audio and video device choices.
            </span>
          </span>
        </div>
      </div>
      <OpenSourceNotice />
    </>
  );
}
