// Group voice/video calls with screen sharing, in group chats.
//
//   - Media: WebRTC through a LiveKit SFU on this app's own server, which
//     forwards each person's media to the others. Every frame is also
//     encrypted end to end in this browser with the call's room key
//     (lib/groupCallCrypto.ts), so the SFU only ever forwards ciphertext.
//     Nothing is published until this app holds a room key, and LiveKit
//     drops any frame it has no key for rather than sending it in the clear.
//   - Keys: the first person in the call makes a room key per epoch and
//     seals it to each participant's identity key; the server moves to a
//     new epoch whenever someone joins or leaves.
//   - Safety words come from every participant's identity key (lib/sas.ts).
//   - Plans: only a Premium member can start a call (voice or video);
//     anyone in the group can join; screen sharing needs your own Premium.
//     The server enforces each, down to what the LiveKit token lets you
//     publish.
//   - Link: once the starter is connected, their app posts the call's link
//     in the group chat (lib/groupCallLink.ts). Members paste it into "Join
//     with a link" (joinByLink); the link only works while the call is on.
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  ConnectionState,
  type Participant,
  type RemoteTrack,
  Room,
  RoomEvent,
  Track,
  isE2EESupported,
} from "livekit-client";
import * as api from "../lib/api.js";
import { CALLS_UNAVAILABLE_MESSAGE, webRtcAvailable } from "../lib/torRouting.js";
import { ApiError, type CallKind, type GroupCallEventDto, type GroupCallJoinDto } from "../lib/api.js";
import { fromBase64 } from "../lib/e2eCrypto.js";
import { GroupKeyError, newRoomKey, openRoomKey, RoomKeyProvider, sealRoomKey } from "../lib/groupCallCrypto.js";
import { groupCallInviteText, groupCallLinkUrl } from "../lib/groupCallLink.js";
import { readPrefs } from "../lib/localPrefs.js";
import { startRingtone } from "../lib/ringtone.js";
import { groupSasWords } from "../lib/sas.js";
import { type GroupConversation, tierAtLeast } from "../types.js";
import { sendDmMessage } from "./actions.js";
import { useAppState } from "./AppStateContext.js";
import { useCall } from "./CallContext.js";
import { useLive } from "./LiveContext.js";

export type GroupCallPhase = "incoming" | "connecting" | "active" | "reconnecting" | "ended";

/** One video tile: a person's camera (or avatar when it's off), or a shared screen. */
export interface GroupTile {
  key: string;
  userId: string | null;
  handle: string;
  avatarUrl: string | null;
  isSelf: boolean;
  source: "camera" | "screen";
  track: MediaStreamTrack | null;
  micMuted: boolean;
  speaking: boolean;
}

export interface GroupCallView {
  callId: string | null;
  conversationId: string;
  groupLabel: string;
  kind: CallKind;
  phase: GroupCallPhase;
  /** Who started it (shown when it rings). */
  hostHandle: string | null;
  muted: boolean;
  cameraOff: boolean;
  sharingScreen: boolean;
  minimized: boolean;
  connectedAt: number | null;
  tiles: GroupTile[];
  /** Four words from the call and every participant's identity key. */
  sas: string[] | null;
  /** This app has the room key for the current epoch (until then nothing is sent or shown). */
  keyReady: boolean;
  /** A problem worth showing without ending the call (e.g. a key that failed to verify). */
  notice: string | null;
  endedMessage: string | null;
}

export interface ActiveGroupCall {
  callId: string;
  kind: CallKind;
  participants: number;
}

interface GroupCallContextValue {
  call: GroupCallView | null;
  /** The server has LiveKit configured and this browser can encrypt calls end to end. */
  available: boolean;
  /** Calls going on in your groups, by conversation id, for a Join button. */
  active: ReadonlyMap<string, ActiveGroupCall>;
  start(group: GroupConversation, kind: CallKind): void;
  /**
   * Joins the call a pasted link points to. Resolves once the call screen is
   * up; rejects (with something to show) if the call isn't on, you aren't in
   * its group, or you're already in a call.
   */
  joinByLink(callId: string): Promise<void>;
  accept(): void;
  decline(): void;
  leave(): void;
  toggleMute(): void;
  toggleCamera(): void;
  toggleScreenShare(): void;
  setMinimized(minimized: boolean): void;
  dismiss(): void;
  refreshActive(): void;
}

