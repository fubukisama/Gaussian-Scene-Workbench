export function shouldStartSelectionDrag({
  mode,
  hasPositions,
  button,
  isSpaceDown,
  altKey,
  ctrlKey,
  metaKey,
}) {
  return mode !== "navigate" &&
    Boolean(hasPositions) &&
    button === 0 &&
    !isSpaceDown &&
    !altKey &&
    !ctrlKey &&
    !metaKey;
}

export function selectionModeStatus(mode) {
  if (mode === "brush") {
    return "Brush mode: left-drag paints selection; Shift+drag removes; Space+drag or Alt+drag navigates.";
  }
  if (mode === "rect") {
    return "Rect mode: left-drag selects; Space+drag, Alt+drag, right/middle drag, or wheel navigates.";
  }
  if (mode === "lasso") {
    return "Lasso mode: left-drag selects; Space+drag, Alt+drag, right/middle drag, or wheel navigates.";
  }
  return "Navigate mode.";
}

export function temporaryNavigationStatus(mode) {
  if (mode === "navigate") return "Navigate mode.";
  return "Temporary Navigate: drag to move the viewport; release Space to continue trimming.";
}

export function isEditableShortcutTarget(target) {
  const tagName = target?.tagName?.toUpperCase?.() || "";
  if (target?.isContentEditable) return true;
  if (tagName === "TEXTAREA" || tagName === "SELECT") return true;
  if (tagName !== "INPUT") return false;
  const type = (target.type || "text").toLowerCase();
  return new Set([
    "date",
    "datetime-local",
    "email",
    "month",
    "number",
    "password",
    "search",
    "tel",
    "text",
    "time",
    "url",
    "week",
  ]).has(type);
}
