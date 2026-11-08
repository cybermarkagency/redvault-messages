// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stories: the server's /api/stories (docs/stories.md). Everything here is
// ciphertext or ids; the server never sees what a story shows.
import { ApiError } from "./api.js";
import { API_BASE } from "./apiBase.js";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}/stories${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });
  if (res.status === 204) return undefined as T;
  const body = await res.json().catch(() => null);
  if (!res.ok) throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  return body as T;
}

const send = <T>(method: string, path: string, body?: unknown) => request<T>(path, { method, body: body === undefined ? undefined : JSON.stringify(body) });
const id = encodeURIComponent;

export type StoryKind = "image" | "video" | "text" | "audio";
export type StoryAudience = "friends" | "close_friends" | "custom";

export interface StoryDto {
  id: string;
  authorId: string;
  kind: StoryKind;
  metaCiphertext: string;
  metaNonce: string;
  mediaSize: number | null;
  hasMedia: boolean;
  allowReplies: boolean;
  createdAt: string;
  expiresAt: string;
  /** This viewer's copy of the story key, sealed by the author. */
  wrappedKey: string;
}

export interface FeedDto {
  own: (StoryDto & { viewCount: number })[];
  authors: { authorId: string; stories: (StoryDto & { seen: boolean })[] }[];
}

export interface HighlightDto {
  id: string;
  ownerId: string;
  titleCiphertext: string;
  titleNonce: string;
  coverStoryId: string;
  createdAt: string;
  stories: StoryDto[];
}

export interface StoryPrefsDto {
  prefsCiphertext: string | null;
  prefsNonce: string | null;
  archiveEnabled: boolean;
  revision: number;
}

export interface WrappedKeyDto {
  userId: string;
  wrappedKey: string;
}

export interface NewStoryDto {
  id: string;
  kind: StoryKind;
  metaCiphertext: string;
  metaNonce: string;
  mediaSize: number | null;
  audience: StoryAudience;
  allowReplies: boolean;
  /** How long it stays up (1, 6, 12, 24, 48 hours or 7 days, in seconds); the server uses 24 hours when left out. */
  ttlSeconds?: number;
  keys: WrappedKeyDto[];
}

/** `interactive` and `ttlSeconds` are missing from servers that predate interactive stories. */
export interface StoryConfigDto {
  /** False when the server has Stories switched off (STORIES_ENABLED): the apps hide them. Missing on older servers, which means on. */
  enabled?: boolean;
  media: boolean;
  /** Audio stories are accepted (they need media storage). */
  audio?: boolean;
  interactive?: boolean;
  ttlSeconds?: number[];
}
export const getStoryConfig = () => request<StoryConfigDto>("/config");
export const getStoryFeed = () => request<FeedDto>("/feed");
export const createStory = (story: NewStoryDto) => send<{ id: string; published: boolean; expiresAt: string }>("POST", "", story);

/** The encrypted photo or video, exactly `mediaSize` bytes. */
export async function uploadStoryMedia(storyId: string, ciphertext: Blob): Promise<void> {
  const res = await fetch(`${API_BASE}/stories/${id(storyId)}/media`, {
    method: "PUT",
    credentials: "include",
    headers: { "Content-Type": "application/octet-stream" },
    body: ciphertext,
  });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  }
}

/** Streams the encrypted media back. */
export async function downloadStoryMedia(storyId: string): Promise<AsyncIterable<Uint8Array>> {
  const res = await fetch(`${API_BASE}/stories/${id(storyId)}/media`, { credentials: "include", cache: "no-store" });
  if (!res.ok) throw new ApiError(res.status, "story_media_unavailable", "This story isn't available any more.");
  const body = res.body;
  if (!body) {
    const buf = new Uint8Array(await res.arrayBuffer());
    return (async function* () {
      yield buf;
    })();
  }
  const reader = body.getReader();
  return (async function* () {
    try {
      for (;;) {
        const chunk = await reader.read();
        if (chunk.done) return;
        yield chunk.value;
      }
    } finally {
      reader.releaseLock();
    }
  })();
}

export const markStoryViewed = (storyId: string) => send<void>("POST", `/${id(storyId)}/view`);
export const getStoryViewers = (storyId: string) => request<string[]>(`/${id(storyId)}/viewers`);
export const deleteStory = (storyId: string) => send<void>("DELETE", `/${id(storyId)}`);
export const getStoryArchive = () => request<(StoryDto & { archivedAt: string | null })[]>("/archive");
export const getStoryKeyGaps = () => request<{ storyId: string; users: { userId: string; publicKey: string }[] }[]>("/key-gaps");
export const addStoryKeys = (storyId: string, keys: WrappedKeyDto[]) => send<{ added: number }>("POST", `/${id(storyId)}/keys`, { keys });
export const revokeStoryAccess = (userId: string, storyIds?: string[]) => send<{ removed: number }>("POST", "/revoke", storyIds ? { userId, storyIds } : { userId });

// --- Interactive stickers: sealed answers in, totals out (docs/stories-advanced/) ---

export interface AnswerRowDto {
  stickerId: string;
  /** Null for an answer sent without a name. */
  responderId: string | null;
  anonymous: boolean;
  envelope: string;
  revision: number;
  respondedAt: string;
}
export interface MyAnswerDto {
  stickerId: string;
  anonymous: boolean;
  envelope: string;
  revision: number;
}
export interface TotalsRowDto {
  stickerId: string;
  revision: number;
  nonce: string;
  ciphertext: string;
}

export const putStoryAnswer = (storyId: string, stickerId: string, body: { envelope: string; anonymous: boolean; final: boolean }) =>
  send<{ revision: number }>("PUT", `/${id(storyId)}/responses/${id(stickerId)}`, body);
export const getStoryAnswers = (storyId: string) => request<AnswerRowDto[]>(`/${id(storyId)}/responses`);
export const getMyStoryAnswers = (storyId: string) => request<MyAnswerDto[]>(`/${id(storyId)}/responses/mine`);
export const putStoryTotals = (storyId: string, stickerId: string, body: { revision: number; nonce: string; ciphertext: string }) =>
  send<void>("PUT", `/${id(storyId)}/results/${id(stickerId)}`, body);
export const getStoryTotals = (storyId: string) => request<TotalsRowDto[]>(`/${id(storyId)}/results`);
export const reportStory = (storyId: string, body: { reason: string; evidence?: string }) => send<{ reported: boolean }>("POST", `/${id(storyId)}/report`, body);

export const reportStoryAnswer = (storyId: string, body: { stickerId: string; envelope: string; reason: string; evidence?: string }) =>
  send<{ reported: boolean }>("POST", `/${id(storyId)}/responses/report`, body);

export const getHighlights = (userId: string) => request<HighlightDto[]>(`/users/${id(userId)}/highlights`);
export const createHighlight = (h: { titleCiphertext: string; titleNonce: string; coverStoryId: string; storyIds: string[] }) => send<{ id: string }>("POST", "/highlights", h);
export const updateHighlight = (highlightId: string, patch: { titleCiphertext?: string; titleNonce?: string; coverStoryId?: string; storyIds?: string[] }) =>
  send<{ id: string }>("PATCH", `/highlights/${id(highlightId)}`, patch);
export const deleteHighlight = (highlightId: string) => send<void>("DELETE", `/highlights/${id(highlightId)}`);

export const getStoryPrefs = () => request<StoryPrefsDto>("/prefs");
export const putStoryPrefs = (p: { prefsCiphertext: string; prefsNonce: string; archiveEnabled: boolean; revision: number }) =>
  send<{ archiveEnabled: boolean; revision: number }>("PUT", "/prefs", p);
