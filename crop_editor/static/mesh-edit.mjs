export function compactMeshByDeletedFaces(meshData, deletedFaces) {
  const faceCount = meshData.indices.length / 3;
  const kept = [];
  const remap = new Map();
  const positions = [];
  const uvs = meshData.uvs?.length === (meshData.positions.length / 3) * 2 ? [] : null;
  const normals = meshData.normals?.length === meshData.positions.length ? [] : null;
  const colors = meshData.colors?.length === meshData.positions.length ? [] : null;

  function mappedVertex(index) {
    if (remap.has(index)) return remap.get(index);
    const next = remap.size;
    const src = index * 3;
    positions.push(meshData.positions[src], meshData.positions[src + 1], meshData.positions[src + 2]);
    if (uvs) {
      const uvSrc = index * 2;
      uvs.push(meshData.uvs[uvSrc], meshData.uvs[uvSrc + 1]);
    }
    if (normals) normals.push(meshData.normals[src], meshData.normals[src + 1], meshData.normals[src + 2]);
    if (colors) colors.push(meshData.colors[src], meshData.colors[src + 1], meshData.colors[src + 2]);
    remap.set(index, next);
    return next;
  }

  for (let face = 0; face < faceCount; face++) {
    if (deletedFaces.has(face)) continue;
    const base = face * 3;
    kept.push(
      mappedVertex(meshData.indices[base]),
      mappedVertex(meshData.indices[base + 1]),
      mappedVertex(meshData.indices[base + 2])
    );
  }
  return {
    ...meshData,
    positions: new Float32Array(positions),
    uvs: uvs ? new Float32Array(uvs) : meshData.uvs,
    normals: normals ? new Float32Array(normals) : meshData.normals,
    colors: colors ? new Uint8Array(colors) : meshData.colors,
    indices: new Uint32Array(kept),
    vertexCount: positions.length / 3,
    faceCount: kept.length / 3,
  };
}

export function facesTouchingVertices(indices, selectedVertices) {
  const faces = new Set();
  for (let i = 0; i < indices.length; i += 3) {
    if (selectedVertices.has(indices[i]) || selectedVertices.has(indices[i + 1]) || selectedVertices.has(indices[i + 2])) {
      faces.add(i / 3);
    }
  }
  return faces;
}

export function selectableVertices(meshData, deletedFaces = new Set()) {
  const vertices = new Set();
  const faceCount = meshData.indices.length / 3;
  for (let face = 0; face < faceCount; face++) {
    if (deletedFaces.has(face)) continue;
    const base = face * 3;
    vertices.add(meshData.indices[base]);
    vertices.add(meshData.indices[base + 1]);
    vertices.add(meshData.indices[base + 2]);
  }
  return vertices;
}

export function meshToAsciiPly(meshData, deletedFaces = new Set()) {
  const compact = compactMeshByDeletedFaces(meshData, deletedFaces);
  const hasColors = compact.colors?.length === compact.positions.length;
  const lines = [
    "ply",
    "format ascii 1.0",
    `element vertex ${compact.positions.length / 3}`,
    "property float x",
    "property float y",
    "property float z",
    ...(hasColors ? [
      "property uchar red",
      "property uchar green",
      "property uchar blue",
    ] : []),
    `element face ${compact.indices.length / 3}`,
    "property list uchar int vertex_indices",
    "end_header",
  ];
  for (let i = 0; i < compact.positions.length; i += 3) {
    const xyz = `${compact.positions[i]} ${compact.positions[i + 1]} ${compact.positions[i + 2]}`;
    if (hasColors) {
      lines.push(`${xyz} ${compact.colors[i]} ${compact.colors[i + 1]} ${compact.colors[i + 2]}`);
    } else {
      lines.push(xyz);
    }
  }
  for (let i = 0; i < compact.indices.length; i += 3) {
    lines.push(`3 ${compact.indices[i]} ${compact.indices[i + 1]} ${compact.indices[i + 2]}`);
  }
  return `${lines.join("\n")}\n`;
}
