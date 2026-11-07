// Templates for text stories: a look (background, font, colours) and often a sticker to fill in. They are
// starting points: everything stays editable, and nothing is fetched from anywhere.
import { newStickerId, parseStickers, type StoryBackground, type StoryFont, type StoryOverlay, type StorySticker } from "../storyCrypto.js";

export interface TextTemplate {
  id: string;
  label: string;
  /** The words to start from. */
  text: string;
  background: StoryBackground;
  font: StoryFont;
  /** Stickers with their fields to fill in; ids are made when the template is used. */
  stickers: (() => StorySticker)[];
  overlays: StoryOverlay[];
}

const place = { x: 0.5, scale: 1, rotation: 0 };
const emoji = (text: string, x: number, y: number, scale = 1): StoryOverlay => ({ type: "emoji", text, x, y, scale, rotation: 0, color: "white", boxed: false, font: "classic" });

export const TEXT_TEMPLATES: TextTemplate[] = [
  { id: "plain", label: "Plain", text: "", background: "sunset", font: "classic", stickers: [], overlays: [] },
  {
    id: "this-or-that",
    label: "This or that",
    text: "This or that?",
    background: "ocean",
    font: "strong",
    stickers: [() => ({ ...place, y: 0.62, id: newStickerId(), type: "poll", question: "Which one?", options: ["This", "That"], results: "everyone" })],
    overlays: [],
  },
  {
    id: "ask-me",
    label: "Ask me anything",
    text: "Ask me anything",
    background: "berry",
    font: "modern",
    stickers: [() => ({ ...place, y: 0.62, id: newStickerId(), type: "question", prompt: "Ask me anything", anonymous: false })],
    overlays: [],
  },
  {
    id: "mood",
    label: "Mood check",
    text: "How are you today?",
    background: "mint",
    font: "modern",
    stickers: [() => ({ ...place, y: 0.62, id: newStickerId(), type: "slider", prompt: "Mood", emoji: "😍", results: "everyone" })],
    overlays: [],
  },
  {
    id: "countdown",
    label: "Countdown",
    text: "Coming soon",
    background: "night",
    font: "strong",
    stickers: [() => ({ ...place, y: 0.62, id: newStickerId(), type: "countdown", title: "The big day", endsAt: Date.now() + 7 * 86_400_000 })],
    overlays: [emoji("🎉", 0.5, 0.18, 1.4)],
  },
  { id: "announcement", label: "Announcement", text: "Big news!", background: "ember", font: "strong", stickers: [], overlays: [emoji("📣", 0.5, 0.2, 1.6)] },
  { id: "gratitude", label: "Grateful for", text: "Today I'm grateful for…", background: "forest", font: "classic", stickers: [], overlays: [emoji("🌱", 0.5, 0.78, 1.4)] },
  { id: "quote", label: "Quote", text: "“Write something worth remembering.”", background: "slate", font: "typewriter", stickers: [], overlays: [] },
];

/** A template's content, with fresh sticker ids and every sticker checked by the same parser a viewer uses. */
export function applyTemplate(t: TextTemplate): { text: string; background: StoryBackground; font: StoryFont; stickers: StorySticker[]; overlays: StoryOverlay[] } {
  return {
    text: t.text,
    background: t.background,
    font: t.font,
    stickers: parseStickers(t.stickers.map((make) => make())),
    overlays: t.overlays.map((o) => ({ ...o })),
  };
}
