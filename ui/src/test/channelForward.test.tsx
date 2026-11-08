// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import { parseStructured, serializeChannelForward, serializeForwarded } from "../lib/richMessage.js";
import { MessageBubble } from "../components/MessageBubble.js";
import type { Message } from "../types.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  recordChannelViews: vi.fn(async () => {}),
}));

const CHANNEL = "11111111-2222-4333-8444-555555555555";
const POST = "aaaaaaaa-0000-4000-8000-000000000001";

beforeEach(() => {
  vi.clearAllMocks();
  localStorage.clear();
});

describe("a channel post forwarded into a chat", () => {
  it("keeps which post it was, inside the encrypted message", () => {
    const parsed = parseStructured(serializeChannelForward("Hello everyone", { channelId: CHANNEL.toUpperCase(), postId: POST, channelName: "RedVault news" }));
    expect(parsed).toEqual({
      kind: "text",
      text: "Hello everyone",
      forwarded: true,
      channelPost: { channelId: CHANNEL, postId: POST, channelName: "RedVault news" },
    });
  });

  it("is still a plain forwarded message to an app that doesn't know channel posts", () => {
    const body = JSON.parse(serializeChannelForward("Hello", { channelId: CHANNEL, postId: POST, channelName: "News" }).slice("\u0000rv-msg:".length));
    expect(body).toMatchObject({ type: "text", version: 1, text: "Hello", forwarded: true });
    expect(parseStructured(serializeForwarded("Hello"))).toEqual({ kind: "text", text: "Hello", forwarded: true });
  });

  it("rejects a malformed reference", () => {
    const bad = "\u0000rv-msg:" + JSON.stringify({ type: "text", version: 1, text: "x", channelPost: { channelId: "nope", postId: POST, channelName: "N" } });
    expect(parseStructured(bad)).toEqual({ kind: "invalid" });
  });

  it("shows where it came from, opens the channel, and counts one view per device", async () => {
    const message = {
      id: "m1",
      from: "them",
      text: "Hello everyone",
      time: "10:00",
      forwarded: true,
      channelPost: { channelId: CHANNEL, postId: POST, channelName: "RedVault news" },
    } as unknown as Message;
    const onOpenChannel = vi.fn();
    const user = userEvent.setup();
    const first = render(<MessageBubble message={message} onOpenChannel={onOpenChannel} />);
    await user.click(screen.getByRole("button", { name: "↪ Forwarded from RedVault news" }));
    expect(onOpenChannel).toHaveBeenCalledWith(CHANNEL);
    expect(api.recordChannelViews).toHaveBeenCalledWith([POST]);
    first.unmount();

    render(<MessageBubble message={message} />);
    expect(api.recordChannelViews).toHaveBeenCalledTimes(1);
  });
});
