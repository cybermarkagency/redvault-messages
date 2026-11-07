import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import { PaidMessageBar, usePaidMessaging } from "../components/earnings/PaidMessageBar.js";
import { ExemptionToggle } from "../components/earnings/ExemptionToggle.js";
import { TipDialog } from "../components/earnings/TipDialog.js";
import * as crypto from "../lib/e2eCrypto.js";
import { deliverPendingKeys, openPost, PaymentNotCompleted, publishPost, waitForPurchase } from "../lib/earnings.js";
import * as eapi from "../lib/earningsApi.js";
import { newPostId, newPostKey, openPostBody, PostCryptoError, sealPostBody, unwrapPostKey, wrapPostKey } from "../lib/postCrypto.js";
import { newStoryId, unwrapStoryKey, wrapStoryKey } from "../lib/storyCrypto.js";

vi.mock("../lib/earningsApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/earningsApi.js")>()),
  getPurchase: vi.fn(),
  startPurchase: vi.fn(),
  getQuote: vi.fn(),
  getKeyRequests: vi.fn(),
  getPost: vi.fn(),
  deliverPostKey: vi.fn(async () => undefined),
  createPost: vi.fn(async () => ({ id: "x", status: "published" })),
  uploadPostMedia: vi.fn(async () => undefined),
  getEarningSettings: vi.fn(),
  setExemption: vi.fn(async () => undefined),
}));

type Keys = { publicKey: Uint8Array; privateKey: Uint8Array };
let creator: Keys & { userId: string };
let buyer: Keys & { userId: string };
let stranger: Keys;

beforeAll(async () => {
  await crypto.ready();
  creator = { ...crypto.generateKeyPair(), userId: "00000000-0000-4000-8000-0000000000c1" };
  buyer = { ...crypto.generateKeyPair(), userId: "00000000-0000-4000-8000-0000000000b1" };
  stranger = crypto.generateKeyPair();
});

beforeEach(() => {
  vi.mocked(eapi.getPurchase).mockReset();
  vi.mocked(eapi.startPurchase).mockReset();
  vi.mocked(eapi.deliverPostKey).mockClear();
  vi.mocked(eapi.createPost).mockClear();
});

const purchase = (status: eapi.PurchaseStatus, over: Partial<eapi.PurchaseDto> = {}): eapi.PurchaseDto => ({
  id: "p1",
  product: "paid_message",
  status,
  amountMinor: 300,
  currency: "usd",
  commissionPercent: 10,
  platformFeeMinor: 30,
  creatorUserId: creator.userId,
  postId: null,
  paidMessageMode: "per_message",
  showName: true,
  createdAt: "2026-10-19T00:00:00.000Z",
  paidAt: null,
  refundedAt: null,
  useBy: null,
  keyDelivered: false,
  ...over,
});

describe("exclusive post crypto", () => {
  it("only the buyer it was sealed to opens the key, made by the creator, for that post", () => {
    const id = newPostId();
    const key = newPostKey();
    const wrapped = wrapPostKey(id, key, buyer.publicKey, creator.privateKey);
    expect(unwrapPostKey(id, wrapped, creator.publicKey, buyer.privateKey)).toEqual(key);
    expect(() => unwrapPostKey(id, wrapped, creator.publicKey, stranger.privateKey)).toThrow(PostCryptoError);
    expect(() => unwrapPostKey(newPostId(), wrapped, creator.publicKey, buyer.privateKey)).toThrow(/another post/);
    expect(() => unwrapPostKey(id, wrapPostKey(id, key, buyer.publicKey, stranger.privateKey), creator.publicKey, buyer.privateKey)).toThrow(PostCryptoError);
  });

  it("a story key can't pass for a post key, or the other way round", () => {
    const id = newStoryId();
    const key = newPostKey();
    expect(() => unwrapPostKey(id, wrapStoryKey(id, key, buyer.publicKey, creator.privateKey), creator.publicKey, buyer.privateKey)).toThrow(/format/);
    expect(() => unwrapStoryKey(id, wrapPostKey(id, key, buyer.publicKey, creator.privateKey), creator.publicKey, buyer.privateKey)).toThrow();
  });

  it("the body is bound to its post and checked when opened", () => {
    const id = newPostId();
    const key = newPostKey();
    const sealed = sealPostBody(id, key, { v: 1, text: "Behind the scenes" });
    expect(openPostBody(id, key, sealed.ciphertext, sealed.nonce).text).toBe("Behind the scenes");
    expect(() => openPostBody(newPostId(), key, sealed.ciphertext, sealed.nonce)).toThrow(PostCryptoError);
    const bad = sealPostBody(id, key, { v: 1, text: "x", media: { key: "a", header: "b", sha256: "no", encryptedSize: 1, chunkSize: 1, mime: "text/html" } });
    expect(() => openPostBody(id, key, bad.ciphertext, bad.nonce)).toThrow(/malformed/);
  });
});

