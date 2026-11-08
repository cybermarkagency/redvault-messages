// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// One-to-one voice/video calls with screen sharing, between contacts or
// through a call link.
//
//   - Media: WebRTC, encrypted end to end by DTLS-SRTP. Forced through the
//     TURN relay (iceTransportPolicy "relay"), so neither person ever
//     learns the other's IP address; the relay only forwards ciphertext.
//   - Setup: the offer and answer (with each side's DTLS fingerprint) are
//     sealed end to end: between the two identity keys for a contact
//     (lib/callCrypto.ts), or with the link's secret for a call link
//     (lib/callLinkCrypto.ts). The server relays them but can't read,
//     alter or forge them, so it can't slip itself or anyone else into the
//     call. Once connected, both screens show the same four words derived
//     from both fingerprints (lib/sas.ts) to say aloud as a final check.
//   - Signaling: the live channel (LiveContext), an HTTPS long poll.
//   - Plans: one-to-one voice and video calls with a contact are free for
//     everyone. Call links need Pro (voice) or Premium (video), and a link's
//     owner hosts; screen sharing needs Premium. The server enforces each.
//   - History: kept only on this device, encrypted (lib/callHistory.ts).
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import * as api from "../lib/api.js";
import { gatherComplete, relayExpiryMs, renewalDelayMs } from "../lib/turnRenewal.js";
import { CALLS_UNAVAILABLE_MESSAGE, webRtcAvailable } from "../lib/torRouting.js";
import { ApiError, type CallKind, type IceServerDto, type LiveEventDto, type SealedSignalDto } from "../lib/api.js";
import { CallSignalError, openCallSignal, sealCallSignal, type CallSignal } from "../lib/callCrypto.js";
import { type CallOutcome, recordCall } from "../lib/callHistory.js";
import { openLinkSignal, openOwnedLink, sealLinkSignal } from "../lib/callLinkCrypto.js";
import { readPrefs, writePrefs } from "../lib/localPrefs.js";
import { startRingtone } from "../lib/ringtone.js";
import { sasWords, sdpFingerprint } from "../lib/sas.js";
import { type DMConversation, tierAtLeast } from "../types.js";
import { useAppState } from "./AppStateContext.js";
import { useLive } from "./LiveContext.js";

export type CallPhase = "outgoing" | "incoming" | "connecting" | "active" | "reconnecting" | "ended";

export interface CallView {
  /** The 1:1 chat for a call between contacts; null for a call link. */
  conversationId: string | null;
  linkId: string | null;
  linkName: string | null;
  peerUserId: string;
  peerHandle: string;
  peerAvatarUrl: string | null;
  kind: CallKind;
  direction: "outgoing" | "incoming";
  phase: CallPhase;
  muted: boolean;
  cameraOff: boolean;
  sharingScreen: boolean;
  /** The other side is sharing their screen (confirmed by the server, which checks their plan). */
  remoteSharing: boolean;
  connectedAt: number | null;
  /** Four words from both DTLS fingerprints, once connected. */
  sas: string[] | null;
  /** The call screen is tucked away ("Back to chat"); a bar shows it's still on. */
  minimized: boolean;
  /** Video call: the in-call chat panel is open. */
  chatOpen: boolean;
  /** Why the call ended, or what went wrong, for the closing banner. */
  endedMessage: string | null;
}

export interface MediaDeviceOption {
  deviceId: string;
  label: string;
}

interface CallContextValue {
  call: CallView | null;
  /** False when this server has no TURN relay configured (or is too old for calls): hide the call buttons. */
  callsAvailable: boolean;
  localStream: MediaStream | null;
  remoteStream: MediaStream | null;
  /** Bumped whenever a remote track arrives, so video elements re-attach. */
  remoteVersion: number;
  devices: { microphones: MediaDeviceOption[]; speakers: MediaDeviceOption[]; cameras: MediaDeviceOption[] };
  startCall(conversation: DMConversation, kind: CallKind): void;
  /** Ask to join someone's call link (you hold its secret from the URL). */
  joinLink(link: { id: string; secret: Uint8Array; name: string; kind: CallKind; ownerId: string; ownerHandle: string; ownerAvatarUrl: string | null }): void;
  accept(options?: { audioOnly?: boolean }): void;
  decline(): void;
  hangUp(): void;
  toggleMute(): void;
  toggleCamera(): void;
  toggleScreenShare(): void;
  setMinimized(minimized: boolean): void;
  toggleChat(): void;
  refreshDevices(): void;
  setMicrophone(deviceId: string): void;
  setSpeaker(deviceId: string): void;
  setCamera(deviceId: string): void;
  dismiss(): void;
}

interface Session {
  callId: string | null;
  conversationId: string | null;
  linkId: string | null;
  linkName: string | null;
  /** Call links: the link's secret, which seals the offer and answer. */
  linkSecret: Uint8Array | null;
  peerUserId: string;
  peerHandle: string;
  /** Calls between contacts: the contact's identity key, which seals the offer and answer. */
  peerPublicKey: Uint8Array | null;
  kind: CallKind;
  direction: "outgoing" | "incoming";
  iceServers: IceServerDto[];
  offerSdp: string | null;
  pc: RTCPeerConnection | null;
  local: MediaStream | null;
  screen: MediaStream | null;
  remote: MediaStream;
  closed: boolean;
  stopTone: (() => void) | null;
  disconnectTimer: ReturnType<typeof setTimeout> | null;
  /** The caller's next TURN credential renewal (lib/turnRenewal.ts). */
  renewTimer: ReturnType<typeof setTimeout> | null;
  /** The newest ICE restart signal accepted (its sentAt), so a replayed one is ignored. */
  lastRestartAt: number;
  startedAt: number;
  connectedAt: number | null;
}