interface Session {
  callId: string | null;
  conversationId: string;
  kind: CallKind;
  room: Room | null;
  keys: RoomKeyProvider;
  /** Events for this call that arrived before the room was connected. */
  buffer: GroupCallEventDto[];
  ready: boolean;
  epoch: number;
  keyholder: string | null;
  members: { userId: string; identity: string }[];
  /** The epoch whose key this app holds (0: none yet). */
  keyEpoch: number;
  /** Keys that arrived for an epoch this app hasn't seen the roster of yet. */
  earlyKeys: Map<number, Extract<GroupCallEventDto, { type: "group-key" }>>;
  published: boolean;
  closed: boolean;
  /** This app started the call: post its link in the chat once connected. */
  announce: boolean;
  stopTone: (() => void) | null;
  audio: HTMLElement[];
}

const GroupCallCtx = createContext<GroupCallContextValue | null>(null);
const ENDED_BANNER_MS = 4000;

function signalingUrl(url: string): string {
  if (!url.startsWith("/")) return url;
  return `${window.location.protocol === "https:" ? "wss" : "ws"}://${window.location.host}${url}`;
}

function errorMessage(err: unknown, fallback: string): string {
  if (err instanceof ApiError || err instanceof GroupKeyError) return err.message;
  return fallback;
}

