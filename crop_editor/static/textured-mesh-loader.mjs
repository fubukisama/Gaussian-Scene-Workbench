function resolveIndex(raw, count) {
  const value = Number(raw);
  if (!Number.isFinite(value) || value === 0) return null;
  return value > 0 ? value - 1 : count + value;
}

export function parseObjTexturedMesh(text) {
  const vertices = [];
  const texcoords = [];
  const positions = [];
  const uvs = [];
  const indices = [];
  const refToIndex = new Map();

  function vertexForRef(ref) {
    if (refToIndex.has(ref)) return refToIndex.get(ref);
    const parts = ref.split("/");
    const vertexIndex = resolveIndex(parts[0], vertices.length);
    const texcoordIndex = parts[1] ? resolveIndex(parts[1], texcoords.length) : null;
    if (vertexIndex === null || !vertices[vertexIndex]) {
      throw new Error(`Invalid OBJ vertex reference: ${ref}`);
    }
    const next = positions.length / 3;
    positions.push(...vertices[vertexIndex]);
    if (texcoordIndex !== null && texcoords[texcoordIndex]) {
      uvs.push(...texcoords[texcoordIndex]);
    } else {
      uvs.push(0, 0);
    }
    refToIndex.set(ref, next);
    return next;
  }

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("#")) continue;
    const parts = line.split(/\s+/);
    if (parts[0] === "v" && parts.length >= 4) {
      vertices.push([Number(parts[1]), Number(parts[2]), Number(parts[3])]);
    } else if (parts[0] === "vt" && parts.length >= 3) {
      texcoords.push([Number(parts[1]), Number(parts[2])]);
    } else if (parts[0] === "f" && parts.length >= 4) {
      const face = parts.slice(1).map(vertexForRef);
      for (let i = 1; i < face.length - 1; i++) {
        indices.push(face[0], face[i], face[i + 1]);
      }
    }
  }

  if (!positions.length || !indices.length) throw new Error("OBJ has no textured triangles");
  return {
    positions: new Float32Array(positions),
    uvs: new Float32Array(uvs),
    indices: new Uint32Array(indices),
    vertexCount: positions.length / 3,
    faceCount: indices.length / 3,
  };
}
