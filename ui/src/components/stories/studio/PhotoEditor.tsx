import { useCallback, useEffect, useRef, useState } from "react";
import {
  addStroke,
  aspectCrop,
  clamp,
  dragRect,
  drawEdit,
  extendStroke,
  FILTER_LABELS,
  FILTERS,
  hasEdits,
  MAX_PEN,
  MIN_PEN,
  NEUTRAL_EDIT,
  newStroke,
  outputSize,
  PEN_COLORS,
  renderEdit,
  rotateBy,
  rotatedSize,
  withGeometry,
  type Adjust,
  type EditState,
  type FilterId,
  type Handle,
  type Rect,
  type Stroke,
} from "../../../lib/studio/imageEdit.js";
import { canRedo, canUndo, pushHistory, redo, startHistory, undo, type History } from "../../../lib/studio/history.js";
import { MAX_IMAGE_SIDE, type PreparedMedia } from "../../../lib/stories.js";
import { STORY_COLORS, type StoryColor } from "../../../lib/storyCrypto.js";
import { storyColor } from "../StoryFrame.js";
import { useFit } from "./useFit.js";
import { CloseIcon } from "../../icons24.js";
import styles from "../Stories.module.css";

const PREVIEW_SIDE = 900;
const ASPECTS: { label: string; value: number | null | "original" }[] = [
  { label: "Free", value: "original" },
  { label: "1:1", value: 1 },
  { label: "4:5", value: 4 / 5 },
  { label: "9:16", value: 9 / 16 },
  { label: "16:9", value: 16 / 9 },
];

type Tab = "crop" | "adjust" | "filters" | "draw";

interface Props {
  /** The photo to edit (any image the browser can open). */
  blob: Blob;
  onDone: (media: PreparedMedia) => void;
  onCancel: () => void;
}

const HANDLES: { id: Handle; className: string; label: string }[] = [
  { id: "nw", className: "handleNw", label: "Top left corner" },
  { id: "ne", className: "handleNe", label: "Top right corner" },
  { id: "sw", className: "handleSw", label: "Bottom left corner" },
  { id: "se", className: "handleSe", label: "Bottom right corner" },
];

