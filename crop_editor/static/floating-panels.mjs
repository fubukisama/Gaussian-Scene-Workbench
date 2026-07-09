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

export function clampPanelSize(size) {
  const margin = finiteNumber(size.margin, DEFAULT_MARGIN);
  const fallbackWidth = globalThis.window?.innerWidth ?? 0;
  const fallbackHeight = globalThis.window?.innerHeight ?? 0;
  const viewportWidth = Math.max(0, finiteNumber(size.viewportWidth, fallbackWidth));
  const viewportHeight = Math.max(0, finiteNumber(size.viewportHeight, fallbackHeight));
  const minWidth = Math.max(1, finiteNumber(size.minWidth, 280));
  const minHeight = Math.max(1, finiteNumber(size.minHeight, 180));
  const maxWidth = Math.max(minWidth, Math.min(finiteNumber(size.maxWidth, viewportWidth), Math.max(minWidth, viewportWidth - margin * 2)));
  const maxHeight = Math.max(minHeight, Math.min(finiteNumber(size.maxHeight, viewportHeight), Math.max(minHeight, viewportHeight - margin * 2)));
  return {
    width: Math.min(maxWidth, Math.max(minWidth, finiteNumber(size.width, minWidth))),
    height: Math.min(maxHeight, Math.max(minHeight, finiteNumber(size.height, minHeight))),
  };
}

function isInteractiveTarget(target) {
  return Boolean(target?.closest?.("button, input, select, textarea, a, label"));
}

function readStoredState(storageKey) {
  if (!storageKey) return null;
  try {
    const parsed = JSON.parse(localStorage.getItem(storageKey) || "null");
    if (!parsed || !Number.isFinite(parsed.left) || !Number.isFinite(parsed.top)) return null;
    return parsed;
  } catch (_) {
    return null;
  }
}

function writeStoredState(storageKey, state) {
  if (!storageKey) return;
  try {
    localStorage.setItem(storageKey, JSON.stringify(state));
  } catch (_) {
    // Window persistence is optional.
  }
}

function applyPanelPosition(panel, position) {
  panel.style.left = `${position.left}px`;
  panel.style.top = `${position.top}px`;
  panel.style.right = "auto";
  panel.style.bottom = "auto";
}

function applyPanelSize(panel, size) {
  panel.style.width = `${size.width}px`;
  panel.style.height = `${size.height}px`;
  panel.style.maxWidth = `calc(100vw - ${DEFAULT_MARGIN * 2}px)`;
  panel.style.maxHeight = `calc(100vh - ${DEFAULT_MARGIN * 2}px)`;
}

function panelState(panel) {
  const rect = panel.getBoundingClientRect();
  return { left: rect.left, top: rect.top, width: rect.width, height: rect.height };
}

export function makePanelDraggable(panel, handle, options = {}) {
  if (!panel || !handle) return;
  const margin = options.margin ?? DEFAULT_MARGIN;
  const storageKey = options.storageKey || "";
  const stored = readStoredState(storageKey);
  if (stored) {
    const rect = panel.getBoundingClientRect();
    if (Number.isFinite(stored.width) && Number.isFinite(stored.height)) {
      applyPanelSize(panel, clampPanelSize({
        width: stored.width,
        height: stored.height,
        minWidth: options.minWidth,
        minHeight: options.minHeight,
        maxWidth: options.maxWidth,
        maxHeight: options.maxHeight,
        viewportWidth: window.innerWidth,
        viewportHeight: window.innerHeight,
        margin,
      }));
    }
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
  let resizeObserver = null;
  let resizeWriteTimer = null;

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
    writeStoredState(storageKey, { ...panelState(panel), ...position });
    drag = null;
    document.body.classList.remove("dragging-panel");
  }

  handle.addEventListener("pointerup", finishDrag);
  handle.addEventListener("pointercancel", finishDrag);

  if (storageKey && "ResizeObserver" in window) {
    resizeObserver = new ResizeObserver(() => {
      if (drag) return;
      if (resizeWriteTimer) clearTimeout(resizeWriteTimer);
      resizeWriteTimer = setTimeout(() => {
        writeStoredState(storageKey, panelState(panel));
      }, 120);
    });
    resizeObserver.observe(panel);
  }

  window.addEventListener("resize", () => {
    const rect = panel.getBoundingClientRect();
    const size = clampPanelSize({
      width: rect.width,
      height: rect.height,
      minWidth: options.minWidth,
      minHeight: options.minHeight,
      maxWidth: options.maxWidth,
      maxHeight: options.maxHeight,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    });
    applyPanelSize(panel, size);
    const position = clampPanelPosition({
      left: rect.left,
      top: rect.top,
      width: size.width,
      height: size.height,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      margin,
    });
    applyPanelPosition(panel, position);
    writeStoredState(storageKey, { ...position, ...size });
  });
}
