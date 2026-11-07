import { useEffect, useLayoutEffect, useRef, useState } from "react";
import {
  prepareImage,
  prepareVideo,
  StoryInputError,
  storyRecipients,
  type PostStage,
  type PreparedMedia,
  type StoryAudiencePick,
  type StoryDraft,
} from "../../lib/stories.js";
import { deleteDraft, DraftsUnavailableError, listDrafts, loadDraft, MAX_SLIDES, saveDraft, type DraftContent, type DraftSlide, type DraftSummary } from "../../lib/storyDrafts.js";
import {
  MAX_OVERLAYS,
  MAX_OVERLAY_TEXT,
  MAX_STICKERS,
  MAX_STORY_TEXT,
  STORY_ANIMATIONS,
  STORY_BACKGROUNDS,
  STORY_COLORS,
  STORY_FONTS,
  type StoryAnimation,
  type StoryBackground,
  type StoryCaption,
  type StoryColor,
  type StoryFont,
  type StoryMeta,
  type StoryOverlay,
  type StorySticker,
} from "../../lib/storyCrypto.js";
import { applyTemplate, TEXT_TEMPLATES, type TextTemplate } from "../../lib/studio/textTemplates.js";
import { useAppState } from "../../state/AppStateContext.js";
import { useStories } from "../../state/StoriesContext.js";
import { EmojiPicker } from "../EmojiPicker.js";
import { CloseIcon, ImageIcon, RefreshIcon, SmileIcon, StarIcon, TrashIcon } from "../icons24.js";
import { useDialogFocus } from "../useDialogFocus.js";
import { StickerEditor, STICKER_CHOICES, type StickerType } from "./StickerEditor.js";
import { stickerLabel, stickerStyle, StickerView } from "./StickerView.js";
import { overlayClass, overlayStyle, storyColor, StoryFrame } from "./StoryFrame.js";
import { AudioStudio, type AudioResult } from "./studio/AudioStudio.js";
import { CameraCapture } from "./studio/CameraCapture.js";
import { CaptionEditor } from "./studio/CaptionEditor.js";
import { CollageMaker } from "./studio/CollageMaker.js";
import { PhotoEditor } from "./studio/PhotoEditor.js";
import styles from "./Stories.module.css";

const FONT_LABELS: Record<StoryFont, string> = { classic: "Classic", modern: "Modern", typewriter: "Typewriter", strong: "Strong" };
const ANIM_LABELS: Record<StoryAnimation, string> = { none: "Still", pulse: "Pulse", bounce: "Bounce", fade: "Fade" };
const DEFAULT_TTL = 24 * 3600;

/** "1 hour", "24 hours", "48 hours", "7 days". */
export function ttlLabel(seconds: number): string {
  if (seconds === 7 * 86400) return "7 days";
  const hours = Math.round(seconds / 3600);
  return hours === 1 ? "1 hour" : `${hours} hours`;
}

interface TextEdit {
  index: number | null;
  text: string;
  color: StoryColor;
  font: StoryFont;
  boxed: boolean;
}

type Drag = { kind: "overlay" | "sticker"; index: number; dx: number; dy: number };
type Mode = "pick" | "media" | "text" | "audio";
type Studio = "camera" | "editor" | "collage" | "voice" | "audiofile" | "captions" | null;

/** One slide of a story as the composer holds it. "pick" is a slide nothing has been chosen for yet. */
interface Slide {
  mode: Mode;
  media: PreparedMedia | null;
  text: string;
  background: StoryBackground;
  font: StoryFont;
  overlays: StoryOverlay[];
  stickers: StorySticker[];
  captions: StoryCaption[];
}

const emptySlide = (): Slide => ({ mode: "pick", media: null, text: "", background: "sunset", font: "classic", overlays: [], stickers: [], captions: [] });
const hasContent = (s: Slide) => s.mode !== "pick" && (s.media !== null || s.text.trim() !== "" || s.overlays.length > 0 || s.stickers.length > 0);
/** Whether a slide could be shared as it is. */
const isReady = (s: Slide) => (s.mode === "text" ? s.text.trim() !== "" : s.mode === "pick" ? false : s.media !== null);
const toDraftSlide = (s: Slide): DraftSlide => ({ ...s, mode: s.mode === "pick" ? "text" : s.mode });
const fromDraftSlide = (s: DraftSlide): Slide => ({ ...s });

