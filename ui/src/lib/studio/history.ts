// Undo and redo for the studio's editors: a plain value at each step, nothing mutated in place.

export interface History<T> {
  past: T[];
  present: T;
  future: T[];
}

export const MAX_UNDO = 60;

export function startHistory<T>(value: T): History<T> {
  return { past: [], present: value, future: [] };
}

/** A new step. Anything that could have been redone is gone; the oldest steps drop off past MAX_UNDO. */
export function pushHistory<T>(h: History<T>, value: T, limit = MAX_UNDO): History<T> {
  if (Object.is(h.present, value)) return h;
  const past = [...h.past, h.present];
  while (past.length > limit) past.shift();
  return { past, present: value, future: [] };
}

export const canUndo = <T>(h: History<T>): boolean => h.past.length > 0;
export const canRedo = <T>(h: History<T>): boolean => h.future.length > 0;

export function undo<T>(h: History<T>): History<T> {
  if (!canUndo(h)) return h;
  const past = h.past.slice(0, -1);
  return { past, present: h.past[h.past.length - 1], future: [h.present, ...h.future] };
}

export function redo<T>(h: History<T>): History<T> {
  if (!canRedo(h)) return h;
  const [next, ...future] = h.future;
  return { past: [...h.past, h.present], present: next, future };
}
