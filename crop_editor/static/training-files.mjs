export function trainingFileKey(file) {
  const path = file.webkitRelativePath || file.name || "";
  const size = Number.isFinite(file.size) ? file.size : 0;
  const modified = Number.isFinite(file.lastModified) ? file.lastModified : 0;
  return `${path}\u0000${size}\u0000${modified}`;
}

export function createTrainingFileStore(initialFiles = []) {
  const files = [];
  const keys = new Set();

  function add(nextFiles) {
    let added = 0;
    for (const file of Array.from(nextFiles || [])) {
      const key = trainingFileKey(file);
      if (keys.has(key)) continue;
      keys.add(key);
      files.push(file);
      added++;
    }
    return added;
  }

  add(initialFiles);

  return {
    add,
    clear() {
      files.length = 0;
      keys.clear();
    },
    list() {
      return files.slice();
    },
  };
}
