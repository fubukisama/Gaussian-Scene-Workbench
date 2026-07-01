const DEFAULT_MARGIN = 8;

function finiteNumber(value, fallback) {
  return Number.isFinite(value) ? value : fallback;
}

export function clampPanelPosition(position) {
  const margin = finiteNumber(position.margin, DEFAULT_MARGIN);
  const fallbackWidth = globalThis.window?.innerWidth ?? 0;
  const fallbackHeight = globalThis.window?.innerHeight ?? 0;
  const viewportWidth = Math.max(0, finiteNumber(position.viewportWidth, fallbackWidth));
  const viewportHeight = Math.max(0, finiteNumber(position.viewportHeight, fallbackHeight));
  const width = Math.max(0, finiteNumber(position.width, 0));
  const height = Math.max(0, finiteNumber(position.height, 0));
  const minLeft = margin;
  const minTop = margin;
  const maxLeft = Math.max(minLeft, viewportWidth - width - margin);
  const maxTop = Math.max(minTop, viewportHeight - height - margin);
  return {
    left: Math.min(maxLeft, Math.max(minLeft, finiteNumber(position.left, minLeft))),
    top: Math.min(maxTop, Math.max(minTop, finiteNumber(position.top, minTop))),
  };
}

function isInteractiveTarget(target) {
  return Boolean(target?.closest?.("button, input, select, textarea, a, label"));
}

function readStoredPosition(storageKey) {
  if (!storageKey) return null;
  try {
    const parsed = JSON.parse(localStorage.getItem(storageKey) || "null");
    if (!parsed || !Number.isFinite(parsed.left) || !Number.isFinite(parsed.top)) return null;
    return parsed;
  } catch (_) {
    return null;
  }
}

function writeStoredPosition(storageKey, position) {
  if (!storageKey) return;
  try {
    localStorage.setItem(storageKey, JSON.stringify(position));
  } catch (_) {
    // Position persistence is optional.
  }
}

function applyPanelPosition(panel, position) {
  panel.style.left = `${position.left}px`;
  panel.style.top = `${position.top}px`;
  panel.style.right = "auto";
  panel.style.bottom = "auto";
}

export function makePanelDraggable(panel, handle, options = {}) {
  if (!panel || !handle) return;
  const margin = options.margin ?? DEFAULT_MARGIN;
  const storageKey = options.storageKey || "";
  const stored = readStoredPosition(storageKey);
  if (stored) {
    const rect = panel.getBoundingClientRect();
    applyPanelPosition(panel, clampPanelPosition({
      left: stored.left,
      top: stored.top,
      width: rect.width,
      height: rect.height,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    }));
  }

  let drag = null;

  function panelPositionFromPointer(event) {
    const position = clampPanelPosition({
      left: event.clientX - drag.offsetX,
      top: event.clientY - drag.offsetY,
      width: drag.width,
      height: drag.height,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    });
    applyPanelPosition(panel, position);
    return position;
  }

  handle.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || isInteractiveTarget(event.target)) return;
    const rect = panel.getBoundingClientRect();
    drag = {
      offsetX: event.clientX - rect.left,
      offsetY: event.clientY - rect.top,
      width: rect.width,
      height: rect.height,
    };
    applyPanelPosition(panel, clampPanelPosition({
      left: rect.left,
      top: rect.top,
      width: rect.width,
      height: rect.height,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    }));
    handle.setPointerCapture?.(event.pointerId);
    document.body.classList.add("dragging-panel");
    event.preventDefault();
  });

  handle.addEventListener("pointermove", (event) => {
    if (!drag) return;
    panelPositionFromPointer(event);
  });

  function finishDrag(event) {
    if (!drag) return;
    const position = panelPositionFromPointer(event);
    writeStoredPosition(storageKey, position);
    drag = null;
    document.body.classList.remove("dragging-panel");
  }

  handle.addEventListener("pointerup", finishDrag);
  handle.addEventListener("pointercancel", finishDrag);

  window.addEventListener("resize", () => {
    const rect = panel.getBoundingClientRect();
    const position = clampPanelPosition({
      left: rect.left,
      top: rect.top,
      width: rect.width,
      height: rect.height,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    });
    applyPanelPosition(panel, position);
    writeStoredPosition(storageKey, position);
  });
}
