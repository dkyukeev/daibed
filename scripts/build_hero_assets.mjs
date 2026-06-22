#!/usr/bin/env node

import { convertBbmodel } from "./convert_bbmodel_to_glb.mjs";

const sources = [
  "assets/heroes/radon/Radon.bbmodel",
  "assets/heroes/orbita/Orbita.bbmodel",
  "assets/heroes/brom/Brom.bbmodel",
  "assets/heroes/konvoy/Konvoy.bbmodel",
  "assets/heroes/likho/Likho.bbmodel",
  "assets/heroes/witness/Witness.bbmodel",
];

for (const source of sources) {
  const result = await convertBbmodel(source);
  console.log(`${source} -> ${result.outputPath} (${result.triangles} triangles)`);
}
