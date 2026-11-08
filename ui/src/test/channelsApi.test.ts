// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { afterEach, beforeAll, describe, expect, it, vi } from "vitest";
import { getChannel, getChannelPosts, recordChannelViews, searchChannels } from "../lib/api.js";
import {
  channelAccessToken,
  channelSecretFromFragment,
  channelUrl,
  follow,
  hasUnread,
  markSeen,
  newChannelSecret,
  openChannelSecret,
  sealChannelSecret,
  unfollow,
} from "../lib/channels.js";
import * as crypto from "../lib/e2eCrypto.js";

const ID = "11111111-2222-4333-8444-555555555555";

beforeAll(async () => {
  await crypto.ready();
});

describe("reading channels", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("never sends the session cookie, so the server can't tell which account reads what", async () => {
    const fetchMock = vi.fn(async () => new Response("[]", { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    await searchChannels("news");
    await getChannel(ID).catch(() => {});
    await getChannelPosts(ID, "2026-10-18T09:00:00.000Z");
    expect(fetchMock).toHaveBeenCalledTimes(3);
    for (const [url, init] of fetchMock.mock.calls as unknown as [string, RequestInit][]) {
      expect(init.credentials).toBe("omit");
      expect(url).toContain("/public/channels");
    }
    expect((fetchMock.mock.calls[0] as unknown as [string])[0]).toContain("q=news");
  });

  it("sends a private channel's access token, and counts views without the cookie", async () => {
    const fetchMock = vi.fn(async () => new Response(null, { status: 204 }));
    vi.stubGlobal("fetch", fetchMock);
    await getChannel(ID, "dG9rZW4=").catch(() => {});
    await recordChannelViews(["aaaaaaaa-0000-4000-8000-000000000001"]);
    const [[, read], [viewsUrl, views]] = fetchMock.mock.calls as unknown as [string, RequestInit][];
    expect((read.headers as Record<string, string>)["X-Channel-Access"]).toBe("dG9rZW4=");
    expect(viewsUrl).toContain("/public/channels/views");
    expect(views.credentials).toBe("omit");
    expect(JSON.parse(String(views.body))).toEqual({ postIds: ["aaaaaaaa-0000-4000-8000-000000000001"] });
  });
});

describe("private links", () => {
  it("keep the secret in the fragment; the server gets only a token derived from it", () => {
    const secret = newChannelSecret();
    const url = new URL(channelUrl(ID, secret));
    expect(url.searchParams.get("channel")).toBe(ID);
    expect(url.search).not.toContain(secret);
    expect(channelSecretFromFragment(url.hash)).toBe(secret);
    expect(channelSecretFromFragment("#too-short")).toBeNull();
    expect(channelAccessToken(secret)).not.toContain(secret);
    expect(channelAccessToken(secret)).toBe(channelAccessToken(secret));
  });

  it("the owner keeps the secret sealed to their own key", () => {
    const owner = crypto.generateKeyPair();
    const other = crypto.generateKeyPair();
    const secret = newChannelSecret();
    const sealed = sealChannelSecret(secret, owner.publicKey);
    expect(openChannelSecret(sealed, owner.publicKey, owner.privateKey)).toBe(secret);
    expect(openChannelSecret(sealed, other.publicKey, other.privateKey)).toBeNull();
    expect(openChannelSecret(null, owner.publicKey, owner.privateKey)).toBeNull();
  });

  it("a public channel's link has no secret", () => {
    expect(new URL(channelUrl(ID)).hash).toBe("");
  });
});

describe("following, on this device", () => {
  const channel = { id: ID, name: "News", description: "", createdAt: "", lastPostAt: "2026-10-18T09:00:00.000Z", isPrivate: false, avatarUrl: null, acceptSuggestions: false };

  it("counts what's already posted as seen, and only newer posts as new", () => {
    const list = follow([], channel);
    expect(list).toEqual([{ id: ID, name: "News", seenAt: channel.lastPostAt }]);
    expect(follow(list, channel)).toBe(list);
    expect(hasUnread(list[0], channel)).toBe(false);
    const newer = { ...channel, name: "News+", lastPostAt: "2026-10-18T10:00:00.000Z" };
    expect(hasUnread(list[0], newer)).toBe(true);
    expect(markSeen(list, newer)).toEqual([{ id: ID, name: "News+", seenAt: newer.lastPostAt }]);
    expect(unfollow(list, ID)).toEqual([]);
  });

  it("keeps a private channel's secret with the follow", () => {
    expect(follow([], { ...channel, isPrivate: true }, "s3cret")[0].secret).toBe("s3cret");
  });
});