const CallCtx = createContext<CallContextValue | null>(null);

/** How long ICE gathering may take; relay candidates normally arrive well within this. */
const GATHER_TIMEOUT_MS = 6000;
/** A dropped connection gets this long to come back before the call ends. */
const RECONNECT_GRACE_MS = 15_000;
const ENDED_BANNER_MS = 4000;

const ENDED_MESSAGES: Record<api.CallEndReason, { outgoing: string; incoming: string }> = {
  hangup: { outgoing: "Call ended", incoming: "Call ended" },
  declined: { outgoing: "Call declined", incoming: "Call declined" },
  missed: { outgoing: "No answer", incoming: "Missed call" },
  busy: { outgoing: "They're on another call", incoming: "Call ended" },
  failed: { outgoing: "Call dropped", incoming: "Call dropped" },
};

function errorMessage(err: unknown, fallback: string): string {
  if (err instanceof ApiError || err instanceof CallSignalError) return err.message;
  return fallback;
}

function transceiverFor(pc: RTCPeerConnection, kind: "audio" | "video"): RTCRtpTransceiver | undefined {
  return pc.getTransceivers().find((t) => t.receiver.track.kind === kind);
}

function audioConstraint(deviceId: string | null): MediaTrackConstraints | boolean {
  return deviceId ? { deviceId: { exact: deviceId } } : true;
}

async function getMedia(kind: CallKind, prefs: { microphoneId: string | null; cameraId: string | null }): Promise<{ stream: MediaStream; cameraOff: boolean }> {
  const audio = audioConstraint(prefs.microphoneId);
  if (kind === "video") {
    try {
      return { stream: await navigator.mediaDevices.getUserMedia({ audio, video: audioConstraint(prefs.cameraId) }), cameraOff: false };
    } catch {
      // No camera (or it's refused): join with voice only.
    }
  }
  try {
    return { stream: await navigator.mediaDevices.getUserMedia({ audio, video: false }), cameraOff: kind === "video" };
  } catch (err) {
    // A remembered microphone that's gone: fall back to the default.
    if (prefs.microphoneId) return { stream: await navigator.mediaDevices.getUserMedia({ audio: true, video: false }), cameraOff: kind === "video" };
    throw err;
  }
}

function blankView(fields: Pick<CallView, "conversationId" | "linkId" | "linkName" | "peerUserId" | "peerHandle" | "peerAvatarUrl" | "kind" | "direction" | "phase">): CallView {
  return {
    ...fields,
    muted: false,
    cameraOff: fields.kind === "voice",
    sharingScreen: false,
    remoteSharing: false,
    connectedAt: null,
    sas: null,
    minimized: false,
    chatOpen: false,
    endedMessage: null,
  };
}

