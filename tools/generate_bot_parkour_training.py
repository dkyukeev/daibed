#!/usr/bin/env python3
"""Generate symmetric procedural parkour curricula and sealed holdouts."""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

LANES = (-52, -44, -36, -28, -20, -12, 12, 20, 28, 36, 44, 52)


def generate_map(output: Path, seed: int, split: str) -> dict[str, object]:
    rng = random.Random(seed)
    blocks: dict[tuple[int, int, int], tuple[str, int, int]] = {}
    specials: list[tuple[str, int, int, int, int]] = []
    nodes: list[tuple[str, int, int, int, int, str]] = []
    edges: list[tuple[str, str, float, int, str]] = []

    def block(x: int, y: int, z: int, kind: str = "StoneBlock", team: int = -1, breakable: int = 0) -> None:
        blocks[(x, y, z)] = (kind, team, breakable)

    def platform(min_x: int, max_x: int, min_z: int, max_z: int, y: int, kind: str = "StoneBlock") -> None:
        for x in range(min_x, max_x + 1):
            for z in range(min_z, max_z + 1):
                block(x, y, z, kind)

    def add_team(team: int, x: int, z: int, facing: int) -> None:
        platform(x - 10, x + 10, z - 8, z + 8, 0)
        specials.extend([
            ("spawn", x, 1, z, team), ("core", x - facing * 6, 1, z, team),
            ("chest", x - facing * 3, 1, z - 3, team), ("shop", x, 1, z - 5, team),
            ("gen_iron", x + facing * 3, 1, z + 3, team), ("gen_gold", x, 1, z + 5, team),
        ])

    add_team(0, -82, -32, 1); add_team(1, 82, -32, -1)
    add_team(2, -82, 32, 1); add_team(3, 82, 32, -1)
    gap_order = [1, 2, 3]
    rng.shuffle(gap_order)
    run_lengths = [rng.randint(6, 9) for _ in range(3)]
    anchor_sides: list[int] = []

    for lane_index, z in enumerate(LANES):
        lane_rng = random.Random(seed ^ (lane_index * 0x9E3779B9))
        lane_gaps = gap_order[:] if lane_index % 2 == 0 else list(reversed(gap_order))
        lane_runs = [max(5, length + lane_rng.choice((-1, 0, 1))) for length in run_lengths]
        cursor = -72
        ranges: list[tuple[int, int]] = []
        for run_length, gap in zip(lane_runs, lane_gaps):
            end = min(-38, cursor + run_length - 1)
            ranges.append((cursor, end))
            cursor = end + gap + 1
        ranges.append((cursor, -30))
        for min_x, max_x in ranges:
            if min_x <= max_x:
                platform(min_x, max_x, z - 1, z + 1, 0, "SmoothStoneBlock")
                platform(-max_x, -min_x, z - 1, z + 1, 0, "SmoothStoneBlock")

        # The long void is always exactly x=-15..14; only the approach changes.
        platform(-17, -16, z - 1, z + 1, 0, "SmoothStoneBlock")
        platform(15, 16, z - 1, z + 1, 0, "SmoothStoneBlock")
        anchor_side = lane_rng.choice((-1, 1))
        anchor_sides.append(anchor_side)
        anchor_z = z + anchor_side * 2
        for step in range(1, 5):
            block(-30 + step, step, anchor_z); block(30 - step, step, anchor_z)
        platform(-25, -21, z - 1, z + 1, 4); platform(21, 25, z - 1, z + 1, 4)
        for offset, height in enumerate((3, 2, 1, 0)):
            platform(-20 + offset, -20 + offset, z - 1, z + 1, height)
            platform(20 - offset, 20 - offset, z - 1, z + 1, height)

        prefix = f"lane{lane_index:02d}"
        lane_nodes = [
            (f"{prefix}_left_base", -70, 1, z, -1, "lane"),
            (f"{prefix}_left_ramp", -30, 1, z, -1, "lane"),
            (f"{prefix}_bridge_start", -16, 1, z, -1, "bridge_start"),
            (f"{prefix}_bridge_land", 15, 1, z, -1, "bridge_land"),
            (f"{prefix}_right_ramp", 30, 1, z, -1, "lane"),
            (f"{prefix}_right_base", 70, 1, z, -1, "lane"),
        ]
        nodes.extend(lane_nodes)
        ids = [entry[0] for entry in lane_nodes]
        edges.extend([(ids[0], ids[1], 1.0, 1, "main"), (ids[1], ids[2], 1.0, 1, "main"),
                      (ids[2], ids[3], 1.0, 1, "bridge"), (ids[3], ids[4], 1.0, 1, "main"),
                      (ids[4], ids[5], 1.0, 1, "main")])
        specials.extend([("gen_crystal", -35, 1, z, -1), ("gen_crystal", 35, 1, z, -1)])

    biome = seed % 5
    lines = ["daibedmap 1", f"biome {biome}", "layout 0", "collapse 12"]
    lines.extend(f"special {kind} {x} {y} {z} {team}" for kind, x, y, z, team in specials)
    lines.extend(f"route_node {name} {x} {y} {z} {team} {kind}" for name, x, y, z, team, kind in nodes)
    lines.extend(f"route_edge {source} {target} {cost:g} {bidirectional} {tag}" for source, target, cost, bidirectional, tag in edges)
    lines.extend(f"block {x} {y} {z} {kind} {team} {breakable} 0"
                 for (x, y, z), (kind, team, breakable) in sorted(blocks.items()))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"file": str(output), "split": split, "seed": seed, "biome": biome,
            "gapOrder": gap_order, "runLengths": run_lengths, "anchorSides": anchor_sides,
            "lanes": len(LANES), "blocks": len(blocks)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="maps/bot_parkour.dbmap")
    parser.add_argument("--seed", type=int, default=41001)
    parser.add_argument("--suite-dir", default="")
    parser.add_argument("--train-count", type=int, default=6)
    parser.add_argument("--holdout-count", type=int, default=3)
    args = parser.parse_args()
    if not args.suite_dir:
        manifest = [generate_map(Path(args.output), args.seed, "single")]
    else:
        suite = Path(args.suite_dir)
        manifest = [generate_map(suite / "train" / f"parkour_train_{i:02d}.dbmap",
                                 args.seed + i * 7919, "train") for i in range(max(1, args.train_count))]
        manifest += [generate_map(suite / "holdout" / f"parkour_holdout_{i:02d}.dbmap",
                                  args.seed ^ 0xA5A5A5A5 ^ (i * 104729), "holdout")
                     for i in range(max(1, args.holdout_count))]
        (suite / "manifest.json").write_text(json.dumps({"version": 1, "maps": manifest}, indent=2), encoding="utf-8")
    print(f"generated {len(manifest)} map(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
