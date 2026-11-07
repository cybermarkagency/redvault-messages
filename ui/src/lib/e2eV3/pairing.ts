// Pairing a new browser with a trusted device of the same account by
// comparing codes (crypto-core/src/pairing.rs: vodozemac's SAS with a key
// commitment), for when neither can scan the other's QR code. The server only
// relays the messages; a relay that swaps keys makes the two screens show
// different numbers, and the person stops.
import * as api from "../api.js";
import { EngineError, publishList, receive, type V3Device } from "./device.js";
import { runEngine, type EngineResponse } from "./engine.js";

type Message =
  | { type: "commit"; commitment: string; identity: api.V3DeviceIdentity }
  | { type: "key"; publicKey: string; identity: api.V3DeviceIdentity }
  | { type: "reveal"; publicKey: string }
  | { type: "mac"; mac: string }
  | { type: "offer"; qr: string };

export interface WaitOptions {
  intervalMs?: number;
  timeoutMs?: number;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function stateless(request: Record<string, unknown>): Promise<EngineResponse> {
  const response = await runEngine(request);
  if (!response.ok) throw new EngineError(response.error ?? "engine_error", response.message ?? String(request.op));
  return response;
}

function post(enrollmentId: string, from: V3Device, message: Message) {
  return api.postV3EnrollmentMessage(enrollmentId, from.deviceId, JSON.stringify(message));
}

/** The first message of `type` the other device sent, polling until it comes. */
async function waitFor<T extends Message["type"]>(
  enrollmentId: string,
  self: V3Device,
  type: T,
  { intervalMs = 1000, timeoutMs = 10 * 60 * 1000 }: WaitOptions = {}
): Promise<Extract<Message, { type: T }>> {
  const until = Date.now() + timeoutMs;
  for (;;) {
    for (const m of await api.getV3EnrollmentMessages(enrollmentId)) {
      if (m.fromDeviceId === self.deviceId) continue;
      try {
        const body = JSON.parse(m.body) as Message;
        if (body.type === type) return body as Extract<Message, { type: T }>;
      } catch {
        // Not ours to read; keep waiting.
      }
    }
    if (Date.now() > until) throw new EngineError("pairing_timeout", "The other device didn't answer in time.");
    await sleep(intervalMs);
  }
}

/** On the new browser. */
export class NewDevicePairing {
  private trustedDevice: api.V3DeviceIdentity | null = null;

  private constructor(
    readonly device: V3Device,
    readonly enrollmentId: string,
    private readonly sasId: string
  ) {}

  /** Asks the account's trusted devices to pair; they're told and show the request. */
  static async begin(device: V3Device): Promise<NewDevicePairing> {
    const enrollment = await api.createV3Enrollment(device.deviceId);
    const started = await stateless({ op: "sasStart", pairingId: enrollment.id, role: "new" });
    await post(enrollment.id, device, { type: "commit", commitment: started.commitment as string, identity: await device.identity() });
    return new NewDevicePairing(device, enrollment.id, started.sasId as string);
  }

  /** Waits for a trusted device to answer; returns the numbers to compare with its screen. */
  async codes(wait?: WaitOptions): Promise<number[]> {
    const key = await waitFor(this.enrollmentId, this.device, "key", wait);
    const revealed = await stateless({ op: "sasReveal", sasId: this.sasId });
    await post(this.enrollmentId, this.device, { type: "reveal", publicKey: revealed.publicKey as string });
    const established = await stateless({
      op: "sasEstablish",
      sasId: this.sasId,
      theirPublicKey: key.publicKey,
      newDevice: await this.device.identity(),
      trustedDevice: key.identity,
    });
    this.trustedDevice = key.identity;
    return established.decimals as number[];
  }

  /** The person said the numbers match. Resolves with the account's list once the trusted device has linked this one. */
  async confirm(wait?: WaitOptions): Promise<api.V3SignedDeviceList> {
    const ours = await this.device.must("sasMac", { sasId: this.sasId });
    await post(this.enrollmentId, this.device, { type: "mac", mac: ours.mac as string });
    const theirs = await waitFor(this.enrollmentId, this.device, "mac", wait);
    await this.device.must("sasConfirm", { sasId: this.sasId, mac: theirs.mac });
    const offer = await this.device.must("linkOffer");
    await post(this.enrollmentId, this.device, { type: "offer", qr: offer.qrText as string });
    const until = Date.now() + (wait?.timeoutMs ?? 10 * 60 * 1000);
    for (;;) {
      const link = (await receive(this.device)).find((r) => r.kind === "link" && !r.error);
      if (link) break;
      if (Date.now() > until) throw new EngineError("pairing_timeout", "The trusted device didn't link this one in time.");
      await sleep(wait?.intervalMs ?? 1000);
    }
    await this.device.publishKeys(10);
    await api.deleteV3Enrollment(this.enrollmentId).catch(() => {});
    return (await this.device.must("deviceList")).deviceList as api.V3SignedDeviceList;
  }

  /** The numbers didn't match, or the person gave up. */
  async cancel(): Promise<void> {
    await api.deleteV3Enrollment(this.enrollmentId).catch(() => {});
    await this.device.op("cancelLink");
  }

  get trusted(): api.V3DeviceIdentity | null {
    return this.trustedDevice;
  }
}

/** On a trusted device, answering a pairing request. */
export class TrustedPairing {
  private constructor(
    readonly device: V3Device,
    readonly enrollmentId: string,
    private readonly sasId: string,
    readonly newDevice: api.V3DeviceIdentity
  ) {}

  /** Answers the request; returns the pairing and the numbers to compare with the new device's screen. */
  static async begin(device: V3Device, enrollmentId: string, wait?: WaitOptions): Promise<{ pairing: TrustedPairing; codes: number[] }> {
    const commit = await waitFor(enrollmentId, device, "commit", wait);
    const started = await stateless({ op: "sasStart", pairingId: enrollmentId, role: "trusted" });
    const identity = await device.identity();
    await post(enrollmentId, device, { type: "key", publicKey: started.publicKey as string, identity });
    const reveal = await waitFor(enrollmentId, device, "reveal", wait);
    const established = await stateless({
      op: "sasEstablish",
      sasId: started.sasId,
      theirPublicKey: reveal.publicKey,
      theirCommitment: commit.commitment,
      newDevice: commit.identity,
      trustedDevice: identity,
    });
    return { pairing: new TrustedPairing(device, enrollmentId, started.sasId as string, commit.identity), codes: established.decimals as number[] };
  }

  /** The person said the numbers match: approves exactly the device compared with, signs the new list and sends the link. */
  async confirm(wait?: WaitOptions): Promise<api.V3SignedDeviceList> {
    const ours = await this.device.must("sasMac", { sasId: this.sasId });
    await post(this.enrollmentId, this.device, { type: "mac", mac: ours.mac as string });
    const theirs = await waitFor(this.enrollmentId, this.device, "mac", wait);
    await this.device.must("sasConfirm", { sasId: this.sasId, mac: theirs.mac });
    const offer = await waitFor(this.enrollmentId, this.device, "offer", wait);
    const approved = await publishList(this.device, () => this.device.must("approveDevice", { qr: offer.qr, verifiedBy: "sas" }));
    await api.sendV3Messages(this.device.deviceId, null, [
      { userId: this.newDevice.userId, deviceId: this.newDevice.deviceId, body: JSON.stringify(approved.message) },
    ]);
    return approved.deviceList as api.V3SignedDeviceList;
  }

  /** The numbers didn't match: nothing is approved. */
  async reject(): Promise<void> {
    await api.deleteV3Enrollment(this.enrollmentId).catch(() => {});
  }
}
