#!/usr/bin/env node

// Minimal, dependency-free Blockbench "free" model exporter for DaiBed.
// It intentionally supports the subset used by the hero assets: textured
// cubes with per-face UV rectangles and optional Euler rotation.

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { basename, dirname, extname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const GL_ARRAY_BUFFER = 34962;
const GL_ELEMENT_ARRAY_BUFFER = 34963;
const GL_FLOAT = 5126;
const GL_UNSIGNED_SHORT = 5123;
const BLOCKBENCH_UNITS_PER_WORLD_UNIT = 16;

const FACE_LAYOUTS = {
  north: {
    normal: [0, 0, -1],
    corners: (a, b) => [[a[0], a[1], a[2]], [a[0], b[1], a[2]], [b[0], b[1], a[2]], [b[0], a[1], a[2]]],
  },
  south: {
    normal: [0, 0, 1],
    corners: (a, b) => [[a[0], a[1], b[2]], [b[0], a[1], b[2]], [b[0], b[1], b[2]], [a[0], b[1], b[2]]],
  },
  east: {
    normal: [1, 0, 0],
    corners: (a, b) => [[b[0], a[1], a[2]], [b[0], b[1], a[2]], [b[0], b[1], b[2]], [b[0], a[1], b[2]]],
  },
  west: {
    normal: [-1, 0, 0],
    corners: (a, b) => [[a[0], a[1], a[2]], [a[0], a[1], b[2]], [a[0], b[1], b[2]], [a[0], b[1], a[2]]],
  },
  up: {
    normal: [0, 1, 0],
    corners: (a, b) => [[a[0], b[1], a[2]], [a[0], b[1], b[2]], [b[0], b[1], b[2]], [b[0], b[1], a[2]]],
  },
  down: {
    normal: [0, -1, 0],
    corners: (a, b) => [[a[0], a[1], a[2]], [b[0], a[1], a[2]], [b[0], a[1], b[2]], [a[0], a[1], b[2]]],
  },
};

function align4(value) {
  return (value + 3) & ~3;
}

function rotateEuler(point, rotation) {
  const [rx, ry, rz] = rotation.map((degrees) => Number(degrees || 0) * Math.PI / 180);
  let [x, y, z] = point;

  if (rx !== 0) {
    const c = Math.cos(rx);
    const s = Math.sin(rx);
    [y, z] = [y * c - z * s, y * s + z * c];
  }
  if (ry !== 0) {
    const c = Math.cos(ry);
    const s = Math.sin(ry);
    [x, z] = [x * c + z * s, -x * s + z * c];
  }
  if (rz !== 0) {
    const c = Math.cos(rz);
    const s = Math.sin(rz);
    [x, y] = [x * c - y * s, x * s + y * c];
  }
  return [x, y, z];
}

function transformPoint(point, element) {
  const origin = element.origin || [0, 0, 0];
  const local = point.map((value, axis) => Number(value) - Number(origin[axis] || 0));
  const rotated = rotateEuler(local, element.rotation || [0, 0, 0]);
  return rotated.map((value, axis) => (value + Number(origin[axis] || 0)) / BLOCKBENCH_UNITS_PER_WORLD_UNIT);
}

function transformNormal(normal, element) {
  const rotated = rotateEuler(normal, element.rotation || [0, 0, 0]);
  const length = Math.hypot(...rotated) || 1;
  return rotated.map((value) => value / length);
}

function faceUvs(face, width, height) {
  const uv = face.uv || [0, 0, width, height];
  let result = [
    [Number(uv[0]) / width, Number(uv[3]) / height],
    [Number(uv[0]) / width, Number(uv[1]) / height],
    [Number(uv[2]) / width, Number(uv[1]) / height],
    [Number(uv[2]) / width, Number(uv[3]) / height],
  ];
  const quarterTurns = (((Number(face.rotation || 0) / 90) % 4) + 4) % 4;
  for (let i = 0; i < quarterTurns; ++i) {
    result = [result[3], result[0], result[1], result[2]];
  }
  return result;
}

function embeddedTexture(model, inputPath) {
  const texture = model.textures?.[0];
  if (!texture) {
    throw new Error(`${inputPath}: model does not contain a texture`);
  }
  const source = texture.source || "";
  const match = /^data:image\/png;base64,(.+)$/s.exec(source);
  if (!match) {
    throw new Error(`${inputPath}: the primary texture must be an embedded PNG`);
  }
  return Buffer.from(match[1], "base64");
}

function appendChunk(chunks, data, target) {
  const offset = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const paddedLength = align4(data.length);
  const padded = Buffer.alloc(paddedLength);
  data.copy(padded);
  chunks.push(padded);
  return { buffer: 0, byteOffset: offset, byteLength: data.length, ...(target ? { target } : {}) };
}

function arrayBuffer(values, kind) {
  const typed = kind === "uint16" ? new Uint16Array(values) : new Float32Array(values);
  return Buffer.from(typed.buffer, typed.byteOffset, typed.byteLength);
}

function buildGeometry(model) {
  const positions = [];
  const normals = [];
  const texcoords = [];
  const indices = [];
  const width = Number(model.resolution?.width || 64);
  const height = Number(model.resolution?.height || 64);

  for (const element of model.elements || []) {
    if (element.type !== "cube" || element.export === false) continue;
    const from = element.from.map(Number);
    const to = element.to.map(Number);
    const a = from.map((value, axis) => Math.min(value, to[axis]));
    const b = from.map((value, axis) => Math.max(value, to[axis]));

    for (const [faceName, layout] of Object.entries(FACE_LAYOUTS)) {
      const face = element.faces?.[faceName];
      if (!face || face.texture === null || face.texture === undefined) continue;
      const corners = layout.corners(a, b);
      const uv = faceUvs(face, width, height);
      const normal = transformNormal(layout.normal, element);
      const base = positions.length / 3;
      for (let i = 0; i < 4; ++i) {
        positions.push(...transformPoint(corners[i], element));
        normals.push(...normal);
        texcoords.push(...uv[i]);
      }
      indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
    }
  }

  if (positions.length === 0) throw new Error("model contains no exportable cube faces");
  return { positions, normals, texcoords, indices };
}

function bounds(values, components) {
  const min = Array(components).fill(Number.POSITIVE_INFINITY);
  const max = Array(components).fill(Number.NEGATIVE_INFINITY);
  for (let i = 0; i < values.length; i += components) {
    for (let axis = 0; axis < components; ++axis) {
      min[axis] = Math.min(min[axis], values[i + axis]);
      max[axis] = Math.max(max[axis], values[i + axis]);
    }
  }
  return { min, max };
}

export async function convertBbmodel(inputFile, outputFile = undefined) {
  const inputPath = resolve(inputFile);
  const outputPath = resolve(outputFile || inputPath.slice(0, -extname(inputPath).length) + ".glb");
  const model = JSON.parse(await readFile(inputPath, "utf8"));
  const png = embeddedTexture(model, inputPath);
  const geometry = buildGeometry(model);
  const chunks = [];
  const bufferViews = [];

  bufferViews.push(appendChunk(chunks, arrayBuffer(geometry.positions, "float"), GL_ARRAY_BUFFER));
  bufferViews.push(appendChunk(chunks, arrayBuffer(geometry.normals, "float"), GL_ARRAY_BUFFER));
  bufferViews.push(appendChunk(chunks, arrayBuffer(geometry.texcoords, "float"), GL_ARRAY_BUFFER));
  if (geometry.positions.length / 3 > 65535) {
    throw new Error(`${inputPath}: model exceeds the 16-bit index limit`);
  }
  bufferViews.push(appendChunk(chunks, arrayBuffer(geometry.indices, "uint16"), GL_ELEMENT_ARRAY_BUFFER));
  bufferViews.push(appendChunk(chunks, png));
  const binary = Buffer.concat(chunks);
  const positionBounds = bounds(geometry.positions, 3);
  const modelName = model.name || basename(inputPath, extname(inputPath));

  const gltf = {
    asset: { version: "2.0", generator: "DaiBed bbmodel_to_glb" },
    scene: 0,
    scenes: [{ nodes: [0] }],
    nodes: [{ name: modelName, mesh: 0 }],
    meshes: [{ name: `${modelName}Mesh`, primitives: [{
      attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 },
      indices: 3,
      material: 0,
    }] }],
    materials: [{
      name: `${modelName}Material`,
      pbrMetallicRoughness: {
        baseColorFactor: [1, 1, 1, 1],
        baseColorTexture: { index: 0 },
        metallicFactor: 0,
        roughnessFactor: 1,
      },
      alphaMode: "OPAQUE",
    }],
    textures: [{ sampler: 0, source: 0 }],
    samplers: [{ magFilter: 9728, minFilter: 9728, wrapS: 10497, wrapT: 10497 }],
    images: [{ name: `${modelName}Atlas`, bufferView: 4, mimeType: "image/png" }],
    accessors: [
      { bufferView: 0, componentType: GL_FLOAT, count: geometry.positions.length / 3, type: "VEC3", ...positionBounds },
      { bufferView: 1, componentType: GL_FLOAT, count: geometry.normals.length / 3, type: "VEC3" },
      { bufferView: 2, componentType: GL_FLOAT, count: geometry.texcoords.length / 2, type: "VEC2" },
      { bufferView: 3, componentType: GL_UNSIGNED_SHORT, count: geometry.indices.length, type: "SCALAR", min: [0], max: [geometry.positions.length / 3 - 1] },
    ],
    bufferViews,
    buffers: [{ byteLength: binary.length }],
  };

  let json = Buffer.from(JSON.stringify(gltf), "utf8");
  if (json.length !== align4(json.length)) {
    json = Buffer.concat([json, Buffer.alloc(align4(json.length) - json.length, 0x20)]);
  }

  const header = Buffer.alloc(12);
  header.writeUInt32LE(0x46546c67, 0);
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(12 + 8 + json.length + 8 + binary.length, 8);
  const jsonHeader = Buffer.alloc(8);
  jsonHeader.writeUInt32LE(json.length, 0);
  jsonHeader.writeUInt32LE(0x4e4f534a, 4);
  const binaryHeader = Buffer.alloc(8);
  binaryHeader.writeUInt32LE(binary.length, 0);
  binaryHeader.writeUInt32LE(0x004e4942, 4);

  await mkdir(dirname(outputPath), { recursive: true });
  await writeFile(outputPath, Buffer.concat([header, jsonHeader, json, binaryHeader, binary]));
  const pngPath = outputPath.slice(0, -extname(outputPath).length) + ".png";
  await writeFile(pngPath, png);
  return { outputPath, pngPath, vertices: geometry.positions.length / 3, triangles: geometry.indices.length / 3 };
}

const isMain = process.argv[1] && resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url));
if (isMain) {
  if (process.argv.length < 3 || process.argv.length > 4) {
    console.error("Usage: node scripts/convert_bbmodel_to_glb.mjs <input.bbmodel> [output.glb]");
    process.exitCode = 2;
  } else {
    convertBbmodel(process.argv[2], process.argv[3])
      .then((result) => console.log(`${result.outputPath}: ${result.vertices} vertices, ${result.triangles} triangles`))
      .catch((error) => {
        console.error(error.message);
        process.exitCode = 1;
      });
  }
}
