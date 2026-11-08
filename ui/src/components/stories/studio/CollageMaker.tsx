// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { drawCollage, layoutsFor, MAX_COLLAGE_PHOTOS, renderCollage } from "../../../lib/studio/collage.js";
import type { PreparedMedia } from "../../../lib/stories.js";
import { CloseIcon } from "../../icons24.js";
import { useFit } from "./useFit.js";
import styles from "../Stories.module.css";

interface Item {
  id: number;
  bitmap: ImageBitmap;
  name: string;
}

const BACKGROUNDS = [
  { label: "Dark", value: "#111111" },
  { label: "Light", value: "#f5f5f4" },
  { label: "Warm", value: "#7c2d12" },
  { label: "Cool", value: "#1e3a8a" },
];

interface Props {
  onDone: (media: PreparedMedia) => void;
  onCancel: () => void;
}

let nextId = 1;

/** Two to four photos in one picture, laid out and drawn on this device. */
export function CollageMaker({ onDone, onCancel }: Props) {
  const [items, setItems] = useState<Item[]>([]);
  const [layoutId, setLayoutId] = useState<string | null>(null);
  const [gap, setGap] = useState(12);
  const [background, setBackground] = useState(BACKGROUNDS[0].value);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const stageRef = useRef<HTMLDivElement>(null);
  const fileRef = useRef<HTMLInputElement>(null);
  const fit = useFit(stageRef, 9 / 16);

  const layouts = layoutsFor(items.length);
  const layout = layouts.find((l) => l.id === layoutId) ?? layouts[0] ?? null;

  useEffect(
    () => () => {
      items.forEach((i) => i.bitmap.close());
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    []
  );

  useEffect(() => {
    const target = canvasRef.current;
    if (!target || !layout || items.length !== layout.cells.length) return;
    const drawn = drawCollage(items.map((i) => i.bitmap), layout, { width: 360, height: 640, gap: gap / 3, background });
    target.width = drawn.width;
    target.height = drawn.height;
    target.getContext("2d")!.drawImage(drawn, 0, 0);
  });

  async function add(files: FileList | null) {
    if (!files) return;
    setError(null);
    const room = MAX_COLLAGE_PHOTOS - items.length;
    const added: Item[] = [];
    for (const file of Array.from(files).slice(0, room)) {
      if (!file.type.startsWith("image/")) continue;
      try {
        added.push({ id: nextId++, bitmap: await createImageBitmap(file, { imageOrientation: "from-image" }), name: file.name });
      } catch {
        setError(`"${file.name}" couldn't be opened.`);
      }
    }
    if (files.length > room) setError(`A collage holds at most ${MAX_COLLAGE_PHOTOS} photos.`);
    setItems((list) => [...list, ...added]);
    setLayoutId(null);
  }

  const move = (index: number, by: number) =>
    setItems((list) => {
      const to = index + by;
      if (to < 0 || to >= list.length) return list;
      const copy = [...list];
      [copy[index], copy[to]] = [copy[to], copy[index]];
      return copy;
    });

  const remove = (index: number) => {
    setItems((list) => {
      list[index].bitmap.close();
      return list.filter((_, i) => i !== index);
    });
    setLayoutId(null);
  };

  async function finish() {
    if (!layout || items.length !== layout.cells.length) return;
    setBusy(true);
    try {
      const out = await renderCollage(items.map((i) => i.bitmap), layout, { gap, background });
      onDone({ kind: "image", blob: out.blob, mime: "image/jpeg", width: out.width, height: out.height });
    } catch {
      setError("That collage couldn't be made.");
      setBusy(false);
    }
  }

  return (
    <div className={styles.editor} role="group" aria-label="Make a collage">
      <div className={styles.editorTop}>
        <button type="button" className={styles.chromeButton} aria-label="Cancel the collage" onClick={onCancel}>
          <CloseIcon size={20} />
        </button>
        <strong className={styles.editorTitle}>Collage</strong>
        <span className={styles.headerSpacer} />
        <button type="button" className={styles.sheetButtonPrimary} disabled={busy || !layout || items.length < 2} onClick={finish}>
          {busy ? "Making…" : "Done"}
        </button>
      </div>
      <div className={styles.editorStage} ref={stageRef}>
        {items.length < 2 ? (
          <p className={styles.studioMessage}>Choose two to four photos. They stay on this device.</p>
        ) : (
          <canvas ref={canvasRef} className={styles.editorCanvas} style={{ width: fit.w || undefined, height: fit.h || undefined }} aria-label="Collage preview" />
        )}
      </div>
      <div className={styles.editorTools}>
        {error && (
          <p className={styles.sheetError} role="alert">
            {error}
          </p>
        )}
        <ul className={styles.collageList} aria-label="Photos">
          {items.map((item, i) => (
            <li key={item.id} className={styles.sheetRow}>
              <span className={styles.sheetGrow}>
                {i + 1}. {item.name}
              </span>
              <button type="button" className={styles.sheetButton} aria-label={`Move ${item.name} earlier`} disabled={i === 0} onClick={() => move(i, -1)}>
                ↑
              </button>
              <button type="button" className={styles.sheetButton} aria-label={`Move ${item.name} later`} disabled={i === items.length - 1} onClick={() => move(i, 1)}>
                ↓
              </button>
              <button type="button" className={styles.sheetButton} aria-label={`Remove ${item.name}`} onClick={() => remove(i)}>
                Remove
              </button>
            </li>
          ))}
        </ul>
        {items.length < MAX_COLLAGE_PHOTOS && (
          <button type="button" className={styles.sheetButton} onClick={() => fileRef.current?.click()}>
            {items.length === 0 ? "Choose photos" : "Add a photo"}
          </button>
        )}
        <input ref={fileRef} type="file" accept="image/jpeg,image/png,image/webp,image/heic" multiple hidden onChange={(e) => { void add(e.target.files); e.target.value = ""; }} />
        {layouts.length > 0 && (
          <div className={styles.editorRow} role="radiogroup" aria-label="Layout">
            {layouts.map((l) => (
              <button key={l.id} type="button" role="radio" aria-checked={layout?.id === l.id} className={`${styles.sheetButton} ${layout?.id === l.id ? styles.editorPicked : ""}`} onClick={() => setLayoutId(l.id)}>
                {l.label}
              </button>
            ))}
          </div>
        )}
        {items.length >= 2 && (
          <>
            <label className={styles.sliderRow}>
              <span>Space between</span>
              <input type="range" min={0} max={40} value={gap} aria-label="Space between photos" onChange={(e) => setGap(Number(e.target.value))} />
            </label>
            <div className={styles.editorRow} role="radiogroup" aria-label="Background">
              {BACKGROUNDS.map((b) => (
                <button key={b.value} type="button" role="radio" aria-checked={background === b.value} className={`${styles.sheetButton} ${background === b.value ? styles.editorPicked : ""}`} onClick={() => setBackground(b.value)}>
                  {b.label}
                </button>
              ))}
            </div>
          </>
        )}
      </div>
    </div>
  );
}