export function CallProvider({ children }: { children: ReactNode }) {
  const { state } = useAppState();
  const live = useLive();
  const [call, setCall] = useState<CallView | null>(null);
  const [localStream, setLocalStream] = useState<MediaStream | null>(null);
  const [remoteStream, setRemoteStream] = useState<MediaStream | null>(null);
  const [remoteVersion, setRemoteVersion] = useState(0);
  const [devices, setDevices] = useState<CallContextValue["devices"]>({ microphones: [], speakers: [], cameras: [] });
  const sessionRef = useRef<Session | null>(null);
  const stateRef = useRef(state);
  stateRef.current = state;
  const endedTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const remoteAudioRef = useRef<HTMLAudioElement>(null);

  const patch = useCallback((changes: Partial<CallView>) => {
    setCall((current) => (current ? { ...current, ...changes } : current));
  }, []);

  const prefs = () => readPrefs(stateRef.current.identity?.userId);

  /** Writes the finished call to this device's (encrypted) history. */
  const remember = useCallback((session: Session, outcome: CallOutcome) => {
    const identity = stateRef.current.identity;
    if (!identity || !session.callId) return;
    recordCall(identity.userId, identity.privateKey, {
      id: session.callId,
      peerUserId: session.peerUserId,
      peerHandle: session.peerHandle,
      conversationId: session.conversationId,
      linkName: session.linkName,
      kind: session.kind,
      direction: session.direction,
      outcome,
      startedAt: session.startedAt,
      durationSec: session.connectedAt ? Math.round((Date.now() - session.connectedAt) / 1000) : 0,
    });
  }, []);

  /** Tears the session down and leaves a closing banner (or nothing). */
  const finish = useCallback(
    (message: string | null, outcome?: CallOutcome) => {
      const session = sessionRef.current;
      sessionRef.current = null;
      if (session) {
        session.closed = true;
        session.stopTone?.();
        if (session.disconnectTimer) clearTimeout(session.disconnectTimer);
        if (session.renewTimer) clearTimeout(session.renewTimer);
        session.local?.getTracks().forEach((t) => t.stop());
        session.screen?.getTracks().forEach((t) => t.stop());
        session.pc?.close();
        remember(session, outcome ?? (session.connectedAt ? "answered" : session.direction === "incoming" ? "missed" : "unanswered"));
      }
      setLocalStream(null);
      setRemoteStream(null);
      if (endedTimer.current) clearTimeout(endedTimer.current);
      if (message) {
        setCall((current) =>
          current ? { ...current, phase: "ended", endedMessage: message, sharingScreen: false, remoteSharing: false, minimized: false } : current
        );
        endedTimer.current = setTimeout(() => setCall(null), ENDED_BANNER_MS);
      } else {
        setCall(null);
      }
    },
    [remember]
  );

  /** Ends our side and tells the server (which tells the other side). */
  const hangUp = useCallback(() => {
    const session = sessionRef.current;
    if (session?.callId) api.hangUpCall(session.callId).catch(() => {});
    finish(session ? "Call ended" : null);
  }, [finish]);

  const fail = useCallback(
    (message: string) => {
      const session = sessionRef.current;
      if (session?.callId) api.hangUpCall(session.callId).catch(() => {});
      finish(message, session?.connectedAt ? "answered" : "failed");
    },
    [finish]
  );

  const createPeerConnection = useCallback(
    (session: Session): RTCPeerConnection => {
      const pc = new RTCPeerConnection({
        iceServers: session.iceServers.map((s) => ({ urls: s.urls, username: s.username, credential: s.credential })),
        // Relay only: never reveal either person's IP address to the other.
        iceTransportPolicy: "relay",
        bundlePolicy: "max-bundle",
        rtcpMuxPolicy: "require",
      });
      pc.ontrack = (event) => {
        if (session.closed) return;
        session.remote.addTrack(event.track);
        setRemoteVersion((v) => v + 1);
      };
      pc.onconnectionstatechange = () => {
        if (session.closed || sessionRef.current !== session) return;
        if (pc.connectionState === "connected") {
          session.stopTone?.();
          session.stopTone = null;
          if (session.disconnectTimer) clearTimeout(session.disconnectTimer);
          session.disconnectTimer = null;
          session.connectedAt ??= Date.now();
          // The caller keeps the TURN credentials fresh for a long call (only one side restarts ICE).
          if (session.direction === "outgoing" && !session.renewTimer) scheduleRenewalRef.current(session);
          const mine = sdpFingerprint(pc.localDescription?.sdp);
          const theirs = sdpFingerprint(pc.remoteDescription?.sdp);
          setCall((c) =>
            c ? { ...c, phase: "active", connectedAt: session.connectedAt, sas: mine && theirs ? sasWords(mine, theirs) : c.sas } : c
          );
        } else if (pc.connectionState === "disconnected") {
          patch({ phase: "reconnecting" });
          if (!session.disconnectTimer) session.disconnectTimer = setTimeout(() => fail("Call dropped"), RECONNECT_GRACE_MS);
        } else if (pc.connectionState === "failed") {
          fail("Call dropped");
        }
      };
      session.pc = pc;
      return pc;
    },
    [fail, patch]
  );

  /** Our half of the SDP, once every relay candidate is in it. */
  const localSdp = useCallback(async (pc: RTCPeerConnection, afterRestart = false): Promise<string> => {
    await gatherComplete(pc, GATHER_TIMEOUT_MS, afterRestart);
    const sdp = pc.localDescription?.sdp ?? "";
    if (!/ typ relay /.test(sdp)) throw new Error("relay_unreachable");
    return sdp;
  }, []);

  const seal = useCallback((session: Session, signal: CallSignal): SealedSignalDto => {
    const identity = stateRef.current.identity!;
    return session.linkSecret ? sealLinkSignal(signal, session.linkSecret) : sealCallSignal(signal, session.peerPublicKey!, identity.privateKey);
  }, []);

  /** The signal's conversation field: the contact chat, or the call link. */
  const signalConversation = (session: Session) => (session.linkId ? `link:${session.linkId}` : session.conversationId!);

  /**
   * Long calls (lib/turnRenewal.ts): before the TURN credentials expire, fetch fresh ones and
   * restart ICE with them. The restart offer is sealed like the first one; the media keeps flowing
   * on the old relay until the new path is up.
   */
  const scheduleRenewalRef = useRef<(session: Session) => void>(() => {});
  scheduleRenewalRef.current = (session: Session) => {
    if (session.renewTimer) clearTimeout(session.renewTimer);
    const expiry = relayExpiryMs(session.iceServers);
    if (expiry === null) return;
    session.renewTimer = setTimeout(() => void renewRelay(session), renewalDelayMs(expiry));
  };
  const renewRelay = async (session: Session) => {
    session.renewTimer = null;
    const pc = session.pc;
    const identity = stateRef.current.identity;
    if (session.closed || !pc || !session.callId || !identity) return;
    try {
      const fresh = await api.renewCallRelay(session.callId);
      if (session.closed) return;
      session.iceServers = fresh.iceServers;
      pc.setConfiguration({
        ...pc.getConfiguration(),
        iceServers: fresh.iceServers.map((s) => ({ urls: s.urls, username: s.username, credential: s.credential })),
        iceTransportPolicy: "relay",
      });
      await pc.setLocalDescription(await pc.createOffer({ iceRestart: true }));
      const sdp = await localSdp(pc, true);
      if (session.closed) return;
      const sealed = seal(session, {
        v: 1,
        type: "restart-offer",
        callId: session.callId,
        conversationId: signalConversation(session),
        from: identity.userId,
        to: session.peerUserId,
        kind: session.kind,
        sdp,
        sentAt: Date.now(),
      });
      await api.sendCallSignal(session.callId, sealed);
      scheduleRenewalRef.current(session);
    } catch {
      // The current relay keeps working until its credentials lapse: try again in a minute.
      if (!session.closed) session.renewTimer = setTimeout(() => void renewRelay(session), 60_000);
    }
  };

  /** An ICE restart from the other side (callee: their restart offer; caller: the answer to ours). */
  const handleRestart = async (session: Session, sealed: SealedSignalDto) => {
    const pc = session.pc;
    const identity = stateRef.current.identity;
    if (!pc || !session.callId || !identity || session.closed) return;
    const type = session.direction === "incoming" ? ("restart-offer" as const) : ("restart-answer" as const);
    let signal: CallSignal;
    try {
      const expected = { type, callId: session.callId, from: session.peerUserId, to: identity.userId };
      signal = session.linkSecret
        ? openLinkSignal(sealed, session.linkSecret, { ...expected, linkId: session.linkId! })
        : openCallSignal(sealed, session.peerPublicKey!, identity.privateKey, {
            ...expected,
            conversationId: session.conversationId!,
            kind: session.kind,
          });
    } catch {
      return; // not from them, not for this call, or a replay: ignored
    }
    if (signal.sentAt <= session.lastRestartAt) return;
    // The DTLS identity (what the safety words are made of) must not change mid-call.
    if (sdpFingerprint(signal.sdp) !== sdpFingerprint(pc.remoteDescription?.sdp)) return;
    session.lastRestartAt = signal.sentAt;
    try {
      if (type === "restart-answer") {
        await pc.setRemoteDescription({ type: "answer", sdp: signal.sdp });
        return;
      }
      // Our side gets fresh credentials too, so both relays' allocations outlive the old ones.
      const fresh = await api.renewCallRelay(session.callId);
      session.iceServers = fresh.iceServers;
      pc.setConfiguration({
        ...pc.getConfiguration(),
        iceServers: fresh.iceServers.map((s) => ({ urls: s.urls, username: s.username, credential: s.credential })),
        iceTransportPolicy: "relay",
      });
      await pc.setRemoteDescription({ type: "offer", sdp: signal.sdp });
      await pc.setLocalDescription(await pc.createAnswer());
      const sdp = await localSdp(pc, true);
      if (session.closed) return;
      const answer = seal(session, {
        v: 1,
        type: "restart-answer",
        callId: session.callId,
        conversationId: signalConversation(session),
        from: identity.userId,
        to: session.peerUserId,
        kind: session.kind,
        sdp,
        sentAt: Date.now(),
      });
      await api.sendCallSignal(session.callId, answer);
    } catch {
      // The call carries on over the current relay; the caller tries again later.
    }
  };

  /** Shared by calling a contact and asking to join a link: media, TURN, offer. */
  const runOutgoing = useCallback(
    async (session: Session, reserve: () => Promise<{ callId: string; iceServers: IceServerDto[] }>) => {
      const identity = stateRef.current.identity!;
      let media;
      try {
        media = await getMedia(session.kind, prefs());
      } catch {
        if (!session.closed) finish("Couldn't use your microphone. Allow access to call.", "failed");
        return;
      }
      if (session.closed) return media.stream.getTracks().forEach((t) => t.stop());
      session.local = media.stream;
      setLocalStream(media.stream);
      patch({ cameraOff: session.kind === "voice" || media.cameraOff });

      try {
        const reserved = await reserve();
        if (session.closed) {
          api.hangUpCall(reserved.callId).catch(() => {});
          return;
        }
        session.callId = reserved.callId;
        session.iceServers = reserved.iceServers;
        const pc = createPeerConnection(session);
        const [mic] = media.stream.getAudioTracks();
        const [camera] = media.stream.getVideoTracks();
        pc.addTransceiver(mic, { direction: "sendrecv", streams: [media.stream] });
        // Always a video slot, so screen sharing can start later without renegotiating.
        pc.addTransceiver(camera ?? "video", { direction: "sendrecv", streams: [media.stream] });
        await pc.setLocalDescription(await pc.createOffer());
        const sdp = await localSdp(pc);
        if (session.closed) return;
        const offer = seal(session, {
          v: 1,
          type: "offer",
          callId: reserved.callId,
          conversationId: session.linkId ? `link:${session.linkId}` : session.conversationId!,
          from: identity.userId,
          to: session.peerUserId,
          kind: session.kind,
          sdp,
          sentAt: Date.now(),
        });
        await api.sendCallOffer(reserved.callId, offer);
        if (!session.closed) session.stopTone = startRingtone("outgoing");
      } catch (err) {
        if (session.closed) return;
        if (err instanceof Error && err.message === "relay_unreachable") fail("Couldn't reach the call relay. Try again in a minute.");
        else fail(errorMessage(err, "Couldn't start the call. Try again."));
      }
    },
    [createPeerConnection, fail, finish, localSdp, patch, seal]
  );

  const newSession = (fields: Partial<Session> & Pick<Session, "peerUserId" | "peerHandle" | "kind" | "direction">): Session => ({
    callId: null,
    conversationId: null,
    linkId: null,
    linkName: null,
    linkSecret: null,
    peerPublicKey: null,
    iceServers: [],
    offerSdp: null,
    pc: null,
    local: null,
    screen: null,
    remote: new MediaStream(),
    closed: false,
    stopTone: null,
    disconnectTimer: null,
    renewTimer: null,
    lastRestartAt: 0,
    startedAt: Date.now(),
    connectedAt: null,
    ...fields,
  });

  const startCall = useCallback(
    (conversation: DMConversation, kind: CallKind) => {
      const identity = stateRef.current.identity;
      if (sessionRef.current || !identity || !conversation.userId || !conversation.publicKey) return;
      if (endedTimer.current) clearTimeout(endedTimer.current);
      if (!webRtcAvailable()) {
        // Tor Browser (or any browser without WebRTC): say why, and don't ring anyone.
        setCall({
          ...blankView({
            conversationId: conversation.id,
            linkId: null,
            linkName: null,
            peerUserId: conversation.userId,
            peerHandle: conversation.handle,
            peerAvatarUrl: conversation.avatarUrl ?? null,
            kind,
            direction: "outgoing",
            phase: "ended",
          }),
          endedMessage: CALLS_UNAVAILABLE_MESSAGE,
        });
        endedTimer.current = setTimeout(() => setCall(null), ENDED_BANNER_MS * 2);
        return;
      }
      const session = newSession({
        conversationId: conversation.id,
        peerUserId: conversation.userId,
        peerHandle: conversation.handle,
        peerPublicKey: conversation.publicKey,
        kind,
        direction: "outgoing",
      });
      sessionRef.current = session;
      setRemoteStream(session.remote);
      setCall(
        blankView({
          conversationId: conversation.id,
          linkId: null,
          linkName: null,
          peerUserId: conversation.userId,
          peerHandle: conversation.handle,
          peerAvatarUrl: conversation.avatarUrl ?? null,
          kind,
          direction: "outgoing",
          phase: "outgoing",
        })
      );
      void runOutgoing(session, () => api.startCall(conversation.id, kind));
    },
    [runOutgoing]
  );

  const joinLink = useCallback<CallContextValue["joinLink"]>(
    (link) => {
      const identity = stateRef.current.identity;
      if (sessionRef.current || !identity) return;
      if (endedTimer.current) clearTimeout(endedTimer.current);
      if (!webRtcAvailable()) {
        setCall({
          ...blankView({
            conversationId: null,
            linkId: link.id,
            linkName: link.name,
            peerUserId: link.ownerId,
            peerHandle: link.ownerHandle,
            peerAvatarUrl: link.ownerAvatarUrl,
            kind: link.kind,
            direction: "outgoing",
            phase: "ended",
          }),
          endedMessage: CALLS_UNAVAILABLE_MESSAGE,
        });
        endedTimer.current = setTimeout(() => setCall(null), ENDED_BANNER_MS * 2);
        return;
      }
      const session = newSession({
        linkId: link.id,
        linkName: link.name,
        linkSecret: link.secret,
        peerUserId: link.ownerId,
        peerHandle: link.ownerHandle,
        kind: link.kind,
        direction: "outgoing",
      });
      sessionRef.current = session;
      setRemoteStream(session.remote);
      setCall(
        blankView({
          conversationId: null,
          linkId: link.id,
          linkName: link.name,
          peerUserId: link.ownerId,
          peerHandle: link.ownerHandle,
          peerAvatarUrl: link.ownerAvatarUrl,
          kind: link.kind,
          direction: "outgoing",
          phase: "outgoing",
        })
      );
      void runOutgoing(session, () => api.joinCallLink(link.id));
    },
    [runOutgoing]
  );

  const accept = useCallback(
    (options?: { audioOnly?: boolean }) => {
      const session = sessionRef.current;
      const identity = stateRef.current.identity;
      if (!session || session.direction !== "incoming" || !session.callId || !session.offerSdp || !identity || session.pc) return;
      if (!webRtcAvailable()) return fail(CALLS_UNAVAILABLE_MESSAGE);
      session.stopTone?.();
      session.stopTone = null;
      patch({ phase: "connecting", minimized: false });
      const callId = session.callId;

      (async () => {
        let media;
        try {
          media = await getMedia(options?.audioOnly ? "voice" : session.kind, prefs());
        } catch {
          if (!session.closed) fail("Couldn't use your microphone. Allow access to answer.");
          return;
        }
        if (session.closed) return media.stream.getTracks().forEach((t) => t.stop());
        session.local = media.stream;
        setLocalStream(media.stream);
        patch({ cameraOff: session.kind === "voice" || options?.audioOnly === true || media.cameraOff });

        try {
          const pc = createPeerConnection(session);
          await pc.setRemoteDescription({ type: "offer", sdp: session.offerSdp! });
          const [mic] = media.stream.getAudioTracks();
          const [camera] = media.stream.getVideoTracks();
          for (const transceiver of pc.getTransceivers()) {
            transceiver.direction = "sendrecv";
            const kind = transceiver.receiver.track.kind;
            await transceiver.sender.replaceTrack(kind === "audio" ? mic : (camera ?? null));
            transceiver.sender.setStreams?.(media.stream);
          }
          await pc.setLocalDescription(await pc.createAnswer());
          const sdp = await localSdp(pc);
          if (session.closed) return;
          const answer = seal(session, {
            v: 1,
            type: "answer",
            callId,
            conversationId: session.linkId ? `link:${session.linkId}` : session.conversationId!,
            from: identity.userId,
            to: session.peerUserId,
            kind: session.kind,
            sdp,
            sentAt: Date.now(),
          });
          await api.sendCallAnswer(callId, answer);
        } catch (err) {
          if (session.closed) return;
          if (err instanceof Error && err.message === "relay_unreachable") fail("Couldn't reach the call relay. Try again in a minute.");
          else fail(errorMessage(err, "Couldn't answer the call."));
        }
      })();
    },
    [createPeerConnection, fail, localSdp, patch, seal]
  );

  const decline = useCallback(() => {
    const session = sessionRef.current;
    if (!session || session.direction !== "incoming") return;
    if (session.callId) api.declineCall(session.callId).catch(() => {});
    finish(null, "declined");
  }, [finish]);

  const toggleMute = useCallback(() => {
    const session = sessionRef.current;
    const mic = session?.local?.getAudioTracks()[0];
    if (!mic) return;
    mic.enabled = !mic.enabled;
    patch({ muted: !mic.enabled });
  }, [patch]);

  const toggleCamera = useCallback(() => {
    const session = sessionRef.current;
    const camera = session?.local?.getVideoTracks()[0];
    if (!camera) return;
    camera.enabled = !camera.enabled;
    patch({ cameraOff: !camera.enabled });
  }, [patch]);

  const stopScreenShare = useCallback(async () => {
    const session = sessionRef.current;
    if (!session?.screen || !session.pc) return;
    session.screen.getTracks().forEach((t) => t.stop());
    session.screen = null;
    const camera = session.local?.getVideoTracks()[0] ?? null;
    await transceiverFor(session.pc, "video")?.sender.replaceTrack(camera).catch(() => {});
    patch({ sharingScreen: false });
    if (session.callId) api.setCallScreenShare(session.callId, false).catch(() => {});
  }, [patch]);

  const toggleScreenShare = useCallback(() => {
    const session = sessionRef.current;
    const identity = stateRef.current.identity;
    if (!session?.pc || !session.callId || !identity) return;
    if (session.screen) {
      void stopScreenShare();
      return;
    }
    if (!tierAtLeast(identity.subscriptionTier, "premium")) return;
    const callId = session.callId;
    (async () => {
      let display: MediaStream;
      try {
        display = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: false });
      } catch {
        return; // picker cancelled
      }
      const [track] = display.getVideoTracks();
      if (session.closed || !track) return display.getTracks().forEach((t) => t.stop());
      try {
        // The server checks the plan and tells the other side to show it.
        await api.setCallScreenShare(callId, true);
      } catch (err) {
        display.getTracks().forEach((t) => t.stop());
        patch({ endedMessage: errorMessage(err, "Couldn't share your screen.") });
        return;
      }
      track.contentHint = "detail";
      session.screen = display;
      await transceiverFor(session.pc!, "video")?.sender.replaceTrack(track);
      track.addEventListener("ended", () => void stopScreenShare());
      patch({ sharingScreen: true, endedMessage: null });
    })();
  }, [patch, stopScreenShare]);

  const refreshDevices = useCallback(() => {
    if (!navigator.mediaDevices?.enumerateDevices) return;
    navigator.mediaDevices
      .enumerateDevices()
      .then((list) => {
        const pick = (kind: MediaDeviceKind, fallback: string) =>
          list
            .filter((d) => d.kind === kind && d.deviceId)
            .map((d, i) => ({ deviceId: d.deviceId, label: d.label || `${fallback} ${i + 1}` }));
        setDevices({ microphones: pick("audioinput", "Microphone"), speakers: pick("audiooutput", "Speaker"), cameras: pick("videoinput", "Camera") });
      })
      .catch(() => {});
  }, []);

  /** Swaps a live track for one from another device, without renegotiating. */
  const swapTrack = useCallback(async (kind: "audio" | "video", deviceId: string) => {
    const session = sessionRef.current;
    if (!session?.local) return;
    const fresh = await navigator.mediaDevices.getUserMedia(kind === "audio" ? { audio: { deviceId: { exact: deviceId } } } : { video: { deviceId: { exact: deviceId } } });
    const [track] = kind === "audio" ? fresh.getAudioTracks() : fresh.getVideoTracks();
    const old = kind === "audio" ? session.local.getAudioTracks()[0] : session.local.getVideoTracks()[0];
    if (old) {
      track.enabled = old.enabled;
      session.local.removeTrack(old);
      old.stop();
    }
    session.local.addTrack(track);
    if (session.pc && !(kind === "video" && session.screen)) await transceiverFor(session.pc, kind)?.sender.replaceTrack(track);
    setLocalStream(new MediaStream(session.local.getTracks()));
  }, []);

  const setMicrophone = useCallback(
    (deviceId: string) => {
      const userId = stateRef.current.identity?.userId;
      if (userId) writePrefs(userId, { microphoneId: deviceId });
      swapTrack("audio", deviceId).catch(() => patch({ endedMessage: "Couldn't switch to that microphone." }));
    },
    [patch, swapTrack]
  );

  const setCamera = useCallback(
    (deviceId: string) => {
      const userId = stateRef.current.identity?.userId;
      if (userId) writePrefs(userId, { cameraId: deviceId });
      if (sessionRef.current?.local?.getVideoTracks().length) {
        swapTrack("video", deviceId).catch(() => patch({ endedMessage: "Couldn't switch to that camera." }));
      }
    },
    [patch, swapTrack]
  );

  const applySpeaker = useCallback((deviceId: string | null) => {
    const el = remoteAudioRef.current as (HTMLAudioElement & { setSinkId?: (id: string) => Promise<void> }) | null;
    if (el?.setSinkId && deviceId) el.setSinkId(deviceId).catch(() => {});
  }, []);

  const setSpeaker = useCallback(
    (deviceId: string) => {
      const userId = stateRef.current.identity?.userId;
      if (userId) writePrefs(userId, { speakerId: deviceId });
      applySpeaker(deviceId);
    },
    [applySpeaker]
  );

  const setMinimized = useCallback((minimized: boolean) => patch({ minimized }), [patch]);
  const toggleChat = useCallback(() => setCall((c) => (c ? { ...c, chatOpen: !c.chatOpen } : c)), []);

  const dismiss = useCallback(() => {
    if (endedTimer.current) clearTimeout(endedTimer.current);
    if (!sessionRef.current) setCall(null);
  }, []);

  /** Finds the 1:1 chat a call belongs to; right after sign-in the chat list may still be loading. */
  const findContact = useCallback(async (conversationId: string, peerUserId: string): Promise<DMConversation | null> => {
    for (let attempt = 0; attempt < 20; attempt++) {
      const match = stateRef.current.conversations.find(
        (c): c is DMConversation => c.kind === "dm" && c.id === conversationId && c.userId === peerUserId
      );
      if (match?.publicKey) return match;
      await new Promise((r) => setTimeout(r, 300));
    }
    return null;
  }, []);

  const showBlocked = useCallback((view: CallView, message: string) => {
    setCall({ ...view, phase: "ended", endedMessage: message });
    if (endedTimer.current) clearTimeout(endedTimer.current);
    endedTimer.current = setTimeout(() => setCall(null), ENDED_BANNER_MS * 2);
  }, []);

  const ring = useCallback((session: Session, view: CallView) => {
    const p = prefs();
    const silent = !p.ringtone || p.muted.includes(session.peerUserId);
    session.stopTone = silent ? null : startRingtone("incoming");
    sessionRef.current = session;
    setRemoteStream(session.remote);
    setCall(view);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const handleEvent = useCallback(
    async (event: LiveEventDto) => {
      const identity = stateRef.current.identity;
      if (!identity || event.type === "typing" || event.type === "share_upload") return;
      const session = sessionRef.current;

      if (event.type === "incoming") {
        if (session) return; // the server doesn't ring someone who's on a call
        if (endedTimer.current) clearTimeout(endedTimer.current);

        if (event.linkId) {
          // Someone opened one of your call links.
          let link = null;
          try {
            const mine = await api.listCallLinks();
            const dto = mine.find((l) => l.id === event.linkId);
            link = dto ? openOwnedLink(dto, identity.publicKey, identity.privateKey) : null;
          } catch {
            link = null;
          }
          let guest: api.UserResult | null = null;
          try {
            guest = await api.getUser(event.callerId);
          } catch {
            guest = null;
          }
          const view = blankView({
            conversationId: null,
            linkId: event.linkId,
            linkName: link?.name ?? null,
            peerUserId: event.callerId,
            peerHandle: guest?.username ?? "Someone",
            peerAvatarUrl: guest?.avatarUrl ?? null,
            kind: event.kind,
            direction: "incoming",
            phase: "incoming",
          });
          if (!link || sessionRef.current) return;
          let offerSdp: string;
          try {
            offerSdp = openLinkSignal(event.offer, link.secret, {
              type: "offer",
              callId: event.callId,
              linkId: link.id,
              from: event.callerId,
              to: identity.userId,
            }).sdp;
          } catch {
            api.declineCall(event.callId).catch(() => {});
            showBlocked(view, `Blocked a request to join "${link.name}" that wasn't made with the link.`);
            return;
          }
          ring(
            newSession({
              callId: event.callId,
              linkId: link.id,
              linkName: link.name,
              linkSecret: link.secret,
              peerUserId: event.callerId,
              peerHandle: view.peerHandle,
              kind: event.kind,
              direction: "incoming",
              iceServers: event.iceServers,
              offerSdp,
            }),
            view
          );
          return;
        }

        const contact = event.conversationId ? await findContact(event.conversationId, event.callerId) : null;
        if (!contact || sessionRef.current) return;
        const view = blankView({
          conversationId: contact.id,
          linkId: null,
          linkName: null,
          peerUserId: event.callerId,
          peerHandle: contact.handle,
          peerAvatarUrl: contact.avatarUrl ?? null,
          kind: event.kind,
          direction: "incoming",
          phase: "incoming",
        });
        let offerSdp: string;
        try {
          offerSdp = openCallSignal(event.offer as SealedSignalDto, contact.publicKey!, identity.privateKey, {
            type: "offer",
            callId: event.callId,
            conversationId: contact.id,
            from: event.callerId,
            to: identity.userId,
            kind: event.kind,
          }).sdp;
        } catch {
          // Not sealed by this contact's key: never ring for it.
          api.declineCall(event.callId).catch(() => {});
          showBlocked(view, `Blocked a call that couldn't be verified as coming from ${contact.handle}'s key.`);
          return;
        }
        ring(
          newSession({
            callId: event.callId,
            conversationId: contact.id,
            peerUserId: event.callerId,
            peerHandle: contact.handle,
            peerPublicKey: contact.publicKey!,
            kind: event.kind,
            direction: "incoming",
            iceServers: event.iceServers,
            offerSdp,
          }),
          view
        );
        return;
      }

      if (!session || session.callId !== event.callId) return;

      if (event.type === "answered") {
        if (session.direction !== "outgoing" || !session.pc) return;
        session.stopTone?.();
        session.stopTone = null;
        patch({ phase: "connecting" });
        try {
          const expected = { type: "answer" as const, callId: session.callId, from: session.peerUserId, to: identity.userId };
          const answer = session.linkSecret
            ? openLinkSignal(event.answer, session.linkSecret, { ...expected, linkId: session.linkId! })
            : openCallSignal(event.answer, session.peerPublicKey!, identity.privateKey, {
                ...expected,
                conversationId: session.conversationId!,
                kind: session.kind,
              });
          await session.pc.setRemoteDescription({ type: "answer", sdp: answer.sdp });
        } catch (err) {
          fail(err instanceof CallSignalError ? `Hung up: ${err.message}` : "Couldn't connect the call.");
        }
      } else if (event.type === "signal") {
        void handleRestart(session, event.signal);
      } else if (event.type === "screen") {
        patch({ remoteSharing: event.on });
      } else if (event.type === "ended") {
        const messages = ENDED_MESSAGES[event.reason] ?? ENDED_MESSAGES.hangup;
        const outcome: CallOutcome | undefined =
          event.reason === "declined" ? "declined" : event.reason === "missed" ? (session.direction === "incoming" ? "missed" : "unanswered") : undefined;
        finish(messages[session.direction], outcome);
      }
    },
    [fail, findContact, finish, patch, ring, showBlocked]
  );

  const handleEventRef = useRef(handleEvent);
  handleEventRef.current = handleEvent;
  useEffect(() => live.subscribe((event) => handleEventRef.current(event)), [live]);

  // Signing out (or switching accounts) ends any call.
  const signedInUserId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  useEffect(() => {
    if (!signedInUserId && sessionRef.current) hangUp();
  }, [signedInUserId, hangUp]);

  // Closing the tab hangs up, so the other side isn't left talking to no one.
  useEffect(() => {
    const onUnload = () => {
      const callId = sessionRef.current?.callId;
      if (callId) api.hangUpCallOnUnload(callId);
    };
    window.addEventListener("pagehide", onUnload);
    return () => window.removeEventListener("pagehide", onUnload);
  }, []);

  // Remote audio plays from here, outside the call screen, so it carries on
  // while the app is locked or the screen is showing something else.
  useEffect(() => {
    const el = remoteAudioRef.current;
    if (!el) return;
    if (el.srcObject !== remoteStream) el.srcObject = remoteStream;
    if (remoteStream) {
      applySpeaker(readPrefs(stateRef.current.identity?.userId).speakerId);
      el.play?.()?.catch?.(() => {});
    }
  }, [remoteStream, remoteVersion, applySpeaker]);

  // Device lists (labels only appear once the browser has granted media access).
  const inCall = call !== null && call.phase !== "ended";
  useEffect(() => {
    if (!inCall || !navigator.mediaDevices) return;
    refreshDevices();
    navigator.mediaDevices.addEventListener?.("devicechange", refreshDevices);
    return () => navigator.mediaDevices.removeEventListener?.("devicechange", refreshDevices);
  }, [inCall, localStream, refreshDevices]);

  const value = useMemo<CallContextValue>(
    () => ({
      call,
      callsAvailable: live.callsAvailable,
      localStream,
      remoteStream,
      remoteVersion,
      devices,
      startCall,
      joinLink,
      accept,
      decline,
      hangUp,
      toggleMute,
      toggleCamera,
      toggleScreenShare,
      setMinimized,
      toggleChat,
      refreshDevices,
      setMicrophone,
      setSpeaker,
      setCamera,
      dismiss,
    }),
    [
      call,
      live.callsAvailable,
      localStream,
      remoteStream,
      remoteVersion,
      devices,
      startCall,
      joinLink,
      accept,
      decline,
      hangUp,
      toggleMute,
      toggleCamera,
      toggleScreenShare,
      setMinimized,
      toggleChat,
      refreshDevices,
      setMicrophone,
      setSpeaker,
      setCamera,
      dismiss,
    ]
  );

  return (
    <CallCtx.Provider value={value}>
      {children}
      <audio ref={remoteAudioRef} autoPlay hidden />
    </CallCtx.Provider>
  );
}

export function useCall(): CallContextValue {
  const ctx = useContext(CallCtx);
  if (!ctx) throw new Error("useCall must be used within a CallProvider");
  return ctx;
}
