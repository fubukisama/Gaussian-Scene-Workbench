export function parseMeshChunk(buffer) {
  const view = new DataView(buffer);
  const headerLength = view.getUint32(0, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, 4, headerLength)));
  if (header.format !== "3dgs-editor-mesh-chunk-v1" && header.format !== "3dgs-editor-textured-mesh-chunk-v1") {
    throw new Error("Unsupported mesh chunk format");
  }
  const vertexCount = Number(header.vertex_count || 0);
  const faceCount = Number(header.face_count || 0);
  const rawDataOffset = 4 + headerLength;
  const alignedDataOffset = rawDataOffset + ((4 - (rawDataOffset % 4)) % 4);
  const payloadByteLength = header.format === "3dgs-editor-textured-mesh-chunk-v1"
    ? vertexCount * 3 * 4 + vertexCount * 2 * 4 + faceCount * 3 * 4
    : vertexCount * 3 * 4 + (header.has_vertex_colors ? vertexCount * 3 : 0) + faceCount * 3 * 4;
  const positionOffset = buffer.byteLength >= alignedDataOffset + payloadByteLength
    ? alignedDataOffset
    : rawDataOffset;
  const colorOffset = positionOffset + vertexCount * 3 * 4;
  const float32View = (byteOffset, length) => {
    if (byteOffset % 4 === 0) return new Float32Array(buffer, byteOffset, length);
    return new Float32Array(buffer.slice(byteOffset, byteOffset + length * 4));
  };
  const uint32View = (byteOffset, length) => {
    if (byteOffset % 4 === 0) return new Uint32Array(buffer, byteOffset, length);
    return new Uint32Array(buffer.slice(byteOffset, byteOffset + length * 4));
  };
  if (header.format === "3dgs-editor-textured-mesh-chunk-v1") {
    const uvOffset = colorOffset;
    const indexOffset = uvOffset + vertexCount * 2 * 4;
    return {
      vertexCount,
      faceCount,
      positions: float32View(positionOffset, vertexCount * 3),
      uvs: float32View(uvOffset, vertexCount * 2),
      indices: uint32View(indexOffset, faceCount * 3),
      colors: null,
      hasVertexColors: false,
      hasTextureMap: true,
    };
  }
  const indexOffset = colorOffset + (header.has_vertex_colors ? vertexCount * 3 : 0);
  return {
    vertexCount,
    faceCount,
    positions: float32View(positionOffset, vertexCount * 3),
    colors: header.has_vertex_colors ? new Uint8Array(buffer, colorOffset, vertexCount * 3) : null,
    indices: uint32View(indexOffset, faceCount * 3),
    hasVertexColors: Boolean(header.has_vertex_colors),
  };
}