/** Edits a photo on this device: nothing is uploaded, and the result is a fresh JPEG with no metadata. */
export function PhotoEditor({ blob, onDone, onCancel }: Props) {
  const [bitmap, setBitmap] = useState<ImageBitmap | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [history, setHistory] = useState<History<EditState>>(() => startHistory(NEUTRAL_EDIT));
  const [tab, setTab] = useState<Tab>("crop");
  const [color, setColor] = useState<StoryColor>("white");
  const [penSize, setPenSize] = useState(0.012);
  const [eraser, setEraser] = useState(false);
  const [stroke, setStroke] = useState<Stroke | null>(null);
  const [cropLive, setCropLive] = useState<Rect | null>(null);
  const [busy, setBusy] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const stageRef = useRef<HTMLDivElement>(null);
  const lastGroup = useRef<{ id: string; at: number }>({ id: "", at: 0 });
  const dragging = useRef<{ handle: Handle; x: number; y: number; rect: Rect } | null>(null);

  const state = history.present;

  useEffect(() => {
    let alive = true;
    createImageBitmap(blob, { imageOrientation: "from-image" }).then(
      (b) => (alive ? setBitmap(b) : b.close()),
      () => alive && setError("That photo couldn't be opened.")
    );
    return () => {
      alive = false;
    };
  }, [blob]);
  useEffect(() => () => bitmap?.close(), [bitmap]);

  // In the crop tab the picture is shown whole (turned, with colour) and the crop is an outline on it.
  const shown: EditState = tab === "crop" ? { ...state, crop: null, strokes: [] } : state;
  const inProgress: EditState = stroke && tab === "draw" ? { ...shown, strokes: [...shown.strokes, stroke] } : shown;

  useEffect(() => {
    const target = canvasRef.current;
    if (!bitmap || !target) return;
    const drawn = drawEdit(bitmap, inProgress, PREVIEW_SIDE);
    target.width = drawn.width;
    target.height = drawn.height;
    target.getContext("2d")!.drawImage(drawn, 0, 0);
  });

  /** A change. Steps of the same kind close together (dragging a slider) count as one undo. */
  const commit = useCallback((next: EditState, group = "") => {
    setHistory((h) => {
      const now = Date.now();
      const same = group !== "" && lastGroup.current.id === group && now - lastGroup.current.at < 700;
      lastGroup.current = { id: group, at: now };
      return same ? { ...h, present: next } : pushHistory(h, next);
    });
  }, []);

  const rot = bitmap ? rotatedSize(bitmap.width, bitmap.height, state.rotation) : { w: 1, h: 1 };
  // The canvas box is exactly the picture's shape, so the crop outline (drawn over the box) lines up with the picture.
  const previewSize = bitmap ? outputSize(bitmap.width, bitmap.height, inProgress, PREVIEW_SIDE) : { w: 1, h: 1 };
  const fit = useFit(stageRef, previewSize.w / previewSize.h);
  const crop = cropLive ?? state.crop ?? { x: 0, y: 0, w: 1, h: 1 };

  // --- Crop dragging (positions are fractions of the picture) ---
  function pointerFraction(e: React.PointerEvent): { x: number; y: number } {
    const rect = canvasRef.current!.getBoundingClientRect();
    return { x: (e.clientX - rect.left) / rect.width, y: (e.clientY - rect.top) / rect.height };
  }
  function startCropDrag(e: React.PointerEvent, handle: Handle) {
    e.stopPropagation();
    (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
    dragging.current = { handle, ...pointerFraction(e), rect: crop };
  }
  function moveCropDrag(e: React.PointerEvent) {
    const d = dragging.current;
    if (!d) return;
    const p = pointerFraction(e);
    setCropLive(dragRect(d.rect, d.handle, p.x - d.x, p.y - d.y));
  }
  function endCropDrag() {
    if (!dragging.current) return;
    dragging.current = null;
    if (cropLive) commit(withGeometry(state, { crop: cropLive }));
    setCropLive(null);
  }

  // --- Drawing ---
  function startDraw(e: React.PointerEvent) {
    if (tab !== "draw") return;
    (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
    setStroke(newStroke(color, penSize, eraser, pointerFraction(e)));
  }
  function moveDraw(e: React.PointerEvent) {
    if (stroke) setStroke(extendStroke(stroke, pointerFraction(e)));
  }
  function endDraw() {
    if (stroke) commit(addStroke(state, stroke));
    setStroke(null);
  }

  const setAdjust = (change: Partial<Adjust>) => commit({ ...state, adjust: { ...state.adjust, ...change } }, `adjust-${Object.keys(change)[0]}`);

  async function finish() {
    if (!bitmap) return;
    setBusy(true);
    try {
      const out = await renderEdit(bitmap, state, MAX_IMAGE_SIDE);
      const size = outputSize(bitmap.width, bitmap.height, state, MAX_IMAGE_SIDE);
      onDone({ kind: "image", blob: out, mime: "image/jpeg", width: size.w, height: size.h });
    } catch {
      setError("That photo couldn't be prepared.");
      setBusy(false);
    }
  }

  if (error) {
    return (
      <div className={styles.studioPanel} role="group" aria-label="Edit photo">
        <p className={styles.studioMessage} role="alert">
          {error}
        </p>
        <button type="button" className={styles.sheetButton} onClick={onCancel}>
          Back
        </button>
      </div>
    );
  }

  return (
    <div className={styles.editor} role="group" aria-label="Edit photo">
      <div className={styles.editorTop}>
        <button type="button" className={styles.chromeButton} aria-label="Cancel editing" onClick={onCancel}>
          <CloseIcon size={20} />
        </button>
        <span className={styles.headerSpacer} />
        <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} disabled={!canUndo(history)} onClick={() => setHistory(undo(history))} aria-label="Undo">
          Undo
        </button>
        <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} disabled={!canRedo(history)} onClick={() => setHistory(redo(history))} aria-label="Redo">
          Redo
        </button>
        <button type="button" className={`${styles.chromeButton} ${styles.chromeText}`} disabled={!hasEdits(state)} onClick={() => commit(NEUTRAL_EDIT)} aria-label="Reset all edits">
          Reset
        </button>
        <button type="button" className={styles.sheetButtonPrimary} disabled={!bitmap || busy} onClick={finish}>
          {busy ? "Saving…" : "Done"}
        </button>
      </div>

      <div className={styles.editorStage} ref={stageRef}>
        {!bitmap && <p className={styles.studioMessage}>Opening the photo…</p>}
        <div className={styles.editorCanvasWrap} style={{ width: fit.w || undefined, height: fit.h || undefined }}>
          <canvas
            ref={canvasRef}
            className={styles.editorCanvas}
            aria-label="Photo preview"
            style={{ touchAction: tab === "draw" ? "none" : undefined, cursor: tab === "draw" ? "crosshair" : undefined }}
            onPointerDown={startDraw}
            onPointerMove={moveDraw}
            onPointerUp={endDraw}
            onPointerCancel={endDraw}
          />
          {tab === "crop" && bitmap && (
            <div className={styles.cropBox} style={{ left: `${crop.x * 100}%`, top: `${crop.y * 100}%`, width: `${crop.w * 100}%`, height: `${crop.h * 100}%` }} onPointerMove={moveCropDrag} onPointerUp={endCropDrag} onPointerCancel={endCropDrag}>
              <div className={styles.cropMove} aria-label="Move the crop" role="button" tabIndex={0} onPointerDown={(e) => startCropDrag(e, "move")} />
              {HANDLES.map((h) => (
                <div key={h.id} role="button" tabIndex={0} aria-label={h.label} className={`${styles.cropHandle} ${styles[h.className]}`} onPointerDown={(e) => startCropDrag(e, h.id)} />
              ))}
            </div>
          )}
        </div>
      </div>

      <div className={styles.editorTabs} role="tablist" aria-label="Editing tools">
        {(["crop", "adjust", "filters", "draw"] as Tab[]).map((t) => (
          <button key={t} type="button" role="tab" aria-selected={tab === t} className={tab === t ? styles.editorTabOn : ""} onClick={() => setTab(t)}>
            {t === "crop" ? "Crop" : t === "adjust" ? "Adjust" : t === "filters" ? "Filters" : "Draw"}
          </button>
        ))}
      </div>

      <div className={styles.editorTools}>
        {tab === "crop" && bitmap && (
          <>
            <div className={styles.editorRow}>
              <button type="button" className={styles.sheetButton} aria-label="Rotate left" onClick={() => commit(withGeometry(state, { rotation: rotateBy(state.rotation, -1), crop: null }))}>
                ⟲ Rotate
              </button>
              <button type="button" className={styles.sheetButton} aria-label="Rotate right" onClick={() => commit(withGeometry(state, { rotation: rotateBy(state.rotation, 1), crop: null }))}>
                Rotate ⟳
              </button>
              <button type="button" className={styles.sheetButton} aria-pressed={state.flipX} onClick={() => commit(withGeometry(state, { flipX: !state.flipX, crop: null }))}>
                Flip
              </button>
            </div>
            <div className={styles.editorRow} role="group" aria-label="Crop shape">
              {ASPECTS.map((a) => (
                <button
                  key={a.label}
                  type="button"
                  className={styles.sheetButton}
                  onClick={() => commit(withGeometry(state, { crop: a.value === "original" ? null : aspectCrop(a.value, rot.w, rot.h) }))}
                >
                  {a.label}
                </button>
              ))}
            </div>
            {state.strokes.length > 0 && <p className={styles.sheetNote}>Changing the crop, turn or flip clears your drawing.</p>}
          </>
        )}
        {tab === "adjust" && (
          <>
            {(
              [
                ["brightness", "Brightness"],
                ["contrast", "Contrast"],
                ["saturation", "Saturation"],
              ] as const
            ).map(([key, label]) => (
              <label key={key} className={styles.sliderRow}>
                <span>{label}</span>
                <input type="range" min={-100} max={100} value={state.adjust[key]} aria-label={label} onChange={(e) => setAdjust({ [key]: Number(e.target.value) })} />
                <output>{state.adjust[key]}</output>
              </label>
            ))}
            <label className={styles.sliderRow}>
              <span>Blur</span>
              <input type="range" min={0} max={100} value={state.adjust.blur} aria-label="Blur" onChange={(e) => setAdjust({ blur: Number(e.target.value) })} />
              <output>{state.adjust.blur}</output>
            </label>
          </>
        )}
        {tab === "filters" && (
          <div className={styles.editorRow} role="radiogroup" aria-label="Filter">
            {FILTERS.map((f: FilterId) => (
              <button key={f} type="button" role="radio" aria-checked={state.filter === f} className={`${styles.sheetButton} ${state.filter === f ? styles.editorPicked : ""}`} onClick={() => commit({ ...state, filter: f })}>
                {FILTER_LABELS[f]}
              </button>
            ))}
          </div>
        )}
        {tab === "draw" && (
          <>
            <div className={styles.swatches} role="radiogroup" aria-label="Pen colour">
              {STORY_COLORS.map((c) => (
                <button key={c} type="button" role="radio" aria-checked={color === c && !eraser} aria-label={c} className={`${styles.swatch} ${color === c && !eraser ? styles.swatchOn : ""}`} style={{ background: storyColor(c), outlineColor: PEN_COLORS[c] }} onClick={() => { setColor(c); setEraser(false); }} />
              ))}
            </div>
            <div className={styles.editorRow}>
              <button type="button" className={styles.sheetButton} aria-pressed={eraser} onClick={() => setEraser(!eraser)}>
                Eraser
              </button>
              <label className={styles.sliderRow}>
                <span>Size</span>
                <input type="range" min={MIN_PEN * 1000} max={MAX_PEN * 1000} value={penSize * 1000} aria-label="Pen size" onChange={(e) => setPenSize(clamp(Number(e.target.value) / 1000, MIN_PEN, MAX_PEN))} />
              </label>
            </div>
            <p className={styles.sheetNote}>Draw on the picture. Undo takes back the last line.</p>
          </>
        )}
      </div>
    </div>
  );
}
