function decodeHeader(buffer) {
  const bytes = new Uint8Array(buffer);
  const marker = new TextEncoder().encode("end_header");
  let markerAt = -1;
  for (let i = 0; i <= bytes.length - marker.length; i++) {
    let ok = true;
    for (let j = 0; j < marker.length; j++) {
      if (bytes[i + j] !== marker[j]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      markerAt = i;
      break;
    }
  }
  if (markerAt < 0) throw new Error("Invalid PLY: missing end_header");
  let dataOffset = markerAt + marker.length;
  if (bytes[dataOffset] === 13) dataOffset++;
  if (bytes[dataOffset] === 10) dataOffset++;
  const header = new TextDecoder().decode(bytes.slice(0, dataOffset));
  return { header, dataOffset };
}

function parseHeader(header) {
  const lines = header.split(/\r?\n/);
  const formatLine = lines.find((line) => line.startsWith("format "));
  const format = formatLine?.split(/\s+/)[1];
  let vertexCount = 0;
  let faceCount = 0;
  const vertexProperties = [];
  let element = null;
  for (const line of lines) {
    const parts = line.trim().split(/\s+/);
    if (parts[0] === "element") {
      element = parts[1];
      if (element === "vertex") vertexCount = Number(parts[2]);
      if (element === "face") faceCount = Number(parts[2]);
      continue;
    }
    if (parts[0] === "property" && element === "vertex") {
      vertexProperties.push({ type: parts[1], name: parts[2] });
    }
  }
  return { format, vertexCount, faceCount, vertexProperties };
}

function scalarSize(type) {
  return {
    char: 1,
    uchar: 1,
    int8: 1,
    uint8: 1,
    short: 2,
    ushort: 2,
    int16: 2,
    uint16: 2,
    int: 4,
    uint: 4,
    int32: 4,
    uint32: 4,
    float: 4,
    float32: 4,
    double: 8,
    float64: 8,
  }[type] || 4;
}

function readScalar(view, offset, type, littleEndian = true) {
  switch (type) {
    case "char":
    case "int8":
      return view.getInt8(offset);
    case "uchar":
    case "uint8":
      return view.getUint8(offset);
    case "short":
    case "int16":
      return view.getInt16(offset, littleEndian);
    case "ushort":
    case "uint16":
      return view.getUint16(offset, littleEndian);
    case "int":
    case "int32":
      return view.getInt32(offset, littleEndian);
    case "uint":
    case "uint32":
      return view.getUint32(offset, littleEndian);
    case "double":
    case "float64":
      return view.getFloat64(offset, littleEndian);
    default:
      return view.getFloat32(offset, littleEndian);
  }
}

export function parsePlyMesh(buffer) {
  const { header, dataOffset } = decodeHeader(buffer);
  const { format, vertexCount, faceCount, vertexProperties } = parseHeader(header);
  if (!format || !vertexCount) throw new Error("Invalid PLY mesh");
  if (format === "ascii") return parseAsciiMesh(buffer, dataOffset, vertexCount, faceCount, vertexProperties);
  if (format !== "binary_little_endian") throw new Error(`Unsupported PLY format: ${format}`);
  return parseBinaryLittleEndianMesh(buffer, dataOffset, vertexCount, faceCount, vertexProperties);
}

function colorChannelName(name) {
  const lower = name.toLowerCase();
  if (lower === "red" || lower === "r" || lower === "diffuse_red") return 0;
  if (lower === "green" || lower === "g" || lower === "diffuse_green") return 1;
  if (lower === "blue" || lower === "b" || lower === "diffuse_blue") return 2;
  return -1;
}

function hasColorProperties(vertexProperties) {
  const channels = new Set(vertexProperties.map((prop) => colorChannelName(prop.name)).filter((channel) => channel >= 0));
  return channels.has(0) && channels.has(1) && channels.has(2);
}

function colorValue(value, type) {
  if (type === "float" || type === "float32" || type === "double" || type === "float64") {
    return Math.max(0, Math.min(255, Math.round(value * 255)));
  }
  return Math.max(0, Math.min(255, Math.round(value)));
}

function parseAsciiMesh(buffer, dataOffset, vertexCount, faceCount, vertexProperties) {
  const text = new TextDecoder().decode(new Uint8Array(buffer, dataOffset));
  const lines = text.trim().split(/\r?\n/);
  const positions = new Float32Array(vertexCount * 3);
  const normals = new Float32Array(vertexCount * 3);
  const hasVertexColors = hasColorProperties(vertexProperties);
  const colors = hasVertexColors ? new Uint8Array(vertexCount * 3) : null;
  for (let i = 0; i < vertexCount; i++) {
    const values = lines[i].trim().split(/\s+/).map(Number);
    const base = i * 3;
    for (let p = 0; p < vertexProperties.length; p++) {
      const prop = vertexProperties[p];
      const value = values[p];
      if (prop.name === "x") positions[base] = value;
      if (prop.name === "y") positions[base + 1] = value;
      if (prop.name === "z") positions[base + 2] = value;
      if (prop.name === "nx") normals[base] = value;
      if (prop.name === "ny") normals[base + 1] = value;
      if (prop.name === "nz") normals[base + 2] = value;
      const channel = colorChannelName(prop.name);
      if (colors && channel >= 0) colors[base + channel] = colorValue(value, prop.type);
    }
  }
  const indices = [];
  for (let i = 0; i < faceCount; i++) {
    const values = lines[vertexCount + i].trim().split(/\s+/).map(Number);
    const n = values[0];
    for (let j = 1; j < n - 1; j++) indices.push(values[1], values[j + 1], values[j + 2]);
  }
  return { positions, normals, colors, hasVertexColors, indices: new Uint32Array(indices), vertexCount, faceCount };
}

function parseBinaryLittleEndianMesh(buffer, dataOffset, vertexCount, faceCount, vertexProperties) {
  const view = new DataView(buffer);
  const positions = new Float32Array(vertexCount * 3);
  const normals = new Float32Array(vertexCount * 3);
  const hasVertexColors = hasColorProperties(vertexProperties);
  const colors = hasVertexColors ? new Uint8Array(vertexCount * 3) : null;
  let offset = dataOffset;
  for (let i = 0; i < vertexCount; i++) {
    for (const prop of vertexProperties) {
      const value = readScalar(view, offset, prop.type, true);
      offset += scalarSize(prop.type);
      const base = i * 3;
      if (prop.name === "x") positions[base] = value;
      if (prop.name === "y") positions[base + 1] = value;
      if (prop.name === "z") positions[base + 2] = value;
      if (prop.name === "nx") normals[base] = value;
      if (prop.name === "ny") normals[base + 1] = value;
      if (prop.name === "nz") normals[base + 2] = value;
      const channel = colorChannelName(prop.name);
      if (colors && channel >= 0) colors[base + channel] = colorValue(value, prop.type);
    }
  }
  const indices = [];
  for (let i = 0; i < faceCount; i++) {
    const n = view.getUint8(offset);
    offset += 1;
    const face = [];
    for (let j = 0; j < n; j++) {
      face.push(view.getInt32(offset, true));
      offset += 4;
    }
    for (let j = 1; j < n - 1; j++) indices.push(face[0], face[j], face[j + 1]);
  }
  return { positions, normals, colors, hasVertexColors, indices: new Uint32Array(indices), vertexCount, faceCount };
}
