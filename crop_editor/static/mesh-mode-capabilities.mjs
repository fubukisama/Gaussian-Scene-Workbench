export function meshModeUsesSugar(mode) {
  return mode === "sugar";
}

export function meshModeUsesGs2Mesh(mode) {
  return mode === "gs2mesh";
}

export function meshModeUses3dgs(mode) {
  return meshModeUsesSugar(mode) || meshModeUsesGs2Mesh(mode);
}

export function meshModeMatchesBackend(mode, backend) {
  return meshModeUses3dgs(mode) ? backend === "3dgs" : backend === "2dgs";
}

export function meshModeSupportsTextureBake(mode, backend) {
  if (meshModeUsesGs2Mesh(mode)) return backend === "3dgs";
  if (meshModeUsesSugar(mode)) return false;
  return backend === "2dgs";
}

export function meshActionsForMode(mode, backend, hasScene, busy = false) {
  const matching = Boolean(hasScene) && meshModeMatchesBackend(mode, backend);
  const locked = Boolean(busy) || !matching;
  return {
    exportMeshDisabled: locked,
    loadMeshDisabled: locked,
    bakeTextureDisabled: Boolean(busy) || !hasScene || !meshModeSupportsTextureBake(mode, backend),
    loadTextureDisabled: locked,
  };
}
