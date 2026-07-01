export function createFpsMeter({ sampleMs = 500 } = {}) {
  let windowStart = 0;
  let frames = 0;
  let fps = 0;

  return {
    tick(nowMs) {
      const now = Number(nowMs);
      if (!Number.isFinite(now)) return fps;
      if (!windowStart) {
        windowStart = now;
        frames = 0;
        return fps;
      }
      frames += 1;
      const elapsed = now - windowStart;
      if (elapsed >= sampleMs) {
        fps = Math.round((frames * 1000) / elapsed);
        windowStart = now;
        frames = 0;
      }
      return fps;
    },
    value() {
      return fps;
    },
  };
}
