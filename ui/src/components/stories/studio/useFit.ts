import { useEffect, useState, type RefObject } from "react";

/**
 * The largest box with this width-to-height ratio that fits inside `ref`'s element (minus a little margin),
 * re-measured when it resizes. The editors size their canvas with it so the picture fills the box exactly,
 * which is what lets crop handles, drawn over the box, line up with the picture.
 */
export function useFit(ref: RefObject<HTMLElement | null>, aspect: number): { w: number; h: number } {
  const [space, setSpace] = useState({ w: 0, h: 0 });
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const measure = () => {
      const r = el.getBoundingClientRect();
      setSpace({ w: Math.max(0, r.width - 12), h: Math.max(0, r.height - 12) });
    };
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref]);
  const a = aspect > 0 && Number.isFinite(aspect) ? aspect : 1;
  const w = Math.min(space.w, space.h * a);
  return { w: Math.floor(w), h: Math.floor(w / a) };
}