/** Make a story: photos, video, voice or text, with text, stickers and polls you can drag around, over one or more slides. */
export function StoryComposer() {
  const { state, dispatch } = useAppState();
  const identity = state.identity!;
  const stories = useStories();
  const root = useRef<HTMLDivElement>(null);
  const frameRef = useRef<HTMLDivElement>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const textRef = useRef<HTMLTextAreaElement>(null);
  const previewRef = useRef<string | null>(null);
  const [mode, setMode] = useState<Mode>("pick");
  const [media, setMedia] = useState<PreparedMedia | null>(null);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [text, setText] = useState("");
  const [background, setBackground] = useState<StoryBackground>("sunset");
  const [font, setFont] = useState<StoryFont>("classic");
  const [overlays, setOverlays] = useState<StoryOverlay[]>([]);
  const [stickers, setStickers] = useState<StorySticker[]>([]);
  const [captions, setCaptions] = useState<StoryCaption[]>([]);
  const [slides, setSlides] = useState<Slide[]>([emptySlide()]);
  const [active, setActive] = useState(0);
  const [studio, setStudio] = useState<Studio>(null);
  const [selected, setSelected] = useState<number | null>(null);
  const [selectedSticker, setSelectedSticker] = useState<number | null>(null);
  const [textEdit, setTextEdit] = useState<TextEdit | null>(null);
  const [stickerSheet, setStickerSheet] = useState(false);
  const [stickerEdit, setStickerEdit] = useState<{ type: StickerType; index: number | null } | null>(null);
  const [picker, setPicker] = useState(false);
  const [audience, setAudience] = useState<StoryAudiencePick>("friends");
  const [customIds, setCustomIds] = useState<string[]>([]);
  const [peopleOpen, setPeopleOpen] = useState(false);
  const [ttlSeconds, setTtlSeconds] = useState(DEFAULT_TTL);
  const [allowReplies, setAllowReplies] = useState(stories.prefs.allowReplies);
  const [stage, setStage] = useState<PostStage | "preparing" | null>(null);
  const [progress, setProgress] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [drafts, setDrafts] = useState<DraftSummary[]>([]);
  const [draftId, setDraftId] = useState<string | null>(null);
  const [closePrompt, setClosePrompt] = useState(false);
  const drag = useRef<Drag | null>(null);

  const snapshot = (): Slide => ({ mode, media, text, background, font, overlays, stickers, captions });
  /** Every slide, with the one being edited brought up to date. */
  const flush = (): Slide[] => slides.map((s, i) => (i === active ? snapshot() : s));
  const dirty = flush().some(hasContent);
  const busy = stage !== null && stage !== "done";

  function close() {
    if (busy) return;
    if (dirty) return setClosePrompt(true);
    stories.closeComposer();
  }
  useDialogFocus(
    root,
    studio
      ? () => setStudio(null)
      : textEdit
        ? () => setTextEdit(null)
        : stickerEdit
          ? () => setStickerEdit(null)
          : stickerSheet
            ? () => setStickerSheet(false)
            : peopleOpen
              ? () => setPeopleOpen(false)
              : closePrompt
                ? () => setClosePrompt(false)
                : picker
                  ? () => setPicker(false)
                  : close
  );

  // The text box is as tall as its text, so the words sit exactly where the viewer will centre them.
  useLayoutEffect(() => {
    const el = textRef.current;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = `${el.scrollHeight}px`;
  }, [text, font, mode]);

  // The picture shown while editing (a voice story has none).
  function swapPreview(m: PreparedMedia | null) {
    if (previewRef.current) URL.revokeObjectURL(previewRef.current);
    previewRef.current = m && m.kind !== "audio" ? URL.createObjectURL(m.blob) : null;
    setPreviewUrl(previewRef.current);
  }
  useEffect(
    () => () => {
      if (previewRef.current) URL.revokeObjectURL(previewRef.current);
    },
    []
  );

  // Keyed on the account, not the identity object: a new object each render must not reload the list.
  const identityRef = useRef(identity);
  identityRef.current = identity;
  useEffect(() => {
    let alive = true;
    listDrafts(identityRef.current).then(
      (list) => alive && setDrafts(list),
      () => alive && setDrafts([])
    );
    return () => {
      alive = false;
    };
  }, [identity.userId]);

  const friends = [...stories.friends.values()];
  const hidden = new Set(stories.prefs.hideFrom);
  const pickable = friends.filter((f) => !hidden.has(f.userId));
  const closeCount = storyRecipients(friends, stories.prefs, "close_friends").length;
  const everyoneCount = storyRecipients(friends, stories.prefs, "friends").length;
  const customCount = storyRecipients(friends, stories.prefs, "custom", customIds).length;
  const reach = audience === "friends" ? everyoneCount : audience === "close_friends" ? closeCount : customCount;
  const ttlChoices = stories.ttlChoices.length ? stories.ttlChoices : [DEFAULT_TTL];

  // --- Slides --------------------------------------------------------------------------------------

  function loadSlide(s: Slide) {
    setMode(s.mode);
    setMedia(s.media);
    swapPreview(s.media);
    setText(s.text);
    setBackground(s.background);
    setFont(s.font);
    setOverlays(s.overlays);
    setStickers(s.stickers);
    setCaptions(s.captions);
    setSelected(null);
    setSelectedSticker(null);
    setStudio(null);
  }

  function addSlide() {
    if (!hasContent(snapshot())) return setError("Add something to this slide first.");
    if (slides.length >= MAX_SLIDES) return setError(`At most ${MAX_SLIDES} slides in one story.`);
    setError(null);
    const all = [...flush(), emptySlide()];
    setSlides(all);
    setActive(all.length - 1);
    loadSlide(all[all.length - 1]);
  }

  function selectSlide(i: number) {
    if (i === active) return;
    const all = flush();
    setSlides(all);
    setActive(i);
    loadSlide(all[i]);
    setError(null);
  }

  function removeSlide(i: number) {
    const all = flush();
    all.splice(i, 1);
    const next = all.length ? all : [emptySlide()];
    const nextActive = Math.min(i, next.length - 1);
    setSlides(next);
    setActive(nextActive);
    loadSlide(next[nextActive]);
  }

  const strip = slides.length > 1 || mode !== "pick" ? (
    <div className={styles.slideStrip} role="group" aria-label="Slides">
      {slides.map((_, i) => (
        <button key={i} type="button" className={i === active ? styles.audienceOn : ""} aria-pressed={i === active} aria-label={`Slide ${i + 1}${i === active ? " (editing)" : ""}`} onClick={() => selectSlide(i)}>
          {i + 1}
        </button>
      ))}
      <button type="button" aria-label="Add a slide" disabled={busy || slides.length >= MAX_SLIDES} onClick={addSlide}>
        + Slide
      </button>
      {slides.length > 1 && (
        <button type="button" aria-label={`Remove slide ${active + 1}`} disabled={busy} onClick={() => removeSlide(active)}>
          <TrashIcon size={12} />
        </button>
      )}
    </div>
  ) : null;

  // --- Getting media -------------------------------------------------------------------------------

  function acceptMedia(prepared: PreparedMedia) {
    setError(null);
    setMedia(prepared);
    swapPreview(prepared);
    setCaptions([]);
    setMode("media");
    setStudio(null);
  }

  function acceptAudio(result: AudioResult) {
    setError(null);
    setMedia(result.media);
    swapPreview(result.media);
    setText(result.title);
    setBackground(result.background);
    setCaptions(result.captions);
    setMode("audio");
    setStudio(null);
  }

  async function pickFile(file: File | undefined) {
    if (!file) return;
    setError(null);
    setStage("preparing");
    try {
      const prepared = file.type.startsWith("video/") ? await prepareVideo(file) : await prepareImage(file);
      acceptMedia(prepared);
    } catch (err) {
      setError(err instanceof StoryInputError ? err.message : "That file couldn't be used.");
    } finally {
      setStage(null);
    }
  }

  function useTemplate(t: TextTemplate) {
    const a = applyTemplate(t);
    setText(a.text);
    setBackground(a.background);
    setFont(a.font);
    setOverlays(a.overlays);
    setStickers(a.stickers);
    setMode("text");
  }

  // --- Things on the story ---------------------------------------------------------------------------

  function addOverlay(o: Omit<StoryOverlay, "x" | "y" | "scale" | "rotation">) {
    if (overlays.length >= MAX_OVERLAYS) {
      setError(`At most ${MAX_OVERLAYS} things on one story.`);
      return;
    }
    setOverlays([...overlays, { ...o, x: 0.5, y: 0.45, scale: 1, rotation: 0 }]);
    setSelectedSticker(null);
    setSelected(overlays.length);
  }

  function updateOverlay(index: number, change: Partial<StoryOverlay>) {
    setOverlays(overlays.map((o, i) => (i === index ? { ...o, ...change } : o)));
  }

  function removeOverlay(index: number) {
    setOverlays(overlays.filter((_, i) => i !== index));
    setSelected(null);
  }

  function updateSticker(index: number, change: Partial<StorySticker>) {
    setStickers((list) => list.map((s, i) => (i === index ? ({ ...s, ...change } as StorySticker) : s)));
  }

  function removeSticker(index: number) {
    setStickers(stickers.filter((_, i) => i !== index));
    setSelectedSticker(null);
  }

  function saveSticker(sticker: StorySticker) {
    if (!stickerEdit) return;
    if (stickerEdit.index === null) {
      setStickers([...stickers, { ...sticker, y: 0.5 + 0.1 * (stickers.length % 3) }]);
      setSelected(null);
      setSelectedSticker(stickers.length);
    } else {
      setStickers(stickers.map((s, i) => (i === stickerEdit.index ? sticker : s)));
    }
    setStickerEdit(null);
  }

  function saveText() {
    if (!textEdit) return;
    const t = textEdit.text.trim().slice(0, MAX_OVERLAY_TEXT);
    if (textEdit.index === null) {
      if (t) addOverlay({ type: "text", text: t, color: textEdit.color, font: textEdit.font, boxed: textEdit.boxed });
    } else if (!t) removeOverlay(textEdit.index);
    else updateOverlay(textEdit.index, { text: t, color: textEdit.color, font: textEdit.font, boxed: textEdit.boxed });
    setTextEdit(null);
  }

  function startDrag(e: React.PointerEvent, kind: Drag["kind"], index: number, x: number, y: number) {
    e.stopPropagation();
    const rect = frameRef.current?.getBoundingClientRect();
    if (!rect) return;
    drag.current = { kind, index, dx: (e.clientX - rect.left) / rect.width - x, dy: (e.clientY - rect.top) / rect.height - y };
    if (kind === "overlay") {
      setSelectedSticker(null);
      setSelected(index);
    } else {
      setSelected(null);
      setSelectedSticker(index);
    }
    (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
  }
  function onPointerMove(e: React.PointerEvent) {
    const d = drag.current;
    const rect = frameRef.current?.getBoundingClientRect();
    if (!d || !rect) return;
    const x = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width - d.dx));
    const y = Math.min(1, Math.max(0, (e.clientY - rect.top) / rect.height - d.dy));
    if (d.kind === "overlay") setOverlays((list) => list.map((o, i) => (i === d.index ? { ...o, x, y } : o)));
    else setStickers((list) => list.map((s, i) => (i === d.index ? ({ ...s, x, y } as StorySticker) : s)));
  }
  function onPointerUp() {
    drag.current = null;
  }

  // --- Drafts and sharing -------------------------------------------------------------------------------

  const draftOf = (list: Slide[]): DraftContent => ({ slides: list.filter(hasContent).map(toDraftSlide), audience, customIds, allowReplies, ttlSeconds });

  async function keepDraft(list: Slide[] = flush()): Promise<string | null> {
    try {
      const id = await saveDraft(identity, draftOf(list), draftId ?? undefined);
      setDraftId(id);
      return id;
    } catch (err) {
      setError(err instanceof DraftsUnavailableError ? err.message : "Couldn't save the draft.");
      return null;
    }
  }

  async function openDraft(id: string) {
    setError(null);
    setStage("preparing");
    try {
      const d = await loadDraft(identity, id);
      if (!d) {
        setError("That draft can't be opened any more.");
        setDrafts((list) => list.filter((x) => x.id !== id));
        await deleteDraft(id).catch(() => {});
        return;
      }
      const list = d.slides.map(fromDraftSlide);
      setSlides(list);
      setActive(0);
      loadSlide(list[0]);
      setAudience(d.audience);
      setCustomIds(d.customIds);
      setAllowReplies(d.allowReplies);
      setTtlSeconds(ttlChoices.includes(d.ttlSeconds) ? d.ttlSeconds : DEFAULT_TTL);
      setDraftId(id);
    } catch {
      setError("That draft can't be opened any more.");
    } finally {
      setStage(null);
    }
  }

  const draftFor = (s: Slide): StoryDraft => ({
    media: s.media ?? undefined,
    text: s.mode === "text" ? s.text.slice(0, MAX_STORY_TEXT) : s.mode === "audio" ? s.text.slice(0, MAX_STORY_TEXT) : undefined,
    background: s.background,
    font: s.font,
    overlays: s.overlays,
    stickers: s.stickers,
    captions: s.captions,
    audience,
    customIds,
    allowReplies,
    ttlSeconds,
  });

  async function share() {
    setError(null);
    const ready = flush().filter(hasContent);
    if (ready.length === 0) return;
    const incomplete = ready.findIndex((s) => !isReady(s));
    if (incomplete >= 0) return setError(`Slide ${incomplete + 1} ${ready[incomplete].mode === "text" ? "needs some text" : "needs its photo, video or recording"}.`);
    let shared = 0;
    try {
      for (const slide of ready) {
        if (ready.length > 1) setProgress(`Slide ${shared + 1} of ${ready.length}`);
        await stories.publish(draftFor(slide), setStage);
        shared++;
      }
    } catch (err) {
      setStage(null);
      setProgress(null);
      // What wasn't shared stays here, and is saved as a draft, so a dropped connection loses nothing.
      const rest = ready.slice(shared);
      setSlides(rest);
      setActive(0);
      loadSlide(rest[0]);
      const saved = await keepDraft(rest);
      const why = err instanceof StoryInputError ? err.message : err instanceof Error && err.message ? err.message : "Couldn't share your story.";
      setError(`${shared > 0 ? `${shared} of ${ready.length} slides shared. ` : ""}${why}${saved ? " What's left is saved as a draft: press Share to try again." : " Try again."}`);
      return;
    }
    if (draftId) await deleteDraft(draftId).catch(() => {});
    stories.closeComposer();
  }

  const meta: StoryMeta =
    mode === "audio"
      ? { v: 1, kind: "audio", overlays, closeFriends: audience === "close_friends", background, font, text, waveform: media?.waveform }
      : { v: 1, kind: mode === "media" && media && media.kind !== "audio" ? media.kind : "text", overlays, closeFriends: audience === "close_friends", background, font, text };
  const sel = selected !== null ? overlays[selected] : null;
  const selSticker = selectedSticker !== null ? stickers[selectedSticker] : null;
  const interactiveOk = stories.interactiveEnabled;
  const canCaption = mode === "audio" || (mode === "media" && media?.kind === "video");
  const slideCount = flush().filter(hasContent).length;

  return (
    <div className={styles.viewer} ref={root} role="dialog" aria-modal="true" aria-label="New story">
      <div className={styles.stage}>
        {mode === "pick" ? (
          <div className={styles.pick}>
            <h2 className={styles.pickTitle}>Add to your story</h2>
            <p className={styles.pickNote}>Seen by your friends for as long as you choose. End-to-end encrypted: the server can't see it.</p>
            <button type="button" className={styles.pickButton} disabled={!stories.mediaEnabled || busy} onClick={() => setStudio("camera")}>
              <ImageIcon size={22} />
              Camera
            </button>
            <button type="button" className={styles.pickButton} disabled={!stories.mediaEnabled || busy} onClick={() => fileInput.current?.click()}>
              <ImageIcon size={22} />
              {stage === "preparing" ? "Preparing…" : "Photo or video"}
            </button>
            <button type="button" className={styles.pickButton} disabled={!stories.mediaEnabled || busy} onClick={() => setStudio("collage")}>
              <ImageIcon size={22} />
              Collage
            </button>
            <button type="button" className={styles.pickButton} disabled={!stories.mediaEnabled || busy} onClick={() => setStudio("voice")}>
              <span className={styles.aa}>🎙</span>
              Voice or audio
            </button>
            {!stories.mediaEnabled && <p className={styles.pickNote}>Photo, video and voice stories aren't available on this server yet.</p>}
            <button type="button" className={styles.pickButton} onClick={() => setMode("text")}>
              <span className={styles.aa}>Aa</span>
              Text
            </button>
            <div className={styles.templateRow} role="group" aria-label="Templates">
              {TEXT_TEMPLATES.filter((t) => t.id !== "plain").map((t) => (
                <button key={t.id} type="button" className={styles.templateChip} onClick={() => useTemplate(t)}>
                  {t.label}
                </button>
              ))}
            </div>
            <input
              ref={fileInput}
              type="file"
              accept="image/jpeg,image/png,image/webp,image/heic,image/gif,video/mp4,video/webm,video/quicktime"
              hidden
              onChange={(e) => {
                void pickFile(e.target.files?.[0]);
                e.target.value = "";
              }}
            />
            {strip}
            {drafts.length > 0 && (
              <section className={styles.drafts} aria-label="Drafts">
                <h3 className={styles.draftsTitle}>Drafts</h3>
                <p className={styles.pickNote}>Saved on this device only, encrypted. They are never sent to RedVault.</p>
                <ul className={styles.sheetList}>
                  {drafts.map((d) => (
                    <li key={d.id} className={styles.sheetRow}>
                      <span className={styles.sheetGrow}>
                        {d.preview || "Story"}
                        {d.slideCount > 1 ? ` · ${d.slideCount} slides` : ""}
                        {d.stickerCount > 0 ? ` · ${d.stickerCount} sticker${d.stickerCount === 1 ? "" : "s"}` : ""}
                        <span className={styles.draftAge}> · {new Date(d.updatedAt).toLocaleDateString()}</span>
                      </span>
                      <button type="button" className={styles.sheetButton} disabled={busy} onClick={() => openDraft(d.id)}>
                        Continue
                      </button>
                      <button
                        type="button"
                        className={styles.sheetButton}
                        aria-label={`Delete draft ${d.preview || "story"}`}
                        onClick={() => {
                          setDrafts((list) => list.filter((x) => x.id !== d.id));
                          void deleteDraft(d.id).catch(() => {});
                        }}
                      >
                        <TrashIcon size={14} />
                      </button>
                    </li>
                  ))}
                </ul>
              </section>
            )}
            {error && <p className={styles.sheetError}>{error}</p>}
          </div>
        ) : (
          <>
            <div
              className={styles.tapArea}
              ref={frameRef}
              onPointerDown={() => {
                setSelected(null);
                setSelectedSticker(null);
              }}
              onPointerMove={onPointerMove}
              onPointerUp={onPointerUp}
            >
              <StoryFrame meta={meta} mediaUrl={previewUrl} muted editing>
                {mode === "text" && (
                  <textarea
                    ref={textRef}
                    rows={1}
                    className={`${styles.storyText} ${styles.storyTextInput} ${styles[`font_${font}`]}`}
                    placeholder="Type something"
                    aria-label="Story text"
                    maxLength={MAX_STORY_TEXT}
                    value={text}
                    onChange={(e) => setText(e.target.value)}
                    onPointerDown={(e) => e.stopPropagation()}
                  />
                )}
                {overlays.map((o, i) => (
                  <div
                    key={i}
                    className={`${overlayClass({ ...o, anim: undefined })} ${styles.overlayEditable} ${selected === i ? styles.overlaySelected : ""}`}
                    style={overlayStyle(o)}
                    role="button"
                    tabIndex={0}
                    aria-label={`${o.type === "emoji" ? `Sticker ${o.text}` : `Text: ${o.text}`}${o.anim ? `, ${ANIM_LABELS[o.anim].toLowerCase()} motion` : ""}`}
                    onPointerDown={(e) => startDrag(e, "overlay", i, o.x, o.y)}
                    onDoubleClick={() => o.type === "text" && setTextEdit({ index: i, text: o.text, color: o.color, font: o.font, boxed: o.boxed })}
                    onWheel={(e) => updateOverlay(i, { scale: Math.min(4, Math.max(0.5, o.scale * (e.deltaY < 0 ? 1.08 : 0.92))) })}
                    onKeyDown={(e) => {
                      if (e.key === "Delete" || e.key === "Backspace") removeOverlay(i);
                    }}
                  >
                    {o.text}
                  </div>
                ))}
                {stickers.map((s, i) => (
                  <div
                    key={s.id}
                    className={`${styles.overlay} ${styles.stickerWrap} ${styles.stickerWrapEdit} ${styles.overlayEditable} ${selectedSticker === i ? styles.overlaySelected : ""}`}
                    style={stickerStyle(s)}
                    role="button"
                    tabIndex={0}
                    aria-label={`${stickerLabel(s)}. Drag to move; arrow keys nudge; Delete removes.`}
                    onPointerDown={(e) => startDrag(e, "sticker", i, s.x, s.y)}
                    onDoubleClick={() => setStickerEdit({ type: s.type, index: i })}
                    onFocus={() => {
                      setSelected(null);
                      setSelectedSticker(i);
                    }}
                    onWheel={(e) => updateSticker(i, { scale: Math.min(2.5, Math.max(0.6, s.scale * (e.deltaY < 0 ? 1.08 : 0.92))) })}
                    onKeyDown={(e) => {
                      const step = 0.02;
                      if (e.key === "Delete" || e.key === "Backspace") removeSticker(i);
                      else if (e.key === "ArrowLeft") updateSticker(i, { x: Math.max(0, s.x - step) });
                      else if (e.key === "ArrowRight") updateSticker(i, { x: Math.min(1, s.x + step) });
                      else if (e.key === "ArrowUp") updateSticker(i, { y: Math.max(0, s.y - step) });
                      else if (e.key === "ArrowDown") updateSticker(i, { y: Math.min(1, s.y + step) });
                      else return;
                      e.preventDefault();
                    }}
                  >
                    <StickerView sticker={s} mode="edit" friends={stories.friends} />
                  </div>
                ))}
              </StoryFrame>
            </div>

            <div className={styles.top}>
              <div className={styles.header}>
                <button type="button" className={styles.chromeButton} aria-label="Close" onClick={close}>
                  <CloseIcon size={20} />
                </button>
                <span className={styles.headerSpacer} />
                {(mode === "text" || mode === "audio") && (
                  <button
                    type="button"
                    className={styles.chromeButton}
                    aria-label="Change background"
                    onClick={() => setBackground(STORY_BACKGROUNDS[(STORY_BACKGROUNDS.indexOf(background) + 1) % STORY_BACKGROUNDS.length])}
                  >
                    <RefreshIcon size={18} />
                  </button>
                )}
                {mode === "text" && (
                  <button
                    type="button"
                    className={`${styles.chromeButton} ${styles.chromeText}`}
                    aria-label={`Font: ${FONT_LABELS[font]}`}
                    onClick={() => setFont(STORY_FONTS[(STORY_FONTS.indexOf(font) + 1) % STORY_FONTS.length])}
                  >
                    {FONT_LABELS[font]}
                  </button>
                )}
                {mode === "media" && media?.kind === "image" && (
                  <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-label="Edit the photo: crop, filters, draw" onClick={() => setStudio("editor")}>
                    Edit
                  </button>
                )}
                {mode === "audio" && (
                  <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-label="Edit the recording" onClick={() => setStudio("voice")}>
                    Edit
                  </button>
                )}
                {canCaption && (
                  <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} aria-label="Captions" onClick={() => setStudio("captions")}>
                    Captions{captions.length ? ` (${captions.length})` : ""}
                  </button>
                )}
                <button
                  type="button"
                  className={`${styles.chromeButton} ${styles.chromeText}`}
                  aria-label="Add text"
                  onClick={() => setTextEdit({ index: null, text: "", color: "white", font: "classic", boxed: false })}
                >
                  Aa
                </button>
                <span className={styles.pickerAnchor}>
                  <button type="button" className={styles.chromeButton} aria-label="Add an emoji" onClick={() => setPicker(!picker)}>
                    <SmileIcon size={20} />
                  </button>
                  {picker && (
                    <EmojiPicker
                      alignEnd
                      onPick={(emoji) => {
                        addOverlay({ type: "emoji", text: emoji, color: "white", font: "classic", boxed: false });
                        setPicker(false);
                      }}
                      onClose={() => setPicker(false)}
                    />
                  )}
                </span>
                <button
                  type="button"
                  className={`${styles.chromeButton} ${styles.chromeText}`}
                  aria-label="Add a sticker: poll, question, countdown and more"
                  aria-haspopup="dialog"
                  onClick={() => setStickerSheet(true)}
                >
                  Stickers
                </button>
                <button
                  type="button"
                  className={`${styles.chromeButton} ${styles.chromeText}`}
                  aria-label="Save as a draft"
                  disabled={busy}
                  onClick={async () => setNotice((await keepDraft()) ? "Draft saved on this device" : null)}
                >
                  Draft
                </button>
              </div>
              {sel && selected !== null && !textEdit && (
                <div className={styles.overlayTools} aria-label="Selected item">
                  <button type="button" onClick={() => updateOverlay(selected, { scale: Math.max(0.5, sel.scale / 1.2) })} aria-label="Smaller">
                    A−
                  </button>
                  <button type="button" onClick={() => updateOverlay(selected, { scale: Math.min(4, sel.scale * 1.2) })} aria-label="Bigger">
                    A+
                  </button>
                  <button type="button" onClick={() => updateOverlay(selected, { rotation: (sel.rotation + 345) % 360 })} aria-label="Rotate left">
                    ⟲
                  </button>
                  <button type="button" onClick={() => updateOverlay(selected, { rotation: (sel.rotation + 15) % 360 })} aria-label="Rotate right">
                    ⟳
                  </button>
                  <button
                    type="button"
                    aria-label={`Motion: ${ANIM_LABELS[sel.anim ?? "none"]}. Change motion`}
                    onClick={() => {
                      const next = STORY_ANIMATIONS[(STORY_ANIMATIONS.indexOf(sel.anim ?? "none") + 1) % STORY_ANIMATIONS.length];
                      updateOverlay(selected, { anim: next === "none" ? undefined : next });
                    }}
                  >
                    {ANIM_LABELS[sel.anim ?? "none"]}
                  </button>
                  {sel.type === "text" && (
                    <button type="button" onClick={() => setTextEdit({ index: selected, text: sel.text, color: sel.color, font: sel.font, boxed: sel.boxed })}>
                      Edit
                    </button>
                  )}
                  <button type="button" onClick={() => removeOverlay(selected)} aria-label="Remove">
                    <TrashIcon size={16} />
                  </button>
                </div>
              )}
              {selSticker && selectedSticker !== null && !stickerEdit && (
                <div className={styles.overlayTools} aria-label="Selected sticker">
                  <button type="button" onClick={() => updateSticker(selectedSticker, { scale: Math.max(0.6, selSticker.scale / 1.15) })} aria-label="Smaller sticker">
                    A−
                  </button>
                  <button type="button" onClick={() => updateSticker(selectedSticker, { scale: Math.min(2.5, selSticker.scale * 1.15) })} aria-label="Bigger sticker">
                    A+
                  </button>
                  <button type="button" onClick={() => updateSticker(selectedSticker, { rotation: (selSticker.rotation + 351) % 360 })} aria-label="Rotate sticker left">
                    ⟲
                  </button>
                  <button type="button" onClick={() => updateSticker(selectedSticker, { rotation: (selSticker.rotation + 9) % 360 })} aria-label="Rotate sticker right">
                    ⟳
                  </button>
                  <button type="button" onClick={() => setStickerEdit({ type: selSticker.type, index: selectedSticker })}>
                    Edit
                  </button>
                  <button type="button" onClick={() => removeSticker(selectedSticker)} aria-label="Remove sticker">
                    <TrashIcon size={16} />
                  </button>
                </div>
              )}
            </div>

            {textEdit && (
              <div className={styles.textEditor} role="group" aria-label="Text">
                <textarea
                  autoFocus
                  className={`${styles.textEditorInput} ${styles[`font_${textEdit.font}`]}`}
                  style={textEdit.boxed ? { background: storyColor(textEdit.color) } : { color: storyColor(textEdit.color) }}
                  aria-label="Text to add"
                  maxLength={MAX_OVERLAY_TEXT}
                  value={textEdit.text}
                  onChange={(e) => setTextEdit({ ...textEdit, text: e.target.value })}
                />
                <div className={styles.swatches} role="radiogroup" aria-label="Colour">
                  {STORY_COLORS.map((c) => (
                    <button
                      key={c}
                      type="button"
                      role="radio"
                      aria-checked={textEdit.color === c}
                      aria-label={c}
                      className={`${styles.swatch} ${textEdit.color === c ? styles.swatchOn : ""}`}
                      style={{ background: storyColor(c) }}
                      onClick={() => setTextEdit({ ...textEdit, color: c })}
                    />
                  ))}
                </div>
                <div className={styles.textEditorRow}>
                  <button type="button" className={styles.sheetButton} onClick={() => setTextEdit({ ...textEdit, font: STORY_FONTS[(STORY_FONTS.indexOf(textEdit.font) + 1) % STORY_FONTS.length] })}>
                    {FONT_LABELS[textEdit.font]}
                  </button>
                  <button type="button" className={styles.sheetButton} aria-pressed={textEdit.boxed} onClick={() => setTextEdit({ ...textEdit, boxed: !textEdit.boxed })}>
                    {textEdit.boxed ? "Box on" : "Box off"}
                  </button>
                  <span className={styles.headerSpacer} />
                  <button type="button" className={styles.sheetButtonPrimary} onClick={saveText}>
                    Done
                  </button>
                </div>
              </div>
            )}

            {stickerSheet && (
              <div className={styles.sheet} role="dialog" aria-modal="true" aria-label="Add a sticker">
                <div className={styles.sheetHead}>
                  <strong>Stickers</strong>
                  <button type="button" className={styles.chromeButton} aria-label="Close stickers" onClick={() => setStickerSheet(false)}>
                    <CloseIcon size={18} />
                  </button>
                </div>
                {!interactiveOk && <p className={styles.sheetNote}>Polls, quizzes, questions and sliders aren't available on this server.</p>}
                {stickers.length >= MAX_STICKERS && <p className={styles.sheetError}>At most {MAX_STICKERS} stickers on one story.</p>}
                <ul className={styles.stickerChoices}>
                  {STICKER_CHOICES.filter((c) => interactiveOk || !c.interactive).map((c) => (
                    <li key={c.type}>
                      <button
                        type="button"
                        className={styles.stickerChoice}
                        disabled={stickers.length >= MAX_STICKERS}
                        onClick={() => {
                          setStickerSheet(false);
                          setStickerEdit({ type: c.type, index: null });
                        }}
                      >
                        <strong>{c.label}</strong>
                        <span>{c.hint}</span>
                      </button>
                    </li>
                  ))}
                </ul>
              </div>
            )}

            {stickerEdit && (
              <StickerEditor
                key={`${stickerEdit.type}-${stickerEdit.index}`}
                type={stickerEdit.type}
                initial={stickerEdit.index !== null ? stickers[stickerEdit.index] : undefined}
                friends={stories.friends}
                onSave={saveSticker}
                onCancel={() => setStickerEdit(null)}
              />
            )}

            {peopleOpen && (
              <div className={styles.sheet} role="dialog" aria-modal="true" aria-label="Choose who sees this story">
                <div className={styles.sheetHead}>
                  <strong>Choose people</strong>
                  <button type="button" className={styles.chromeButton} aria-label="Done choosing" onClick={() => setPeopleOpen(false)}>
                    <CloseIcon size={18} />
                  </button>
                </div>
                <p className={styles.sheetNote}>Only these friends get this story's key. Friends you added later won't see it.</p>
                {pickable.length === 0 && <p className={styles.sheetNote}>You have no friends to choose yet.</p>}
                <ul className={styles.sheetList}>
                  {pickable.map((f) => (
                    <li key={f.userId} className={styles.sheetRow}>
                      <label className={styles.sheetGrow}>
                        <input
                          type="checkbox"
                          checked={customIds.includes(f.userId)}
                          onChange={(e) => setCustomIds(e.target.checked ? [...customIds, f.userId] : customIds.filter((x) => x !== f.userId))}
                        />{" "}
                        {f.handle}
                      </label>
                    </li>
                  ))}
                </ul>
                <div className={styles.textEditorRow}>
                  <button type="button" className={styles.sheetButton} onClick={() => setCustomIds(pickable.map((f) => f.userId))}>
                    Everyone
                  </button>
                  <button type="button" className={styles.sheetButton} onClick={() => setCustomIds([])}>
                    No one
                  </button>
                  <span className={styles.headerSpacer} />
                  <button type="button" className={styles.sheetButtonPrimary} onClick={() => setPeopleOpen(false)}>
                    Done
                  </button>
                </div>
              </div>
            )}

            {closePrompt && (
              <div className={styles.sheet} role="alertdialog" aria-modal="true" aria-label="Leave this story?">
                <strong>Leave this story?</strong>
                <p className={styles.sheetNote}>A draft stays on this device, encrypted, and is never sent to RedVault.</p>
                <div className={styles.textEditorRow}>
                  <button type="button" className={styles.sheetButton} onClick={() => setClosePrompt(false)}>
                    Keep editing
                  </button>
                  <button type="button" className={styles.sheetButton} onClick={() => stories.closeComposer()}>
                    Discard
                  </button>
                  <span className={styles.headerSpacer} />
                  <button
                    type="button"
                    className={styles.sheetButtonPrimary}
                    onClick={async () => {
                      if (await keepDraft()) stories.closeComposer();
                      else setClosePrompt(false);
                    }}
                  >
                    Save draft
                  </button>
                </div>
              </div>
            )}

            <div className={styles.bottom}>
              {error && (
                <p className={styles.sheetError} role="alert">
                  {error}
                </p>
              )}
              {notice && !error && (
                <p className={styles.pickNote} role="status">
                  {notice}
                </p>
              )}
              {strip}
              <div className={styles.shareBar}>
                <div className={styles.audience} role="radiogroup" aria-label="Who can see it">
                  <button type="button" role="radio" aria-checked={audience === "friends"} className={audience === "friends" ? styles.audienceOn : ""} onClick={() => setAudience("friends")}>
                    Your story · {everyoneCount}
                  </button>
                  <button
                    type="button"
                    role="radio"
                    aria-checked={audience === "close_friends"}
                    className={`${audience === "close_friends" ? styles.audienceClose : ""}`}
                    disabled={closeCount === 0}
                    title={closeCount === 0 ? "Choose close friends in Story settings" : undefined}
                    onClick={() => setAudience("close_friends")}
                  >
                    <StarIcon size={12} /> Close friends · {closeCount}
                  </button>
                  <button
                    type="button"
                    role="radio"
                    aria-checked={audience === "custom"}
                    className={audience === "custom" ? styles.audienceOn : ""}
                    disabled={pickable.length === 0}
                    onClick={() => {
                      setAudience("custom");
                      setPeopleOpen(true);
                    }}
                  >
                    Choose people{audience === "custom" ? ` · ${customCount}` : ""}
                  </button>
                </div>
                <label className={styles.replyToggle}>
                  <input type="checkbox" checked={allowReplies} onChange={(e) => setAllowReplies(e.target.checked)} />
                  Replies
                </label>
                <label className={styles.replyToggle}>
                  Disappears after
                  <select className={styles.ttlSelect} value={ttlSeconds} aria-label="Disappears after" onChange={(e) => setTtlSeconds(Number(e.target.value))}>
                    {ttlChoices.map((s) => (
                      <option key={s} value={s}>
                        {ttlLabel(s)}
                      </option>
                    ))}
                  </select>
                </label>
                <button
                  type="button"
                  className={styles.shareButton}
                  disabled={busy || (mode === "text" && !text.trim()) || (audience === "custom" && customCount === 0)}
                  onClick={share}
                >
                  {stage === "encrypting" ? "Encrypting…" : stage === "uploading" ? "Sharing…" : slideCount > 1 ? `Share ${slideCount} slides` : "Share"}
                </button>
              </div>
              {progress && busy && (
                <p className={styles.pickNote} role="status">
                  {progress}
                </p>
              )}
              <p className={styles.pickNote} data-testid="story-summary">
                {audience === "custom" ? `Only the ${customCount} ${customCount === 1 ? "friend" : "friends"} you chose` : `${reach} ${reach === 1 ? "friend" : "friends"}`} can see this · gone after {ttlLabel(ttlSeconds)} · end-to-end
                encrypted. Anyone who sees it can screenshot it.
              </p>
              {closeCount === 0 && (
                <button
                  type="button"
                  className={styles.linkButton}
                  onClick={() => {
                    stories.closeComposer();
                    dispatch({ type: "OPEN_SETTINGS", section: "stories" });
                  }}
                >
                  Choose close friends
                </button>
              )}
              {media?.kind === "video" && <p className={styles.pickNote}>Videos are shared as they are, including any details your camera saved in the file.</p>}
            </div>
          </>
        )}
        {mode === "pick" && (
          <div className={styles.top}>
            <div className={styles.header}>
              <span className={styles.headerSpacer} />
              <button type="button" className={styles.chromeButton} aria-label="Close" onClick={close}>
                <CloseIcon size={20} />
              </button>
            </div>
          </div>
        )}

        {studio === "camera" && (
          <CameraCapture onPhoto={acceptMedia} onVideo={acceptMedia} onClose={() => setStudio(null)} onChooseFile={() => { setStudio(null); fileInput.current?.click(); }} />
        )}
        {studio === "editor" && media?.kind === "image" && (
          <PhotoEditor
            blob={media.blob}
            onCancel={() => setStudio(null)}
            onDone={(edited) => {
              setMedia(edited);
              swapPreview(edited);
              setStudio(null);
            }}
          />
        )}
        {studio === "collage" && <CollageMaker onCancel={() => setStudio(null)} onDone={acceptMedia} />}
        {(studio === "voice" || studio === "audiofile") && (
          <AudioStudio source={mode === "audio" ? media?.blob : undefined} start={studio === "audiofile" ? "file" : undefined} onCancel={() => setStudio(null)} onDone={acceptAudio} />
        )}
        {studio === "captions" && canCaption && (
          <CaptionEditor captions={captions} durationMs={media?.durationMs ?? 1000} onChange={setCaptions} onClose={() => setStudio(null)} />
        )}
      </div>
    </div>
  );
}
