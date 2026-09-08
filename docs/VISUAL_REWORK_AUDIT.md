# DaiBed visual rework: baseline and visual bible

Status: stage 1 complete, stage 2 started. This document describes the working
tree as measured on 2026-09-03. The checkout already contained unrelated local
changes, so the baseline intentionally represents that exact working tree and
not a clean revision.

## Stage 1 goal and completion criteria

Goal: establish a reproducible visual baseline and a small set of rules that
can guide later renderer, HUD, first-person-motion, VFX, material, and shadow
work without changing authoritative gameplay.

The stage is complete when:

- fixed camera views exist for the current build;
- graphics settings and test hardware are recorded;
- duplicated or conflicting visual language is identified by subsystem;
- semantic color, timing, emissive, and screen-effect tokens have one owner;
- the next implementation slice has explicit visual and technical boundaries.

## Reproducible baseline

Build and capture commands:

```powershell
cmake -S . -B build/visual-rework `
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB=build-release/_deps/raylib-src `
  -DDAIBED_DIAGNOSTICS=ON
cmake --build build/visual-rework --config Release --parallel 4

Push-Location build/visual-rework/Release
./DaiBed.exe --map-review --map ./maps/castle_bedwars.dbmap
./DaiBed.exe --frame-profile
Pop-Location
```

The eight 1280x720 baseline views are currently build artifacts at:

```text
build/visual-rework/Release/map_review_1.png
build/visual-rework/Release/map_review_2.png
build/visual-rework/Release/map_review_3.png
build/visual-rework/Release/map_review_4.png
build/visual-rework/Release/map_review_5.png
build/visual-rework/Release/map_review_6.png
build/visual-rework/Release/map_review_7.png
build/visual-rework/Release/map_review_8.png
```

`map_review_1..4` cover the spawn/base anchor and `map_review_5..8` the elevated
map-center anchor, each at four cardinal headings. Keep this camera setup for
the matching after-capture.

Capture settings: 1280x720, FOV 62, render scale 100%, draw distance 150 m,
Medium shadows, High effects, post-processing on, bloom on, VSYNC on, full
camera shake, full flashes, Castle biome. The capture used the Castle Bedwars
map, Radon, and a 1v1v1v1 setup.

Hardware reported by OpenGL: NVIDIA GeForce RTX 5070, driver 581.42, OpenGL
3.3. The baseline frame profile ran menu idle for 3 seconds, match start for 6
seconds, and an established match for 8 seconds:

- 989 frames; worst frame 460.3 ms; 3 frames over 25 ms;
- first menu frame 97.9 ms and one later menu frame 30.4 ms;
- first match frame 460.3 ms, of which render was 454.0 ms;
- simulation average 0.21 ms/tick over 816 ticks, including bots 0.16 ms/tick;
- 6 full path searches in 816 ticks, 0.7 ms total path-search time.

The 460 ms match-start spike is dominated by initial render-side world mesh/GPU
work. This is a baseline observation, not a steady-state GPU guarantee. The
existing profiler does not export GPU timestamps or draw-call aggregates; the
F8 overlay only exposes the current chunk draw calls. Adding machine-readable
render counters is a later diagnostics task.

## Asset and subsystem inventory

- 6 hero source/model/texture sets (`bbmodel`, `glb`, `png`) with a procedural
  fallback when a runtime model is missing;
- 47 block kinds, mostly backed by procedurally generated 32x32 albedo textures;
- normal maps generated or loaded per block, with a neutral flat-map fallback;
- only 4 authored item PNGs; most equipment icons/models use procedural drawing
  or share another item's texture;
- 5 GLSL 330 shader files: scene vertex/fragment, post, bloom extraction, and
  bloom blur;
- one fixed-capacity 512-particle CPU pool and a separate `WorldEffect` path;
- a single stabilized sun shadow map (1024 Medium, 2048 High), PCF quality
  switch, and no sun map on Low;
- reusable scene/bloom render targets that are recreated only on size/scale
  changes;
- camera support for first/third person, crouch blend, step smoothing, shake,
  sprint FOV, and sniper FOV, but no general first-person motion layer yet.

## Visual audit

### Readability and palette

- Fog and the dark-gray Castle palette compress distant values strongly. The
  center island silhouette remains visible, but team/objective information
  becomes weak before geometry reaches the draw-distance fade.
- Gameplay green (team, healing, Brom, Likho) overlaps natural foliage green.
  Cyan is simultaneously used for energy, shield, friendly/device relation,
  crystal, and generic UI focus. Red is used by team identity, damage, danger,
  invalid placement, and Orbita. Color alone therefore cannot carry state.
- Hero accent values were copied verbatim into Game, world tick, abilities,
  renderer, and menu code. Common energy, danger, objective, and healing
  literals occur 10-26 times in core visual files.
- Team markers already use distinct circle/square/triangle/ring shapes in the
  minimap. That shape language should be reused for world-space objective and
  relation markers instead of inventing a second convention.

### HUD and first-person view

- The lower-left message panel, centered hotbar/health bar, and lower-right
  ability panel occupy overlapping horizontal bands. At 1280x720 they obscure
  a large part of the traversable edge and compete with the weapon silhouette.
- The health bar is a full-width red strip, so a persistent self-status element
  uses the same strongest signal as immediate enemy damage.
- Weapon/item world, icon, and first-person representations are inconsistent:
  some use authored PNGs, several ranged weapons share the arrow icon, and the
  viewmodel is positioned directly in `Renderer::RenderScene`.
