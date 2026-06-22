#!/usr/bin/env node

// Deterministic source generator for the four heroes introduced after Radon
// and Orbita. The output remains ordinary Blockbench JSON and can be edited
// by hand in Blockbench after generation.

import { createHash } from "node:crypto";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { deflateSync } from "node:zlib";

const SIZE = 64;
const TILE = {
  cloth: 0,
  secondary: 1,
  skin: 2,
  detail: 3,
  metal: 4,
  tech: 5,
  glow: 6,
  prop: 7,
  ghost: 8,
};

function id(seed) {
  return createHash("md5").update(`daibed:${seed}`).digest("hex");
}

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const name = Buffer.from(type, "ascii");
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0);
  name.copy(chunk, 4);
  data.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(Buffer.concat([name, data])), 8 + data.length);
  return chunk;
}

function createAtlas(palette) {
  const rgba = Buffer.alloc(SIZE * SIZE * 4);
  const setPixel = (x, y, color) => {
    const offset = (y * SIZE + x) * 4;
    rgba[offset] = color[0]; rgba[offset + 1] = color[1]; rgba[offset + 2] = color[2]; rgba[offset + 3] = 255;
  };

  for (let tile = 0; tile < 64; ++tile) {
    const colors = palette[tile % palette.length];
    const ox = (tile % 8) * 8;
    const oy = Math.floor(tile / 8) * 8;
    for (let y = 0; y < 8; ++y) {
      for (let x = 0; x < 8; ++x) {
        let color = colors[0];
        if (x === 0 || y === 7) color = colors[2];
        else if (x === 7 || y === 0) color = colors[1];
        else if ((x * 3 + y * 5 + tile) % 11 === 0) color = colors[3];
        else if ((x + y + tile) % 7 === 0) color = colors[1];
        setPixel(ox + x, oy + y, color);
      }
    }
  }

  const scanlines = Buffer.alloc((SIZE * 4 + 1) * SIZE);
  for (let y = 0; y < SIZE; ++y) {
    const row = y * (SIZE * 4 + 1);
    scanlines[row] = 0;
    rgba.copy(scanlines, row + 1, y * SIZE * 4, (y + 1) * SIZE * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(SIZE, 0); ihdr.writeUInt32BE(SIZE, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", deflateSync(scanlines, { level: 9 })),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

function uvFor(tile) {
  const x = (tile % 8) * 8;
  const y = Math.floor(tile / 8) * 8;
  return [x, y, x + 8, y + 8];
}

function makeFaces(tile) {
  const uv = uvFor(tile);
  return Object.fromEntries(["north", "east", "south", "west", "up", "down"].map((face) => [face, { uv: [...uv], texture: 0 }]));
}

function createModel(name, slug, palette, build) {
  const elements = [];
  const groups = new Map();
  const groupNames = ["Head", "Body", "Arms", "Legs", "Props", "AbilityProps", "VFXPreview"];
  for (const group of groupNames) groups.set(group, []);

  const add = (group, elementName, from, to, tile, options = {}) => {
    const uuid = id(`${slug}:element:${group}:${elementName}`);
    const element = {
      name: elementName,
      box_uv: false,
      render_order: "default",
      locked: false,
      export: options.export !== false,
      from,
      to,
      autouv: 0,
      color: options.color || 0,
      origin: options.origin || from.map((value, axis) => (value + to[axis]) * 0.5),
      faces: makeFaces(tile),
      type: "cube",
      uuid,
    };
    if (options.rotation) element.rotation = options.rotation;
    elements.push(element);
    groups.get(group).push(uuid);
  };
  build(add);

  const png = createAtlas(palette);
  const textureName = slug === "witness" ? "witness" : slug;
  const outliner = groupNames.map((group, index) => ({
    name: group,
    origin: [0, 0, 0],
    color: index,
    uuid: id(`${slug}:group:${group}`),
    export: true,
    isOpen: true,
    autouv: 0,
    children: groups.get(group),
  }));
  return {
    model: {
      meta: { format_version: "5.0", model_format: "free", box_uv: false },
      name,
      resolution: { width: SIZE, height: SIZE },
      elements,
      outliner,
      textures: [{
        path: "",
        name: textureName,
        folder: "",
        namespace: "",
        id: "0",
        particle: false,
        render_mode: "default",
        render_sides: "auto",
        frame_time: 1,
        frame_order_type: "loop",
        frame_order: "",
        frame_interpolate: false,
        visible: true,
        internal: true,
        saved: false,
        uuid: id(`${slug}:texture`),
        source: `data:image/png;base64,${png.toString("base64")}`,
        uv_width: SIZE,
        uv_height: SIZE,
      }],
    },
    png,
  };
}

const heroes = [
  {
    name: "Brom", slug: "brom", file: "Brom",
    palette: [
      [[38, 43, 45], [58, 66, 66], [20, 24, 26], [78, 91, 80]],
      [[55, 74, 58], [78, 103, 75], [31, 45, 35], [112, 132, 90]],
      [[218, 164, 119], [244, 194, 145], [156, 105, 73], [232, 177, 126]],
      [[195, 75, 22], [242, 119, 38], [116, 42, 16], [255, 157, 47]],
      [[105, 112, 112], [177, 184, 178], [57, 63, 65], [214, 205, 160]],
      [[26, 35, 31], [53, 79, 60], [10, 15, 14], [85, 158, 88]],
      [[75, 205, 102], [164, 255, 147], [21, 91, 47], [225, 255, 170]],
      [[77, 55, 38], [119, 82, 50], [38, 29, 24], [188, 129, 67]],
      [[78, 91, 86], [122, 141, 131], [42, 48, 47], [93, 180, 133]],
    ],
    build(add) {
      add("Legs", "LeftBoot", [-6, 0, -3], [-1, 4, 3], TILE.cloth); add("Legs", "RightBoot", [1, 0, -3], [6, 4, 3], TILE.cloth);
      add("Legs", "LeftLeg", [-5, 4, -2], [-1, 10, 2], TILE.secondary); add("Legs", "RightLeg", [1, 4, -2], [5, 10, 2], TILE.secondary);
      add("Body", "Torso", [-7, 9, -3], [7, 22, 3], TILE.secondary); add("Body", "WorkApron", [-5, 10, -4], [5, 20, -3], TILE.cloth);
      add("Body", "Belt", [-7.3, 11, -3.4], [7.3, 13, 3.4], TILE.prop); add("Body", "BeltBuckle", [-2, 11, -4], [2, 14, -3.3], TILE.metal);
      add("Body", "LeftPouch", [-8, 11, -2], [-6.8, 16, 2], TILE.prop); add("Body", "RightPouch", [6.8, 11, -2], [8, 16, 2], TILE.prop);
      add("Body", "ChestGauge", [-2.5, 16, -4.1], [2.5, 20.5, -3.3], TILE.tech); add("Body", "GaugeGlow", [-1.1, 17.2, -4.3], [1.1, 19.3, -4.05], TILE.glow);
      add("Body", "Backpack", [-5, 12, 3], [5, 21, 6], TILE.tech); add("Body", "BackpackRail", [-6, 14, 5.7], [6, 17, 6.5], TILE.metal);
      add("Head", "BaldHead", [-5, 22, -4], [5, 31, 4], TILE.skin); add("Head", "LeftEar", [-6, 24, -2], [-5, 28, 2], TILE.skin); add("Head", "RightEar", [5, 24, -2], [6, 28, 2], TILE.skin);
      add("Head", "LeftGoggle", [-4.4, 26, -5], [-0.6, 29.5, -3.8], TILE.cloth); add("Head", "RightGoggle", [0.6, 26, -5], [4.4, 29.5, -3.8], TILE.cloth);
      add("Head", "GoggleBridge", [-1, 27.2, -5.1], [1, 28.2, -4], TILE.metal); add("Head", "LeftLens", [-3.4, 26.8, -5.2], [-1.4, 28.8, -4.9], TILE.tech); add("Head", "RightLens", [1.4, 26.8, -5.2], [3.4, 28.8, -4.9], TILE.tech);
      add("Head", "MoustacheLeft", [-5.2, 23.1, -5.2], [-0.2, 26.2, -3.8], TILE.detail, { rotation: [0, 0, 7] }); add("Head", "MoustacheRight", [0.2, 23.1, -5.2], [5.2, 26.2, -3.8], TILE.detail, { rotation: [0, 0, -7] });
      add("Head", "Nose", [-1.2, 24.9, -5.2], [1.2, 27, -3.8], TILE.skin);
      add("Arms", "LeftUpperArm", [-10, 14, -2.5], [-7, 21, 2.5], TILE.secondary); add("Arms", "RightUpperArm", [7, 14, -2.5], [10, 21, 2.5], TILE.secondary);
      add("Arms", "LeftGlove", [-10, 10, -2.5], [-7, 15, 2.5], TILE.cloth); add("Arms", "RightGlove", [7, 10, -2.5], [10, 15, 2.5], TILE.cloth);
      add("Props", "WrenchHandle", [8.6, 5, 2.4], [10, 13, 3.8], TILE.prop); add("Props", "WrenchHead", [7.5, 4, 2.2], [11.1, 7, 4], TILE.metal);
      add("AbilityProps", "VacuumCore", [11, 1, -5], [19, 3.5, 3], TILE.tech, { export: false }); add("AbilityProps", "VacuumFront", [10, 1.2, -3], [12, 3.2, 1], TILE.metal, { export: false }); add("AbilityProps", "VacuumGlow", [13, 3.5, -3], [17, 4, 1], TILE.glow, { export: false });
      add("AbilityProps", "DroneBody", [-18, 21, -2], [-12, 25, 2], TILE.cloth, { export: false }); add("AbilityProps", "DroneLeftWing", [-23, 22, -1], [-18, 24, 1], TILE.metal, { export: false }); add("AbilityProps", "DroneRightWing", [-12, 22, -1], [-7, 24, 1], TILE.metal, { export: false }); add("AbilityProps", "DroneTurret", [-16.5, 18, -1], [-13.5, 21, 1], TILE.tech, { export: false }); add("AbilityProps", "DroneEye", [-16, 22, -2.3], [-14, 24, -1.9], TILE.glow, { export: false });
      add("VFXPreview", "RepairSpark", [11, 16, -5], [12, 17, -4], TILE.glow, { export: false });
    },
  },
  {
    name: "Konvoy", slug: "konvoy", file: "Konvoy",
    palette: [
      [[20, 24, 31], [40, 48, 60], [8, 10, 15], [55, 73, 83]],
      [[34, 44, 52], [55, 69, 78], [17, 22, 28], [22, 126, 142]],
      [[63, 174, 204], [112, 222, 236], [27, 100, 136], [158, 239, 245]],
      [[10, 13, 19], [31, 38, 49], [3, 5, 8], [45, 205, 225]],
      [[89, 105, 116], [167, 184, 192], [42, 53, 64], [211, 233, 235]],
      [[19, 66, 75], [30, 136, 151], [8, 30, 38], [52, 226, 237]],
      [[30, 218, 235], [169, 255, 255], [8, 89, 113], [225, 255, 255]],
      [[62, 46, 44], [107, 76, 65], [28, 24, 26], [178, 122, 82]],
      [[48, 83, 92], [83, 135, 145], [21, 39, 47], [75, 206, 214]],
    ],
    build(add) {
      add("Legs", "LeftBoot", [-5, 0, -3], [-1, 5, 3], TILE.cloth); add("Legs", "RightBoot", [1, 0, -3], [5, 5, 3], TILE.cloth);
      add("Legs", "LeftLeg", [-4.5, 5, -2], [-1, 14, 2], TILE.secondary); add("Legs", "RightLeg", [1, 5, -2], [4.5, 14, 2], TILE.secondary);
      add("Body", "LongTorso", [-5.5, 13, -3], [5.5, 26, 3], TILE.cloth); add("Body", "FrontCoat", [-4.5, 13, -4], [4.5, 24, -3], TILE.secondary);
      add("Body", "HighCollarLeft", [-6, 23, -3], [-2, 29, 3], TILE.cloth); add("Body", "HighCollarRight", [2, 23, -3], [6, 29, 3], TILE.cloth); add("Body", "ChestGlow", [-1, 17, -4.2], [1, 23, -3.7], TILE.glow);
      add("Head", "Head", [-4.5, 25, -4], [4.5, 34, 4], TILE.skin); add("Head", "VisibleEye", [-3.1, 29, -5], [-0.8, 31.2, -3.8], TILE.glow);
      add("Head", "Eyepatch", [0.2, 28.2, -5.1], [4.3, 32.2, -3.7], TILE.detail); add("Head", "EyepatchStrap", [-4.7, 31.4, -4.5], [4.7, 32.5, 3.8], TILE.detail);
      add("Head", "EyepatchKStem", [1.1, 28.8, -5.35], [1.7, 31.7, -5.05], TILE.glow); add("Head", "EyepatchKUpper", [1.6, 30.1, -5.35], [3.1, 30.7, -5.05], TILE.glow, { rotation: [0, 0, -28] }); add("Head", "EyepatchKLower", [1.6, 29.2, -5.35], [3.1, 29.8, -5.05], TILE.glow, { rotation: [0, 0, 28] });
      add("Head", "Grin", [-3.5, 26.2, -5], [3.5, 28.2, -3.9], TILE.detail); for (let i = 0; i < 5; ++i) add("Head", `Tooth${i + 1}`, [-3 + i * 1.25, 26.5, -5.25], [-2.3 + i * 1.25, 27.8, -4.95], TILE.metal);
      add("Arms", "LeftLongArm", [-8.5, 10, -2.3], [-5.5, 24, 2.3], TILE.secondary); add("Arms", "RightLongArm", [5.5, 10, -2.3], [8.5, 24, 2.3], TILE.secondary); add("Arms", "LeftHand", [-8.7, 7, -2.5], [-5.3, 11, 2.5], TILE.skin); add("Arms", "RightHand", [5.3, 7, -2.5], [8.7, 11, 2.5], TILE.skin);
      add("Props", "LeftCuff", [-9, 10, -2.8], [-5, 12, 2.8], TILE.metal); add("Props", "RightCuff", [5, 10, -2.8], [9, 12, 2.8], TILE.metal);
      for (let i = 0; i < 5; ++i) add("AbilityProps", `EnergyTether${i + 1}`, [10 + i * 2, 13 - i, -1], [11.2 + i * 2, 14.2 - i, 1], TILE.glow, { export: false });
      add("AbilityProps", "TrapBase", [-18, 0, -5], [-9, 1.5, 4], TILE.metal, { export: false }); add("AbilityProps", "TrapJawLeft", [-19, 1, -5], [-16, 5, 4], TILE.tech, { export: false }); add("AbilityProps", "TrapJawRight", [-11, 1, -5], [-8, 5, 4], TILE.tech, { export: false });
      add("VFXPreview", "IntruderMark", [10, 25, -1], [13, 28, 1], TILE.glow, { export: false });
    },
  },
  {
    name: "Likho", slug: "likho", file: "Likho",
    palette: [
      [[41, 142, 37], [75, 208, 59], [18, 74, 22], [112, 238, 74]],
      [[31, 108, 31], [55, 167, 41], [12, 54, 18], [83, 209, 54]],
      [[166, 241, 105], [218, 255, 166], [75, 155, 51], [244, 255, 198]],
      [[8, 15, 12], [23, 35, 25], [2, 5, 4], [51, 111, 45]],
      [[40, 78, 43], [75, 120, 70], [17, 39, 22], [108, 172, 83]],
      [[22, 79, 27], [43, 145, 39], [8, 37, 15], [82, 224, 55]],
      [[119, 255, 79], [218, 255, 177], [42, 153, 34], [239, 255, 199]],
      [[13, 31, 17], [33, 68, 35], [4, 13, 8], [53, 128, 51]],
      [[66, 128, 62], [101, 174, 91], [30, 70, 35], [130, 207, 107]],
    ],
    build(add) {
      const rows = [[-10, 10, 5, 9], [-9, 9, 9, 13], [-8, 8, 13, 17], [-6.5, 6.5, 17, 21], [-5, 5, 21, 25], [-3, 3, 25, 29], [-1.5, 1.5, 29, 32]];
      rows.forEach(([x0, x1, y0, y1], index) => add("Body", `TriangleRow${index + 1}`, [x0, y0, -1.5], [x1, y1, 1.5], index % 2 ? TILE.secondary : TILE.cloth));
      add("Head", "SingleEyeBlack", [-3.4, 18, -2.2], [3.4, 23, -1.4], TILE.detail); add("Head", "SingleEyeWhite", [-2.2, 19, -2.5], [2.2, 22, -2.15], TILE.skin); add("Head", "SinglePupil", [-0.8, 19.3, -2.8], [0.8, 21.7, -2.45], TILE.detail);
      add("Head", "CunningMouth", [-2.8, 15.5, -2.2], [2.8, 17, -1.45], TILE.detail); add("Head", "LeftFang", [-1.8, 15.2, -2.5], [-0.7, 16.6, -2.15], TILE.skin); add("Head", "RightFang", [0.7, 15.2, -2.5], [1.8, 16.6, -2.15], TILE.skin);
      add("Props", "LeftCorner", [-11, 5, -1], [-9, 8, 1], TILE.glow); add("Props", "RightCorner", [9, 5, -1], [11, 8, 1], TILE.glow);
      add("AbilityProps", "GhostTrailNear", [12, 6, 1], [19, 24, 2], TILE.ghost, { export: false }); add("AbilityProps", "GhostTrailFar", [21, 9, 2], [26, 21, 3], TILE.ghost, { export: false });
      add("VFXPreview", "DissolvePixel1", [-13, 23, 0], [-11, 25, 2], TILE.glow, { export: false }); add("VFXPreview", "DissolvePixel2", [12, 14, 0], [13, 15, 1], TILE.glow, { export: false });
    },
  },
  {
    name: "Witness", slug: "witness", file: "Witness",
    palette: [
      [[19, 17, 25], [38, 33, 48], [7, 6, 10], [58, 42, 73]],
      [[38, 28, 52], [67, 48, 85], [16, 12, 25], [91, 53, 117]],
      [[23, 20, 29], [48, 40, 58], [5, 5, 8], [74, 47, 89]],
      [[8, 7, 12], [24, 19, 31], [1, 1, 3], [48, 28, 61]],
      [[89, 81, 98], [163, 148, 170], [42, 37, 51], [202, 174, 199]],
      [[75, 29, 99], [132, 56, 168], [30, 12, 48], [189, 76, 218]],
      [[212, 37, 62], [255, 124, 131], [103, 9, 32], [255, 197, 173]],
      [[73, 45, 37], [123, 77, 55], [31, 22, 23], [175, 109, 66]],
      [[90, 75, 111], [135, 117, 158], [42, 34, 57], [169, 113, 190]],
    ],
    build(add) {
      add("Legs", "TentacleLeftBack", [-6, 0, 0], [-3, 11, 3], TILE.secondary, { rotation: [0, 0, -10] }); add("Legs", "TentacleLeftFront", [-4, 0, -4], [-1, 12, -1], TILE.tech, { rotation: [0, 0, 7] }); add("Legs", "TentacleCenter", [-1.5, -1, -2], [1.5, 12, 2], TILE.secondary); add("Legs", "TentacleRightFront", [1, 0, -4], [4, 12, -1], TILE.tech, { rotation: [0, 0, -7] }); add("Legs", "TentacleRightBack", [3, 0, 0], [6, 11, 3], TILE.secondary, { rotation: [0, 0, 10] });
      add("Body", "FloatingRobe", [-6, 9, -3.5], [6, 25, 3.5], TILE.cloth); add("Body", "RobeFront", [-5, 10, -4.2], [5, 23, -3.4], TILE.secondary); add("Body", "RobeSigil", [-1.5, 15, -4.5], [1.5, 20, -4.15], TILE.tech);
      add("Head", "ShadowFace", [-4, 24, -4], [4, 32, 4], TILE.detail); add("Head", "HoodTop", [-5.5, 29, -4.5], [5.5, 35, 4.5], TILE.cloth); add("Head", "HoodLeft", [-6, 24, -4.5], [-3.5, 32, 4.5], TILE.cloth); add("Head", "HoodRight", [3.5, 24, -4.5], [6, 32, 4.5], TILE.cloth); add("Head", "HoodBrow", [-5, 28, -5], [5, 31, -4], TILE.cloth);
      add("Head", "RedMonocle", [-3, 27, -5.3], [0, 30, -4.4], TILE.glow); add("Head", "MonoclePupil", [-2.2, 27.7, -5.55], [-0.8, 29.3, -5.25], TILE.detail); add("Head", "MonocleStem", [-1.8, 23, -5.2], [-1.1, 27.2, -4.7], TILE.metal);
      add("Arms", "LeftSleeve", [-9, 12, -3], [-6, 24, 3], TILE.cloth, { rotation: [0, 0, -8] }); add("Arms", "RightSleeve", [6, 12, -3], [9, 24, 3], TILE.cloth, { rotation: [0, 0, 8] }); add("Arms", "LeftHand", [-8.5, 9, -2], [-5.5, 14, 2], TILE.secondary); add("Arms", "RightHand", [5.5, 9, -2], [8.5, 14, 2], TILE.secondary);
      add("Props", "BlunderbussStock", [6, 10, 2], [10, 14, 5], TILE.prop); add("Props", "BlunderbussBarrel", [8, 12, 2.5], [20, 15, 4.5], TILE.metal); add("Props", "BlunderbussMuzzle", [19, 11, 2], [23, 16, 5], TILE.metal); add("Props", "BlunderbussGlow", [12, 12.5, 2], [15, 14.5, 2.5], TILE.glow);
      add("AbilityProps", "EchoBody", [-19, 9, -2], [-13, 23, 2], TILE.ghost, { export: false }); add("AbilityProps", "EchoHood", [-20, 22, -3], [-12, 30, 3], TILE.ghost, { export: false }); add("AbilityProps", "EchoLens", [-17.5, 25, -3.4], [-15, 27.5, -3], TILE.glow, { export: false });
      add("VFXPreview", "EchoMarker", [-17, 2, -1], [-15, 5, 1], TILE.tech, { export: false }); add("VFXPreview", "PhantomBlock", [13, 3, -3], [18, 8, 2], TILE.ghost, { export: false });
    },
  },
];

for (const hero of heroes) {
  const { model, png } = createModel(hero.name, hero.slug, hero.palette, hero.build);
  const directory = resolve("assets", "heroes", hero.slug);
  await mkdir(directory, { recursive: true });
  await writeFile(resolve(directory, `${hero.file}.bbmodel`), `${JSON.stringify(model, null, 2)}\n`, "utf8");
  await writeFile(resolve(directory, `${hero.file}.png`), png);
  console.log(`${hero.name}: ${model.elements.length} elements`);
}
