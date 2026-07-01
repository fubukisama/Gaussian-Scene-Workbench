import test from "node:test";
import assert from "node:assert/strict";

import {
  isEditableShortcutTarget,
  selectionModeStatus,
  shouldStartSelectionDrag,
  temporaryNavigationStatus,
} from "../static/interaction.mjs";

test("trim modes keep left drag for selection", () => {
  assert.equal(shouldStartSelectionDrag({
    mode: "brush",
    hasPositions: true,
    button: 0,
    isSpaceDown: false,
    altKey: false,
    ctrlKey: false,
    metaKey: false,
  }), true);
});

test("trim modes allow temporary viewport navigation", () => {
  for (const override of [
    { isSpaceDown: true },
    { altKey: true },
    { button: 1 },
    { button: 2 },
  ]) {
    assert.equal(shouldStartSelectionDrag({
      mode: "lasso",
      hasPositions: true,
      button: 0,
      isSpaceDown: false,
      altKey: false,
      ctrlKey: false,
      metaKey: false,
      ...override,
    }), false);
  }
});

test("trim status tells users how to move the viewport", () => {
  assert.match(selectionModeStatus("rect"), /Space\+drag/);
  assert.match(selectionModeStatus("brush"), /Alt\+drag/);
  assert.match(temporaryNavigationStatus("lasso"), /Temporary Navigate/);
});

test("viewport shortcuts ignore text fields but not checkbox controls", () => {
  assert.equal(isEditableShortcutTarget({ tagName: "INPUT", type: "number" }), true);
  assert.equal(isEditableShortcutTarget({ tagName: "INPUT", type: "text" }), true);
  assert.equal(isEditableShortcutTarget({ tagName: "TEXTAREA" }), true);
  assert.equal(isEditableShortcutTarget({ tagName: "SELECT" }), true);
  assert.equal(isEditableShortcutTarget({ tagName: "INPUT", type: "checkbox" }), false);
  assert.equal(isEditableShortcutTarget({ tagName: "BUTTON" }), false);
});