export function GroupCallProvider({ children }: { children: ReactNode }) {
  const { state, dispatch } = useAppState();
  const live = useLive();
  const oneToOne = useCall();
  const [call, setCall] = useState<GroupCallView | null>(null);
  const [active, setActive] = useState<ReadonlyMap<string, ActiveGroupCall>>(new Map());
  const sessionRef = useRef<Session | null>(null);
  const stateRef = useRef(state);
  stateRef.current = state;
  const oneToOneRef = useRef(oneToOne.call);
  oneToOneRef.current = oneToOne.call;
  const endedTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const audioHost = useRef<HTMLDivElement>(null);

  const available = live.groupCallsAvailable && isE2EESupported();

  const patch = useCallback((changes: Partial<GroupCallView>) => {
    setCall((current) => (current ? { ...current, ...changes } : current));
  }, []);

  const groupOf = (conversationId: string): GroupConversation | undefined =>
    stateRef.current.conversations.find((c): c is GroupConversation => c.kind === "group" && c.id === conversationId);

  /** A participant's identity key, as this app knows it from the group's member list. */
  const publicKeyOf = (session: Session, userId: string): Uint8Array | null => {
    const identity = stateRef.current.identity;
    if (identity && userId === identity.userId) return identity.publicKey;
    const member = groupOf(session.conversationId)?.members.find((m) => m.userId === userId);
    return member ? fromBase64(member.publicKey) : null;
  };

  /** Rebuilds the tiles from the LiveKit room and the roster (which maps pseudonyms to people). */
  const refreshTiles = useCallback(() => {
    const session = sessionRef.current;
    const identity = stateRef.current.identity;
    if (!session?.room || !identity) return;
    const group = groupOf(session.conversationId);
    const byIdentity = new Map(session.members.map((m) => [m.identity, m.userId]));
    const person = (participant: Participant, isSelf: boolean) => {
      const userId = isSelf ? identity.userId : (byIdentity.get(participant.identity) ?? null);
      const member = userId ? group?.members.find((m) => m.userId === userId) : undefined;
      return {
        userId,
        handle: isSelf ? "You" : (member?.handle ?? "Joining…"),
        avatarUrl: isSelf ? identity.avatarUrl : (member?.avatarUrl ?? null),
      };
    };
    const tiles: GroupTile[] = [];
    const add = (participant: Participant, isSelf: boolean) => {
      const who = person(participant, isSelf);
      const camera = participant.getTrackPublication(Track.Source.Camera);
      const screen = participant.getTrackPublication(Track.Source.ScreenShare);
      const micMuted = !participant.isMicrophoneEnabled;
      const cameraTrack = camera?.track && !camera.isMuted ? camera.track.mediaStreamTrack : null;
      tiles.push({ key: `${participant.identity}:camera`, ...who, isSelf, source: "camera", track: cameraTrack, micMuted, speaking: participant.isSpeaking });
      if (screen?.track) {
        tiles.push({ key: `${participant.identity}:screen`, ...who, isSelf, source: "screen", track: screen.track.mediaStreamTrack, micMuted, speaking: false });
      }
    };
    add(session.room.localParticipant, true);
    for (const participant of session.room.remoteParticipants.values()) add(participant, false);
    patch({ tiles });
  }, [patch]);

  const refreshActive = useCallback(() => {
    if (!stateRef.current.identity || !live.groupCallsAvailable) return;
    api
      .listActiveGroupCalls()
      .then(({ calls }) => setActive(new Map(calls.map((c) => [c.conversationId, { callId: c.callId, kind: c.kind, participants: c.participants }]))))
      .catch(() => {});
  }, [live.groupCallsAvailable]);

  /** Tears the session down and leaves a closing banner (or nothing). */
  const finish = useCallback((message: string | null) => {
    const session = sessionRef.current;
    sessionRef.current = null;
    if (session) {
      session.closed = true;
      session.stopTone?.();
      session.audio.forEach((el) => el.remove());
      void session.room?.disconnect();
    }
    if (endedTimer.current) clearTimeout(endedTimer.current);
    if (message) {
      setCall((current) => (current ? { ...current, phase: "ended", endedMessage: message, minimized: false, tiles: [] } : current));
      endedTimer.current = setTimeout(() => setCall(null), ENDED_BANNER_MS);
    } else {
      setCall(null);
    }
  }, []);

  const leave = useCallback(() => {
    const session = sessionRef.current;
    if (session?.callId) api.leaveGroupCall(session.callId).catch(() => {});
    finish(session ? "You left the call" : null);
  }, [finish]);

  /** Starts sending once this app holds a room key: never before. */
  const maybePublish = useCallback(async () => {
    const session = sessionRef.current;
    if (!session?.room || session.published || session.keyEpoch === 0 || session.closed) return;
    session.published = true;
    const participant = session.room.localParticipant;
    const { microphoneId, cameraId } = readPrefs(stateRef.current.identity?.userId);
    try {
      await participant.setMicrophoneEnabled(true, microphoneId ? { deviceId: microphoneId } : undefined);
    } catch {
      if (session.closed) return;
      if (session.callId) api.leaveGroupCall(session.callId).catch(() => {});
      finish("Couldn't use your microphone. Allow access to join the call.");
      return;
    }
    if (session.kind === "video") {
      try {
        await participant.setCameraEnabled(true, cameraId ? { deviceId: cameraId } : undefined);
      } catch {
        patch({ cameraOff: true });
      }
    }
    refreshTiles();
  }, [finish, patch, refreshTiles]);

  const applyKey = useCallback(
    async (session: Session, key: Uint8Array, epoch: number) => {
      await session.keys.setRoomKey(key, epoch);
      session.keyEpoch = Math.max(session.keyEpoch, epoch);
      if (session.keyEpoch === session.epoch) patch({ keyReady: true, notice: null });
      await maybePublish();
    },
    [maybePublish, patch]
  );

  const handleKey = useCallback(
    async (session: Session, event: Extract<GroupCallEventDto, { type: "group-key" }>) => {
      const identity = stateRef.current.identity!;
      if (event.epoch > session.epoch) {
        session.earlyKeys.set(event.epoch, event);
        return;
      }
      if (event.epoch < session.epoch || event.from !== session.keyholder) return;
      const senderKey = publicKeyOf(session, event.from);
      try {
        if (!senderKey) throw new GroupKeyError("The call key came from someone this app has no identity key for.");
        const key = openRoomKey(event.key, senderKey, identity.privateKey, {
          callId: session.callId!,
          conversationId: session.conversationId,
          epoch: event.epoch,
          from: event.from,
          to: identity.userId,
        });
        await applyKey(session, key, event.epoch);
      } catch (err) {
        // Not usable: stay silent rather than use a key that didn't verify.
        patch({ notice: errorMessage(err, "Couldn't verify the call key.") });
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [patch, applyKey]
  );

  const handleRoster = useCallback(
    async (session: Session, event: Extract<GroupCallEventDto, { type: "group-roster" }>) => {
      const identity = stateRef.current.identity!;
      if (event.epoch <= session.epoch) return;
      session.epoch = event.epoch;
      session.keyholder = event.keyholder;
      session.members = event.members;
      setActive((current) => {
        const next = new Map(current);
        next.set(session.conversationId, { callId: session.callId!, kind: session.kind, participants: event.members.length });
        return next;
      });

      const known = event.members.map((m) => ({ userId: m.userId, publicKey: publicKeyOf(session, m.userId) }));
      const complete = known.every((k) => k.publicKey !== null);
      patch({
        keyReady: session.keyEpoch === event.epoch,
        sas: complete ? groupSasWords(session.callId!, known as { userId: string; publicKey: Uint8Array }[]) : null,
      });
      refreshTiles();

      if (event.keyholder === identity.userId) {
        // Our turn: a fresh key for this epoch, sealed to everyone else.
        const key = newRoomKey();
        await applyKey(session, key, event.epoch);
        const sealed = event.members
          .filter((m) => m.userId !== identity.userId)
          .flatMap((m) => {
            const publicKey = publicKeyOf(session, m.userId);
            if (!publicKey) return [];
            const message = { callId: session.callId!, conversationId: session.conversationId, epoch: event.epoch, from: identity.userId, to: m.userId };
            return [{ userId: m.userId, key: sealRoomKey(message, key, publicKey, identity.privateKey) }];
          });
        if (sealed.length > 0) {
          await api.sendGroupCallKeys(session.callId!, event.epoch, sealed).catch((err) => {
            // 409: someone joined or left meanwhile; the next roster brings a new epoch.
            if (!(err instanceof ApiError && err.status === 409)) patch({ notice: "Couldn't hand out the call key. Others may not hear you." });
          });
        }
      } else {
        const early = session.earlyKeys.get(event.epoch);
        session.earlyKeys.delete(event.epoch);
        if (early) await handleKey(session, early);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [handleKey, patch, refreshTiles, applyKey]
  );

  const handleCallEvent = useCallback(
    async (session: Session, event: GroupCallEventDto) => {
      if (event.type === "group-roster") await handleRoster(session, event);
      else if (event.type === "group-key") await handleKey(session, event);
    },
    [handleKey, handleRoster]
  );

  /** Posts the call's link in the group chat, as an ordinary end-to-end encrypted message. */
  const postCallLink = useCallback(
    async (session: Session) => {
      const group = groupOf(session.conversationId);
      if (!group?.conversationKey || !session.callId) return;
      try {
        await sendDmMessage(dispatch, group.id, group.conversationKey, groupCallInviteText(session.kind, groupCallLinkUrl(session.callId)));
      } catch {
        if (!session.closed) patch({ notice: "Couldn't post the call link in the chat. People can still join from the Join button." });
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [dispatch, patch]
  );

  /** Connects to LiveKit with end-to-end encryption on, then catches up on the roster and keys. */
  const connect = useCallback(
    async (session: Session, join: GroupCallJoinDto) => {
      session.callId = join.callId;
      patch({ callId: join.callId, kind: join.kind });
      session.kind = join.kind;
      const room = new Room({
        adaptiveStream: true,
        dynacast: true,
        e2ee: {
          keyProvider: session.keys,
          worker: new Worker(new URL("livekit-client/e2ee-worker", import.meta.url), { type: "module" }),
        },
      });
      session.room = room;
      const onChange = () => refreshTiles();
      room
        .on(RoomEvent.ParticipantConnected, onChange)
        .on(RoomEvent.ParticipantDisconnected, onChange)
        .on(RoomEvent.TrackPublished, onChange)
        .on(RoomEvent.TrackUnpublished, onChange)
        .on(RoomEvent.TrackMuted, onChange)
        .on(RoomEvent.TrackUnmuted, onChange)
        .on(RoomEvent.LocalTrackPublished, onChange)
        .on(RoomEvent.LocalTrackUnpublished, onChange)
        .on(RoomEvent.ActiveSpeakersChanged, onChange)
        .on(RoomEvent.TrackSubscribed, (track: RemoteTrack) => {
          if (track.kind === Track.Kind.Audio && !session.closed) {
            const el = track.attach();
            audioHost.current?.appendChild(el);
            session.audio.push(el);
          }
          refreshTiles();
        })
        .on(RoomEvent.TrackUnsubscribed, (track: RemoteTrack) => {
          track.detach().forEach((el) => {
            el.remove();
            session.audio = session.audio.filter((a) => a !== el);
          });
          refreshTiles();
        })
        .on(RoomEvent.ConnectionStateChanged, (connection: ConnectionState) => {
          if (session.closed) return;
          if (connection === ConnectionState.Reconnecting || connection === ConnectionState.SignalReconnecting) patch({ phase: "reconnecting" });
          else if (connection === ConnectionState.Connected) patch({ phase: "active" });
        })
        .on(RoomEvent.Disconnected, () => {
          if (!session.closed && sessionRef.current === session) {
            if (session.callId) api.leaveGroupCall(session.callId).catch(() => {});
            finish("Call dropped");
          }
        });

      try {
        await room.setE2EEEnabled(true);
        // Relay only: media goes through the call server's TURN relay, so no participant learns
        // another's IP address (as in one-to-one calls). The server's ICE servers are kept.
        await room.connect(signalingUrl(join.url), join.token, { rtcConfig: { iceTransportPolicy: "relay" } });
      } catch {
        if (session.closed) return;
        api.leaveGroupCall(join.callId).catch(() => {});
        finish("Couldn't reach the call server. Try again in a minute.");
        return;
      }
      if (session.closed) {
        void room.disconnect();
        return;
      }
      patch({ phase: "active", connectedAt: Date.now() });
      session.ready = true;
      // The link goes out only now that the call is really on: until the
      // starter is connected there's nothing for anyone to join.
      if (session.announce) {
        session.announce = false;
        void postCallLink(session);
      }
      const buffered = session.buffer.filter((e) => e.callId === join.callId);
      session.buffer = [];
      for (const event of buffered) await handleCallEvent(session, event);
      refreshTiles();
    },
    [finish, handleCallEvent, patch, postCallLink, refreshTiles]
  );

  const newSession = (conversationId: string, kind: CallKind, callId: string | null): Session => ({
    callId,
    conversationId,
    kind,
    room: null,
    keys: new RoomKeyProvider(),
    buffer: [],
    ready: false,
    epoch: 0,
    keyholder: null,
    members: [],
    keyEpoch: 0,
    earlyKeys: new Map(),
    published: false,
    closed: false,
    announce: false,
    stopTone: null,
    audio: [],
  });

  const blankView = (group: GroupConversation, kind: CallKind, phase: GroupCallPhase, callId: string | null, hostHandle: string | null): GroupCallView => ({
    callId,
    conversationId: group.id,
    groupLabel: group.label,
    kind,
    phase,
    hostHandle,
    muted: false,
    cameraOff: kind === "voice",
    sharingScreen: false,
    minimized: false,
    connectedAt: null,
    tiles: [],
    sas: null,
    keyReady: false,
    notice: null,
    endedMessage: null,
  });

  const busy = () => sessionRef.current !== null || (oneToOneRef.current !== null && oneToOneRef.current.phase !== "ended");

  const start = useCallback(
    (group: GroupConversation, kind: CallKind) => {
      if (busy() || !stateRef.current.identity) return;
      if (endedTimer.current) clearTimeout(endedTimer.current);
      if (!webRtcAvailable()) {
        // Tor Browser turns WebRTC off: say why rather than start a call nobody can hear.
        setCall(blankView(group, kind, "connecting", null, null));
        finish(CALLS_UNAVAILABLE_MESSAGE);
        return;
      }
      const session = newSession(group.id, kind, null);
      sessionRef.current = session;
      setCall(blankView(group, kind, "connecting", null, null));
      (async () => {
        let join: GroupCallJoinDto;
        try {
          const ongoing = active.get(group.id);
          join = ongoing ? await api.joinGroupCall(ongoing.callId) : await api.startGroupCall(group.id, kind);
        } catch (err) {
          if (!session.closed) finish(errorMessage(err, "Couldn't start the call. Try again."));
          return;
        }
        if (session.closed) {
          api.leaveGroupCall(join.callId).catch(() => {});
          return;
        }
        session.announce = join.created;
        await connect(session, join);
      })();
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [active, connect, finish]
  );

  const joinByLink = useCallback(
    async (callId: string) => {
      if (!stateRef.current.identity) throw new Error("Sign in to join the call.");
      // The call is ringing here already: pasting its link is the same as answering.
      const ringing = sessionRef.current;
      if (ringing && !ringing.room && ringing.callId === callId) {
        accept();
        return;
      }
      if (busy()) throw new Error("You're already in a call. Leave it first.");
      // The server answers "This call has ended" for a call that was never
      // started, one that's over, and one in a group you're not in.
      const join = await api.joinGroupCall(callId);
      const group = groupOf(join.conversationId);
      if (!group) {
        api.leaveGroupCall(join.callId).catch(() => {});
        throw new Error("That call is in a group chat that isn't loaded yet. Open your chats and try the link again.");
      }
      if (endedTimer.current) clearTimeout(endedTimer.current);
      const session = newSession(group.id, join.kind, join.callId);
      sessionRef.current = session;
      setCall(blankView(group, join.kind, "connecting", join.callId, null));
      // Connecting carries on behind the call screen; it reports its own failures there.
      void connect(session, join);
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [connect]
  );

  const accept = useCallback(() => {
    const session = sessionRef.current;
    if (!session?.callId || session.room) return;
    session.stopTone?.();
    session.stopTone = null;
    patch({ phase: "connecting" });
    const callId = session.callId;
    // Ringing is over: a late "stop ringing" mustn't cancel the join.
    session.callId = null;
    (async () => {
      try {
        const join = await api.joinGroupCall(callId);
        if (session.closed) {
          api.leaveGroupCall(callId).catch(() => {});
          return;
        }
        await connect(session, join);
      } catch (err) {
        if (!session.closed) finish(errorMessage(err, "Couldn't join the call."));
      }
    })();
  }, [connect, finish, patch]);

  const decline = useCallback(() => {
    const session = sessionRef.current;
    if (!session || session.room) return;
    if (session.callId) api.declineGroupCall(session.callId).catch(() => {});
    finish(null);
  }, [finish]);

  const toggleMute = useCallback(() => {
    const participant = sessionRef.current?.room?.localParticipant;
    if (!participant) return;
    const enabled = !participant.isMicrophoneEnabled;
    participant
      .setMicrophoneEnabled(enabled)
      .then(() => {
        patch({ muted: !enabled });
        refreshTiles();
      })
      .catch(() => {});
  }, [patch, refreshTiles]);

  const toggleCamera = useCallback(() => {
    const session = sessionRef.current;
    const participant = session?.room?.localParticipant;
    if (!participant || session.kind !== "video" || session.keyEpoch === 0) return;
    const enabled = !participant.isCameraEnabled;
    participant
      .setCameraEnabled(enabled)
      .then(() => {
        patch({ cameraOff: !enabled });
        refreshTiles();
      })
      .catch(() => patch({ notice: "Couldn't use your camera." }));
  }, [patch, refreshTiles]);

  const toggleScreenShare = useCallback(() => {
    const session = sessionRef.current;
    const participant = session?.room?.localParticipant;
    const identity = stateRef.current.identity;
    if (!participant || !identity || session.keyEpoch === 0) return;
    if (!tierAtLeast(identity.subscriptionTier, "premium")) return;
    const enabled = !participant.isScreenShareEnabled;
    participant
      .setScreenShareEnabled(enabled, { audio: false, contentHint: "detail" })
      .then(() => {
        patch({ sharingScreen: participant.isScreenShareEnabled, notice: null });
        refreshTiles();
      })
      .catch(() => {
        // Cancelled picker, or the token doesn't allow it.
        patch({ sharingScreen: participant.isScreenShareEnabled });
      });
  }, [patch, refreshTiles]);

  const setMinimized = useCallback((minimized: boolean) => patch({ minimized }), [patch]);

  const dismiss = useCallback(() => {
    if (endedTimer.current) clearTimeout(endedTimer.current);
    if (!sessionRef.current) setCall(null);
  }, []);

  const handleEvent = useCallback(
    async (event: api.LiveEventDto) => {
      const identity = stateRef.current.identity;
      if (!identity || !event.type.startsWith("group-")) return;
      const groupEvent = event as GroupCallEventDto;
      const session = sessionRef.current;

      if (groupEvent.type === "group-incoming") {
        setActive((current) => new Map(current).set(groupEvent.conversationId, { callId: groupEvent.callId, kind: groupEvent.kind, participants: 1 }));
        const group = groupOf(groupEvent.conversationId);
        if (!group || busy() || groupEvent.hostId === identity.userId) return;
        if (endedTimer.current) clearTimeout(endedTimer.current);
        const host = group.members.find((m) => m.userId === groupEvent.hostId);
        const next = newSession(group.id, groupEvent.kind, groupEvent.callId);
        const prefs = readPrefs(identity.userId);
        const silent = !prefs.ringtone || prefs.muted.includes(group.id);
        next.stopTone = silent ? null : startRingtone("incoming");
        sessionRef.current = next;
        setCall(blankView(group, groupEvent.kind, "incoming", groupEvent.callId, host?.handle ?? null));
        return;
      }

      if (groupEvent.type === "group-ended") {
        setActive((current) => {
          const next = new Map(current);
          if (next.get(groupEvent.conversationId)?.callId === groupEvent.callId) next.delete(groupEvent.conversationId);
          return next;
        });
      }

      if (!session) return;
      // Joining: hold on to this call's events until the room is connected.
      if (!session.ready) {
        if (groupEvent.type === "group-roster" || groupEvent.type === "group-key") session.buffer.push(groupEvent);
        else if (session.callId === groupEvent.callId) finish(groupEvent.type === "group-ended" ? "Call ended" : null);
        return;
      }
      if (session.callId !== groupEvent.callId) return;
      if (groupEvent.type === "group-ended") finish("Call ended");
      else await handleCallEvent(session, groupEvent);
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [finish, handleCallEvent]
  );

  const handleEventRef = useRef(handleEvent);
  handleEventRef.current = handleEvent;
  useEffect(() => live.subscribe((event) => handleEventRef.current(event)), [live]);

  // Which of your groups have a call going on (for the Join button).
  const signedInUserId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  useEffect(() => {
    if (signedInUserId) refreshActive();
    else setActive(new Map());
  }, [signedInUserId, refreshActive]);

  // Signing out (or switching accounts) leaves the call.
  useEffect(() => {
    if (!signedInUserId && sessionRef.current) leave();
  }, [signedInUserId, leave]);

  // Closing the tab leaves, so the others get a new key straight away.
  useEffect(() => {
    const onUnload = () => {
      const session = sessionRef.current;
      if (session?.callId && session.room) api.leaveGroupCallOnUnload(session.callId);
    };
    window.addEventListener("pagehide", onUnload);
    return () => window.removeEventListener("pagehide", onUnload);
  }, []);

  const value = useMemo<GroupCallContextValue>(
    () => ({
      call,
      available,
      active,
      start,
      joinByLink,
      accept,
      decline,
      leave,
      toggleMute,
      toggleCamera,
      toggleScreenShare,
      setMinimized,
      dismiss,
      refreshActive,
    }),
    [call, available, active, start, joinByLink, accept, decline, leave, toggleMute, toggleCamera, toggleScreenShare, setMinimized, dismiss, refreshActive]
  );

  return (
    <GroupCallCtx.Provider value={value}>
      {children}
      {/* Remote audio plays from here, so it carries on while the call screen is minimized. */}
      <div ref={audioHost} hidden />
    </GroupCallCtx.Provider>
  );
}

export function useGroupCall(): GroupCallContextValue {
  const ctx = useContext(GroupCallCtx);
  if (!ctx) throw new Error("useGroupCall must be used within a GroupCallProvider");
  return ctx;
}
