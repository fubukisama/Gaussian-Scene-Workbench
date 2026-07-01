import test from "node:test";
import assert from "node:assert/strict";

import { createTrainingFileStore } from "../static/training-files.mjs";

function fakeFile(name, options = {}) {
  return {
    name,
    size: options.size ?? 100,
    lastModified: options.lastModified ?? 1,
    webkitRelativePath: options.webkitRelativePath || "",
  };
}

test("training file store appends files across multiple selections", () => {
  const store = createTrainingFileStore([fakeFile("a.jpg")]);

  assert.equal(store.add([fakeFile("b.jpg"), fakeFile("c.mp4")]), 2);

  assert.deepEqual(store.list().map((file) => file.name), ["a.jpg", "b.jpg", "c.mp4"]);
});

test("training file store ignores duplicate selected files", () => {
  const store = createTrainingFileStore();
  const first = fakeFile("a.jpg", { size: 123, lastModified: 456 });
  const duplicate = fakeFile("a.jpg", { size: 123, lastModified: 456 });

  assert.equal(store.add([first]), 1);
  assert.equal(store.add([duplicate]), 0);

  assert.equal(store.list().length, 1);
});

test("training file store deduplicates folder files by relative path", () => {
  const store = createTrainingFileStore();

  assert.equal(store.add([
    fakeFile("IMG_0001.jpg", { webkitRelativePath: "set1/IMG_0001.jpg" }),
    fakeFile("IMG_0001.jpg", { webkitRelativePath: "set2/IMG_0001.jpg" }),
  ]), 2);
  assert.equal(store.add([
    fakeFile("IMG_0001.jpg", { webkitRelativePath: "set1/IMG_0001.jpg" }),
  ]), 0);

  assert.deepEqual(store.list().map((file) => file.webkitRelativePath), [
    "set1/IMG_0001.jpg",
    "set2/IMG_0001.jpg",
  ]);
});

test("training file store can clear accumulated files", () => {
  const store = createTrainingFileStore([fakeFile("a.jpg"), fakeFile("b.jpg")]);

  store.clear();

  assert.deepEqual(store.list(), []);
  assert.equal(store.add([fakeFile("a.jpg")]), 1);
});