describe("paying", () => {
  it("only the server's confirmation counts: waits through pending, resolves when paid", async () => {
    vi.mocked(eapi.getPurchase).mockResolvedValueOnce(purchase("checkout_pending")).mockResolvedValueOnce(purchase("checkout_pending")).mockResolvedValueOnce(purchase("paid"));
    const statuses: string[] = [];
    const done = await waitForPurchase("p1", { pollMs: 1, onStatus: (s) => statuses.push(s) });
    expect(done.status).toBe("paid");
    expect(statuses).toEqual(["checkout_pending", "checkout_pending", "paid"]);
  });

  it("a canceled, failed or refunded purchase, or no answer in time, isn't treated as paid", async () => {
    vi.mocked(eapi.getPurchase).mockResolvedValue(purchase("canceled"));
    await expect(waitForPurchase("p1", { pollMs: 1 })).rejects.toBeInstanceOf(PaymentNotCompleted);
    vi.mocked(eapi.getPurchase).mockResolvedValue(purchase("refunded"));
    await expect(waitForPurchase("p1", { pollMs: 1 })).rejects.toThrow(/refunded/);
    vi.mocked(eapi.getPurchase).mockResolvedValue(purchase("checkout_pending"));
    await expect(waitForPurchase("p1", { pollMs: 1, giveUpMs: 5 })).rejects.toThrow(/haven't heard/);
  });
});

describe("exclusive posts end to end (crypto)", () => {
  it("the server only gets ciphertext; the creator's app seals the key to a buyer, who can then read it", async () => {
    await publishPost(creator, { text: "Secret recipe", priceMinor: 500, teaser: "A recipe" });
    const created = vi.mocked(eapi.createPost).mock.calls[0][0];
    expect(JSON.stringify(created)).not.toContain("Secret recipe");
    expect(created.teaser).toBe("A recipe");

    const postView = (wrappedKey: string, mine: boolean): eapi.PostDto => ({
      id: created.id,
      authorId: creator.userId,
      creatorId: "c",
      priceMinor: 500,
      currency: "usd",
      teaser: "A recipe",
      status: "published",
      createdAt: "",
      hasMedia: false,
      mediaSize: null,
      mine,
      owned: !mine,
      awaitingKey: false,
      locked: false,
      body: { ciphertext: created.bodyCiphertext, nonce: created.bodyNonce, wrappedKey },
    });
    vi.mocked(eapi.getKeyRequests).mockResolvedValue([{ purchaseId: "pp", postId: created.id, buyerId: buyer.userId, buyerPublicKey: crypto.toBase64(buyer.publicKey) }]);
    vi.mocked(eapi.getPost).mockResolvedValue(postView(created.wrappedKey, true));
    expect(await deliverPendingKeys(creator)).toBe(1);
    const [postId, purchaseId, sealedForBuyer] = vi.mocked(eapi.deliverPostKey).mock.calls[0];
    expect([postId, purchaseId]).toEqual([created.id, "pp"]);
    const opened = await openPost(buyer, postView(sealedForBuyer, false), new Map([[creator.userId, creator.publicKey]]));
    expect(opened.body.text).toBe("Secret recipe");

    // A buyer the creator chats with gets it only for the key that chat shows: a swapped-in key gets nothing.
    vi.mocked(eapi.deliverPostKey).mockClear();
    expect(await deliverPendingKeys(creator, new Map([[buyer.userId, stranger.publicKey]]))).toBe(0);
    expect(eapi.deliverPostKey).not.toHaveBeenCalled();
    expect(await deliverPendingKeys(creator, new Map([[buyer.userId, buyer.publicKey]]))).toBe(1);
  });
});

function BarHarness({ onSend }: { onSend: (paid: ReturnType<ReturnType<typeof usePaidMessaging>["sendOptions"]>) => void }) {
  const paid = usePaidMessaging(creator.userId, "conv1");
  return (
    <>
      <PaidMessageBar name="maker" creatorUserId={creator.userId} paid={paid} />
      <button type="button" onClick={() => onSend(paid.sendOptions())}>
        Send
      </button>
    </>
  );
}

describe("paid messages in a chat", () => {
  it("shows the price and holds the send until paid; then the send carries that paid message", async () => {
    const user = userEvent.setup();
    const quote: eapi.QuoteDto = { required: true, reason: "payment_required", mode: "per_message", priceMinor: 300, currency: "usd", purchasable: true, availablePurchaseId: null, safetyContactAvailable: true };
    vi.mocked(eapi.getQuote).mockResolvedValueOnce(quote).mockResolvedValue({ ...quote, availablePurchaseId: "p1" });
    vi.mocked(eapi.startPurchase).mockResolvedValue({ purchaseId: "p1", status: "checkout_pending", checkoutUrl: "/checkout/p1" });
    vi.mocked(eapi.getPurchase).mockResolvedValue(purchase("paid"));
    const opened = vi.spyOn(window, "open").mockReturnValue({} as Window);
    const sends: unknown[] = [];
    render(<BarHarness onSend={(p) => sends.push(p)} />);
    expect(await screen.findByText(/maker charges \$3\.00 for each message/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Send" }));
    expect(sends).toEqual([null]);
    await user.click(screen.getByRole("button", { name: "Pay $3.00" }));
    expect(opened).toHaveBeenCalledWith("/checkout/p1", "_blank", "noopener,noreferrer");
    expect(vi.mocked(eapi.startPurchase).mock.calls[0][0]).toMatchObject({ product: "paid_message", creatorUserId: creator.userId, platform: "web" });
    // No amount from this app: the server prices paid messages.
    expect(vi.mocked(eapi.startPurchase).mock.calls[0][0]).not.toHaveProperty("amountMinor");
    expect(await screen.findByText(/Your next message to maker goes through/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Send" }));
    expect(sends[1]).toEqual({ entitlementId: "p1" });
    opened.mockRestore();
  });

  it("the free safety contact sends without paying", async () => {
    const user = userEvent.setup();
    vi.mocked(eapi.getQuote).mockResolvedValue({ required: true, reason: "payment_required", mode: "first_contact", priceMinor: 500, currency: "usd", purchasable: true, availablePurchaseId: null, safetyContactAvailable: true });
    const sends: unknown[] = [];
    render(<BarHarness onSend={(p) => sends.push(p)} />);
    expect(await screen.findByText(/charges \$5\.00 to start a chat/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Safety contact (free)" }));
    await user.click(screen.getByRole("button", { name: "Send" }));
    expect(sends).toEqual([{ safetyContact: true }]);
  });
});

describe("tips", () => {
  it("presets or a custom amount, and the choice to stay a supporter", async () => {
    const user = userEvent.setup();
    vi.mocked(eapi.startPurchase).mockResolvedValue({ purchaseId: "t1", status: "checkout_pending", checkoutUrl: "/checkout/t1" });
    vi.mocked(eapi.getPurchase).mockResolvedValue(purchase("consumed", { product: "tip" }));
    const opened = vi.spyOn(window, "open").mockReturnValue({} as Window);
    render(<TipDialog creatorUserId={creator.userId} name="maker" onClose={() => undefined} />);
    expect(screen.getByText(/don't unlock anything/)).toBeInTheDocument();
    await user.click(screen.getByRole("radio", { name: "Other" }));
    await user.type(screen.getByRole("textbox", { name: "Tip amount in US dollars" }), "0.50");
    await user.click(screen.getByRole("button", { name: /^Tip \$0\.50/ }));
    expect(screen.getByText(/between \$1\.00 and \$500\.00/)).toBeInTheDocument();
    expect(eapi.startPurchase).not.toHaveBeenCalled();

    await user.click(screen.getByRole("radio", { name: "$5.00" }));
    await user.click(screen.getByRole("checkbox"));
    expect(screen.getByText(/isn't anonymous/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Tip $5.00" }));
    await waitFor(() => expect(screen.getByText(/Your tip went through/)).toBeInTheDocument());
    expect(vi.mocked(eapi.startPurchase).mock.calls[0][0]).toMatchObject({ product: "tip", amountMinor: 500, showName: false });
    opened.mockRestore();
  });
});

describe("free messaging for chosen people", () => {
  it("a creator who charges can let someone in from their profile, and take it back", async () => {
    const user = userEvent.setup();
    vi.mocked(eapi.getEarningSettings).mockResolvedValue({ available: true, creatorReady: true, paidMessagesMode: "per_message", paidMessagePriceMinor: 300, tipsEnabled: false, acceptsMessageRequests: true, exemptions: [] });
    render(<ExemptionToggle userId={buyer.userId} name="fan" />);
    await user.click(await screen.findByRole("button", { name: "Let fan message you free" }));
    expect(eapi.setExemption).toHaveBeenCalledWith(buyer.userId, true);
    expect(screen.getByText("fan can message you for free.")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Charge them again" }));
    expect(eapi.setExemption).toHaveBeenLastCalledWith(buyer.userId, false);
  });

  it("shows nothing to someone who doesn't charge", async () => {
    vi.mocked(eapi.getEarningSettings).mockResolvedValue({ available: true, creatorReady: false, paidMessagesMode: "off", paidMessagePriceMinor: null, tipsEnabled: false, acceptsMessageRequests: true, exemptions: [] });
    const { container } = render(<ExemptionToggle userId={buyer.userId} name="fan" />);
    await waitFor(() => expect(eapi.getEarningSettings).toHaveBeenCalled());
    expect(container).toBeEmptyDOMElement();
  });
});
