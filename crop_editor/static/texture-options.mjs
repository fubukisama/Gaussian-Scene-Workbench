export function textureMaxFacesFromQuality(value) {
  if (value === "fast") return 30000;
  if (value === "smooth") return 100000;
  if (value === "original") return 0;
  if (value === "ultra") return 0;
  return 100000;
}

export function textureOptionsFromQuality(value, requestedTextureRes) {
  const textureRes = Number.isFinite(Number(requestedTextureRes)) ? Number(requestedTextureRes) : 4096;
  if (value === "ultra") {
    return {
      maxFaces: 0,
      textureRes: Math.max(textureRes, 8192),
      localSeamLeveling: false,
      costSmoothnessRatio: 0.3,
      textureSizeMultiple: 8192,
    };
  }
  if (value === "smooth") {
    return {
      maxFaces: 100000,
      textureRes: Math.max(textureRes, 8192),
      localSeamLeveling: false,
      costSmoothnessRatio: 0.3,
      textureSizeMultiple: 8192,
    };
  }
  return {
    maxFaces: textureMaxFacesFromQuality(value),
    textureRes,
    localSeamLeveling: false,
    costSmoothnessRatio: 0.3,
    textureSizeMultiple: 0,
  };
}
