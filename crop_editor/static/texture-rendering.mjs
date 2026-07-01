export function configureOpenMvsTexture(texture, renderer, THREERef) {
  texture.flipY = true;
  if ("colorSpace" in texture && THREERef && "SRGBColorSpace" in THREERef) {
    texture.colorSpace = THREERef.SRGBColorSpace;
  }
  texture.anisotropy = Math.min(renderer?.capabilities?.getMaxAnisotropy?.() || 1, 8);
  texture.needsUpdate = true;
  return texture;
}