- Camera shake moves both position and target, while the viewmodel uses the
  shaken camera basis. There is no separate spring state for camera and hands,
  no walk/sprint bob, mouse sway, strafe inertia, jump apex, landing impulse,
  or item-switch transition.
- Sprint and scope FOV are already presentation-only and smoothed. They are a
  safe integration point for the later Off/Reduced/Full motion setting.

### VFX and particles

- `ParticleSystem` avoids per-particle heap allocation and has Low/Medium/High
  caps, but new particles overwrite ring-buffer slots without priority. A dust
  burst can therefore replace an important hit or ability cue.
- Emitters provide only kind, direction, color, count, and speed. Lifetime,
  size, drag, opacity, and gravity are hard-coded by broad particle kind, so
  different surfaces and gameplay priorities cannot form stable profiles.
- There is no camera-distance culling/LOD, source-velocity inheritance,
  collision policy, reserved critical budget, or burst-rate guard for chain
  destruction.
- `WorldEffect` and `ParticleSystem` independently represent burst/trail/fire
  events. This is the main duplication boundary to resolve, while keeping
  simulation events authoritative and rendering client-only.

### Materials, lighting, and post-processing

- The scene shader already performs sRGB-to-linear lighting, wrapped sun,
  baked vertex AO, normal-map fallback, limited point lights, material-mask
  gloss, fog, and a shadow fallback. This pipeline should be extended rather
  than replaced.
- Emissive behavior is inferred from albedo brightness/saturation. It can make
  unrelated bright colored blocks glow and cannot express the hierarchy
  objective > active threat > ability > decoration explicitly.
- Material properties are encoded indirectly in normal-map alpha and scattered
  texture-generation numbers. There is no named material profile for wool,
  stone, metal, energy, glass, character, weapon, or interactive object.
- Bloom, vignette, and damage-flash strengths were local constants. Shader load
  failures already fall back safely to ordinary scene rendering or texture
  copy, and render targets have symmetric teardown.

### Shadows and performance

- The sun projection is snapped to a light-space texel grid, which is a good
  anti-shimmering foundation.
- High currently increases resolution, radius, and PCF samples, but is still a
  single shadow map; it has no cascade blend or separate near-field precision.
- Player blob/contact shadows are drawn only when shadow quality is above Low,
  while the target visual rules call for blob/contact shadows specifically as
  the Low fallback.
- Up to 12 torch point lights are selected by camera distance only. Screen
  influence, gameplay importance, and update frequency are not represented.
- Initial match rendering has a large measured one-time spike. Chunk rebuild
  timing exists, but frame-profile output needs draw-call and GPU timing data
  before material/shadow cost can be compared reliably.

## Visual bible v0.1

1. World geometry stays matte and value-readable. Saturated emissive is reserved
   for objective, current threat, active ability, and energy—in that order.
2. Relation and team identity use at least two channels: color plus the existing
   team shape, an outline, a direction, or a distinct pulse rhythm.
3. Red means enemy, incoming damage, invalid action, or imminent danger. It is
   not neutral decoration. Hero-specific red/orange must keep a unique shape or
   rhythm so it cannot be mistaken for generic damage.
4. Cyan means shield/energy only when shape distinguishes them. UI focus uses a
   quieter cyan and never outshines the crosshair or an immediate warning.
5. Objective gold is brighter than passive interactables but below a critical
   danger flash. Healing green must be lighter and less foliage-like than team
   green.
6. The crosshair and direct hit/telegraph information own the center. Persistent
   status belongs near the edges and may not cover a platform edge.
7. Camera motion stays subtle; most animation amplitude belongs to the hands and
   held item. Presentation offsets never modify aim, physics, or network state.
8. Small/medium/large VFX share timing and density families. Gameplay-critical
   effects have reserved budget and background dust is discarded first.
9. Surface hits share event semantics but vary through a material profile, not
   independent hard-coded emit calls.
10. Low/Medium/High change fidelity, density, and distance—not the presence of
    gameplay information.

## Token ownership

`src/VisualTheme.h` owns the deliberately small cross-system vocabulary:

- team identities and hero accents;
- danger, healing, shield, energy, objective, resource, panel, and text colors;
- standard flash/trail/impact/dissolve durations;
- named emissive hierarchy levels;
- bloom, vignette, and full/reduced damage-flash strengths.

Material parameters remain with the renderer/scene shader; emitter physics and
budgets remain with `ParticleSystem`; first-person spring tuning will remain
with the new camera/viewmodel presentation component. This avoids turning the
theme into a global bag of unrelated constants.

## Current validation

The first token-only implementation slice was rebuilt in Release and passed:

- `--startup-smoke`;
- `--ui-screenshot` (menu, hero select, settings, controls);
- `--client-gui-smoke`;
- `--protocol-smoke`;
- `--network-actions-smoke`;
- `--network-ranged-smoke`;
- `--client-input-smoke`;
- `--client-dynamic-apply-smoke`;
- `--movement-parity-smoke`.

The generated UI review images are in
`build/visual-rework/Release/ui_*.png`. Shader compilation succeeded for the
scene, post, bloom, and blur programs on the baseline OpenGL 3.3 device.

## Next vertical slice

Stage 2 should add explicit `MaterialProfile` and `VfxEventProfile` data, then
route one complete interaction through them: hard-block hit, wool hit, character
hit, shield hit, block placement, and block destruction. Acceptance criteria:

- the six events are distinguishable on light, dark, and team-colored blocks;
- hero/team/semantic colors come only from the shared theme;
- important impacts cannot be displaced by dust;
- headless execution produces no presentation particles;
- the existing startup, client GUI, network action, ranged, and dynamic-apply
  smoke tests remain green.
