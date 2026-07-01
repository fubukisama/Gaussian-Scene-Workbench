export function shouldKeepUnsavedMeshTrim(meshDirty, meshData) {
  return Boolean(meshDirty && meshData);
}
