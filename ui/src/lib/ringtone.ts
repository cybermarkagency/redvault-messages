// Ring and ringback tones, synthesized in the page (no audio files to load).
// Best effort: a browser that blocks audio before any interaction stays quiet.

type Tone = "incoming" | "outgoing";

const PATTERN: Record<Tone, { everyMs: number; beeps: number[]; hz: number }> = {
  incoming: { everyMs: 2000, beeps: [0, 0.35], hz: 880 },
  outgoing: { everyMs: 4000, beeps: [0, 0.5], hz: 440 },
};

/** Starts the tone; call the returned function to stop it. */
export function startRingtone(tone: Tone): () => void {
  let ctx: AudioContext | null = null;
  let timer: ReturnType<typeof setInterval> | null = null;
  try {
    const Ctx = window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
    if (!Ctx) return () => {};
    ctx = new Ctx();
    const { everyMs, beeps, hz } = PATTERN[tone];
    const ring = () => {
      if (!ctx) return;
      for (const offset of beeps) {
        const at = ctx.currentTime + offset;
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.frequency.value = hz;
        gain.gain.setValueAtTime(0, at);
        gain.gain.linearRampToValueAtTime(0.12, at + 0.02);
        gain.gain.linearRampToValueAtTime(0, at + 0.3);
        osc.connect(gain).connect(ctx.destination);
        osc.start(at);
        osc.stop(at + 0.32);
      }
    };
    ring();
    timer = setInterval(ring, everyMs);
  } catch {
    // No audio: the on-screen call still works.
  }
  return () => {
    if (timer) clearInterval(timer);
    ctx?.close().catch(() => {});
    ctx = null;
  };
}
