#!/usr/bin/env python3
"""Convert a legacy Minecraft Java 1.12 Anvil world into a DaiBed map.

The converter deliberately has no third-party dependencies.  It reads either
the supplied ZIP archive directly (without extracting it), a world directory,
or an individual ``.mca`` file.  Minecraft 1.12 stores a section's IDs in the
legacy ``Blocks`` byte array, its metadata in the ``Data`` nibble array, and
optionally its high ID bits in ``Add``.  This tool implements just enough NBT
and Anvil decoding to read that format safely.

Examples (run from the repository root):

    # Inspect the supplied archive only; this does not create a map file.
    python tools/convert_minecraft_world_to_dbmap.py --report

    # Generate the built-in Castle Bedwars map and its checked default specials.
    python tools/convert_minecraft_world_to_dbmap.py --output maps/castle_bedwars.dbmap

    # Convert another legacy world as geometry only.
    python tools/convert_minecraft_world_to_dbmap.py other-world.zip --no-castle-specials

Every generated geometry line uses the extended dbmap form
``block x y z Token -1 0 variant`` by default.  ``variant`` is the original
Minecraft ``Data`` nibble, which keeps orientation/dye information available to
the renderer while all imported geometry remains neutral and unbreakable.

``--specials`` is an optional UTF-8 file containing already-normalized
``special <kind> <x> <y> <z> <team>`` lines.  Keeping gameplay points separate
from geometry avoids guessing base locations from a decorative Minecraft map.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import gzip
import struct
import sys
import zipfile
import zlib
from pathlib import Path
from typing import Any, Iterable, Iterator


SECTOR_BYTES = 4096
HEADER_BYTES = SECTOR_BYTES * 2
SECTION_VOLUME = 16 * 16 * 16
NIBBLE_BYTES = SECTION_VOLUME // 2

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = Path.home() / "Downloads" / "bedwars-castle-1571798587.zip"
DEFAULT_OUTPUT = REPOSITORY_ROOT / "maps" / "castle_bedwars.dbmap"

# Hand-checked coordinates for the supplied Bedwars_castle archive after its
# default normalization (X -= -1312, Y -= 64, Z -= -284).  The four crystal
# The crystal points sit one block above the reachable upper central floor: its
# floor is at y=60 and the generator markers live at y=61. The lower y=53
# chamber is valid geometry but is not connected to the routes bots and players
# naturally use. Imported Minecraft geometry itself remains neutral; teams are
# represented only by these gameplay specials.
DEFAULT_CASTLE_SPECIALS = (
    # team 0: red / north castle
    "special core 2 53 -93 0",
    "special spawn 2 53 -97 0",
    "special chest 1 53 -95 0",
    "special shop 6 53 -95 0",
    "special gen_iron 4 53 -94 0",
    "special gen_gold 5 53 -96 0",
    # team 1: blue / west castle
    "special core -93 53 -2 1",
    "special spawn -97 53 -2 1",
    "special chest -96 53 -6 1",
    "special shop -84 53 -4 1",
    "special gen_iron -94 53 -2 1",
    "special gen_gold -81 53 6 1",
    # team 2: lime / east castle
    "special core 93 53 2 2",
    "special spawn 97 53 2 2",
    "special chest 81 53 -6 2",
    "special shop 101 53 -6 2",
    "special gen_iron 85 53 3 2",
    "special gen_gold 96 53 6 2",
    # team 3: yellow / south castle
    "special core -2 53 93 3",
    "special spawn -2 53 97 3",
    "special chest -11 53 88 3",
    "special shop 14 53 90 3",
    "special gen_iron -6 53 95 3",
    "special gen_gold 7 53 101 3",
    # neutral central economy, above the centre platform
    "special gen_crystal -2 61 -1 -1",
    "special gen_crystal -2 61 1 -1",
    "special gen_crystal 2 61 -1 -1",
    "special gen_crystal 2 61 1 -1",
)


class ConversionError(RuntimeError):
    """The source world is not a supported, readable legacy Anvil world."""


class NbtReader:
    """Small bounds-checked reader for the classic big-endian NBT format."""

    TAG_END = 0
    TAG_BYTE = 1
    TAG_SHORT = 2
    TAG_INT = 3
    TAG_LONG = 4
    TAG_FLOAT = 5
    TAG_DOUBLE = 6
    TAG_BYTE_ARRAY = 7
    TAG_STRING = 8
    TAG_LIST = 9
    TAG_COMPOUND = 10
    TAG_INT_ARRAY = 11
    TAG_LONG_ARRAY = 12

    def __init__(self, data: bytes) -> None:
        self._data = memoryview(data)
        self._offset = 0

    def _take(self, count: int) -> memoryview:
        if count < 0 or self._offset + count > len(self._data):
            raise ConversionError("truncated NBT payload")
        result = self._data[self._offset:self._offset + count]
        self._offset += count
        return result

    def _unpack(self, format_string: str) -> Any:
        size = struct.calcsize(format_string)
        value = struct.unpack_from(format_string, self._data, self._offset)[0]
        self._offset += size
        return value

    def _count(self) -> int:
        count = self._unpack(">i")
        if count < 0:
            raise ConversionError("negative NBT array length")
        return count

    def _string(self) -> str:
        length = self._unpack(">H")
        # NBT strings are modified UTF-8.  Minecraft's field names in this
        # legacy schema are ASCII; surrogateescape still preserves any unusual
        # value without corrupting byte alignment.
        return bytes(self._take(length)).decode("utf-8", errors="surrogateescape")

    def read_root(self) -> dict[str, Any]:
        tag_type = self._unpack(">B")
        if tag_type != self.TAG_COMPOUND:
            raise ConversionError("NBT root is not a compound")
        self._string()  # Root name is normally empty and is not useful here.
        root = self._payload(tag_type, 0)
        if not isinstance(root, dict):
            raise ConversionError("NBT root compound could not be decoded")
        return root

    def _payload(self, tag_type: int, depth: int) -> Any:
        if depth > 64:
            raise ConversionError("NBT nesting is too deep")

        if tag_type == self.TAG_BYTE:
            return self._unpack(">b")
        if tag_type == self.TAG_SHORT:
            return self._unpack(">h")
        if tag_type == self.TAG_INT:
            return self._unpack(">i")
        if tag_type == self.TAG_LONG:
            return self._unpack(">q")
        if tag_type == self.TAG_FLOAT:
            return self._unpack(">f")
        if tag_type == self.TAG_DOUBLE:
            return self._unpack(">d")
        if tag_type == self.TAG_BYTE_ARRAY:
            return bytes(self._take(self._count()))
        if tag_type == self.TAG_STRING:
            return self._string()
        if tag_type == self.TAG_LIST:
            item_type = self._unpack(">B")
            count = self._unpack(">i")
            # The NBT specification treats negative list lengths as empty.
            if count <= 0:
                return []
            if count > 1_000_000:
                raise ConversionError("unreasonably large NBT list")
            if item_type == self.TAG_END:
                raise ConversionError("NBT list has TAG_End elements")
            return [self._payload(item_type, depth + 1) for _ in range(count)]
        if tag_type == self.TAG_COMPOUND:
            result: dict[str, Any] = {}
            while True:
                child_type = self._unpack(">B")
                if child_type == self.TAG_END:
                    return result
                name = self._string()
                result[name] = self._payload(child_type, depth + 1)
        if tag_type == self.TAG_INT_ARRAY:
            # HeightMap and similar fields are irrelevant to conversion.  Skip
            # their payload exactly rather than allocating them for every chunk.
            self._take(self._count() * 4)
            return None
        if tag_type == self.TAG_LONG_ARRAY:
            self._take(self._count() * 8)
            return None

        raise ConversionError(f"unsupported NBT tag type {tag_type}")


@dataclasses.dataclass(frozen=True)
class BlockMapping:
    token: str


@dataclasses.dataclass
class ConvertedBlock:
    x: int
    y: int
    z: int
    token: str
    team_id: int
    breakable: bool
    variant: int


@dataclasses.dataclass
class ConversionStats:
    region_files: int = 0
    chunks: int = 0
    sections: int = 0
    non_air_blocks: int = 0
    output_blocks: int = 0
    skipped_blocks: int = 0
    replaced_blocks: int = 0
    source_variants: collections.Counter[tuple[int, int]] = dataclasses.field(
        default_factory=collections.Counter
    )
    output_tokens: collections.Counter[str] = dataclasses.field(
        default_factory=collections.Counter
    )
    unsupported_variants: collections.Counter[tuple[int, int]] = dataclasses.field(
        default_factory=collections.Counter
    )
    compression_types: collections.Counter[int] = dataclasses.field(
        default_factory=collections.Counter
    )
    min_x: int | None = None
    max_x: int | None = None
    min_y: int | None = None
    max_y: int | None = None
    min_z: int | None = None
    max_z: int | None = None

    def include_position(self, x: int, y: int, z: int) -> None:
        self.min_x = x if self.min_x is None else min(self.min_x, x)
        self.max_x = x if self.max_x is None else max(self.max_x, x)
        self.min_y = y if self.min_y is None else min(self.min_y, y)
        self.max_y = y if self.max_y is None else max(self.max_y, y)
        self.min_z = z if self.min_z is None else min(self.min_z, z)
        self.max_z = z if self.max_z is None else max(self.max_z, z)

    @property
    def source_bounds(self) -> tuple[int, int, int, int, int, int]:
        if None in (self.min_x, self.max_x, self.min_y, self.max_y, self.min_z, self.max_z):
            raise ConversionError("world contains no non-air blocks")
        return (
            self.min_x,
            self.max_x,
            self.min_y,
            self.max_y,
            self.min_z,
            self.max_z,
        )


def map_minecraft_block(block_id: int, data: int) -> BlockMapping | None:
    """Map a legacy Minecraft numeric block ID/metadata pair to a dbmap token.

    Stairs, slabs, torches and ladders carry orientation in metadata.  The
    current dbmap grammar has no metadata field, so this function deliberately
    maps their material/shape family and discards orientation.  That is visible
    in the report and can be extended later without changing Anvil decoding.
    """

    variant = data & 0x0F

    # Stone family.  In Minecraft 1.12, andesite is stone metadata 5 and
    # polished andesite is metadata 6.
    if block_id == 1:
        if variant == 5:
            return BlockMapping("AndesiteBlock")
        if variant == 6:
            return BlockMapping("PolishedAndesiteBlock")
        return BlockMapping("StoneBlock")
    if block_id == 2:
        return BlockMapping("GrassBlock")
    if block_id == 3:
        return BlockMapping("DirtBlock")
    if block_id == 4:
        return BlockMapping("CobblestoneBlock")

    # Wood / vegetation.
    if block_id == 5:  # planks; metadata 2 = birch
        return BlockMapping("BirchPlankBlock" if (variant & 0x07) == 2 else "WoodBlock")
    if block_id in (17, 162):  # log, log2
        return BlockMapping("WoodBlock")
    if block_id in (18, 161):  # leaves, leaves2
        return BlockMapping("LeafBlock")
    if block_id == 85:  # fence: preserve its material rather than drop a railing
        return BlockMapping("WoodBlock")

    # The original dye is retained in the dbmap variant field.  Static map
    # geometry intentionally stays team-neutral: team ownership belongs to the
    # explicitly authored Core/Spawn/Chest/Shop specials, not to decoration.
    if block_id == 35:  # wool
        return BlockMapping("WoolBlock")
    if block_id in (125, 126):  # double wooden slab / wooden slab
        return BlockMapping("BirchSlabBlock" if (variant & 0x07) == 2 else "WoodBlock")
    if block_id == 135:  # birch stairs
        return BlockMapping("BirchStairsBlock")

    # Legacy stone slab metadata uses bit 3 for upper/lower placement.
    if block_id in (43, 44):  # double stone slab / stone slab
        return BlockMapping("StoneBrickSlabBlock" if (variant & 0x07) == 5 else "StoneSlabBlock")
    if block_id == 98:  # stone brick: 3 = chiseled; cracked/mossy use brick
        return BlockMapping("ChiseledStoneBrickBlock" if variant == 3 else "StoneBrickBlock")
    if block_id == 109:
        return BlockMapping("StoneBrickStairsBlock")

    # Glass and terracotta.  The original dye remains in the final dbmap
    # variant field, so one generic material token correctly supports all
    # Minecraft 1.12 colours without adding a BlockType for each one.
    if block_id == 20:
        return BlockMapping("EnergyGlassBlock")
    if block_id == 95:  # stained glass
        return BlockMapping("ColoredGlassBlock")
    if block_id == 159:  # stained hardened clay
        return BlockMapping("ColoredClayBlock")
    if block_id == 172:  # plain hardened clay
        return BlockMapping("ColoredClayBlock")

    # Resources / castle fixtures.
    if block_id == 22:
        return BlockMapping("LapisBlock")
    if block_id == 41:
        return BlockMapping("GoldBlock")
    if block_id == 42:
        return BlockMapping("MetalBlock")
    if block_id == 50:
        return BlockMapping("TorchBlock")
    if block_id == 57:
        return BlockMapping("DiamondBlock")
    if block_id == 65:
        return BlockMapping("LadderBlock")
    if block_id == 79:
        return BlockMapping("IceBlock")
    if block_id in (10, 11):
        return BlockMapping("LavaBlock")
    if block_id == 101:
        return BlockMapping("IronBarsBlock")
    if block_id in (123, 124):  # redstone lamp, lit/unlit
        return BlockMapping("GlowBlock")
    if block_id == 133:
        return BlockMapping("EmeraldBlock")
    if block_id == 166:
        return BlockMapping("BarrierBlock")

    return None


def _select_zip_regions(archive: zipfile.ZipFile, world_name: str | None) -> list[tuple[str, bytes]]:
    groups: dict[str, list[str]] = {}
    for member in archive.namelist():
        normalized = member.replace("\\", "/")
        lower = normalized.lower()
        marker = "/region/"
        if not lower.endswith(".mca") or marker not in lower:
            continue
        marker_index = lower.rfind(marker)
        prefix = normalized[:marker_index].rstrip("/")
        groups.setdefault(prefix, []).append(normalized)

    if not groups:
        raise ConversionError("ZIP contains no region/*.mca files")

    choices = sorted(groups)
    if world_name:
        desired = world_name.strip("/\\").replace("\\", "/")
        matching = [choice for choice in choices if choice == desired or choice.endswith("/" + desired)]
        if len(matching) != 1:
            available = ", ".join(choices)
            raise ConversionError(f"world '{world_name}' is ambiguous or absent; available: {available}")
        chosen = matching[0]
    elif len(choices) == 1:
        chosen = choices[0]
    else:
        raise ConversionError("ZIP has multiple worlds; pass --world (available: " + ", ".join(choices) + ")")

    return [(member, archive.read(member)) for member in sorted(groups[chosen])]


def read_region_files(source: Path, world_name: str | None) -> list[tuple[str, bytes]]:
    """Load region file bytes from a ZIP, world folder, region folder, or MCA."""

    if not source.exists():
        raise ConversionError(f"source does not exist: {source}")
    if source.is_file() and source.suffix.lower() == ".zip":
        try:
            with zipfile.ZipFile(source) as archive:
                return _select_zip_regions(archive, world_name)
        except zipfile.BadZipFile as exc:
            raise ConversionError(f"invalid ZIP archive: {source}") from exc
    if source.is_file() and source.suffix.lower() == ".mca":
        return [(source.name, source.read_bytes())]
    if not source.is_dir():
        raise ConversionError("source must be a .zip archive, .mca file, or world directory")

    candidates: list[Path] = []
    if world_name:
        world_region = source / world_name / "region"
        if world_region.is_dir():
            candidates = sorted(world_region.glob("*.mca"))
    if not candidates:
        if (source / "region").is_dir():
            candidates = sorted((source / "region").glob("*.mca"))
        elif source.name.lower() == "region":
            candidates = sorted(source.glob("*.mca"))
        else:
            candidates = sorted(path for path in source.rglob("*.mca") if path.parent.name.lower() == "region")
    if not candidates:
        raise ConversionError(f"no region/*.mca files found under: {source}")
    return [(str(path), path.read_bytes()) for path in candidates]


def decompress_chunk(compression: int, compressed: bytes, label: str) -> bytes:
    try:
        if compression == 1:
            return gzip.decompress(compressed)
        if compression == 2:
            return zlib.decompress(compressed)
        if compression == 3:
            return compressed
    except (gzip.BadGzipFile, OSError, zlib.error) as exc:
        raise ConversionError(f"cannot decompress {label}") from exc
    raise ConversionError(f"{label} uses unsupported Anvil compression type {compression}")


def iter_mca_roots(region_name: str, data: bytes, stats: ConversionStats) -> Iterator[tuple[str, dict[str, Any]]]:
    if len(data) < HEADER_BYTES:
        raise ConversionError(f"{region_name}: file is shorter than an Anvil header")

    for slot in range(1024):
        entry = int.from_bytes(data[slot * 4:slot * 4 + 4], byteorder="big")
        sector_offset, sector_count = entry >> 8, entry & 0xFF
        if sector_offset == 0:
            continue
        label = f"{region_name} chunk-slot {slot}"
        if sector_count == 0:
            raise ConversionError(f"{label}: nonzero offset with zero sector count")
        byte_offset = sector_offset * SECTOR_BYTES
        if byte_offset + 5 > len(data):
            raise ConversionError(f"{label}: sector offset lies outside file")
        length = int.from_bytes(data[byte_offset:byte_offset + 4], byteorder="big")
        max_length = sector_count * SECTOR_BYTES - 4
        if length < 1 or length > max_length or byte_offset + 4 + length > len(data):
            raise ConversionError(f"{label}: invalid chunk length {length}")
        compression_byte = data[byte_offset + 4]
        if compression_byte & 0x80:
            # External .mcc sidecars were added after 1.12 and cannot live in
            # this ZIP-only flow.  Fail explicitly instead of silently losing a
            # chunk.
            raise ConversionError(f"{label}: external .mcc chunk storage is unsupported")
        compression = compression_byte & 0x7F
        stats.compression_types[compression] += 1
        compressed = data[byte_offset + 5:byte_offset + 4 + length]
        raw_nbt = decompress_chunk(compression, compressed, label)
        try:
            root = NbtReader(raw_nbt).read_root()
        except (ConversionError, struct.error) as exc:
            raise ConversionError(f"{label}: {exc}") from exc
        stats.chunks += 1
        yield label, root


def nibble_at(values: bytes, index: int) -> int:
    value = values[index >> 1]
    return (value >> (4 * (index & 1))) & 0x0F


def _section_bytes(section: dict[str, Any], name: str, expected: int, label: str) -> bytes:
    value = section.get(name)
    if not isinstance(value, bytes) or len(value) != expected:
        actual = len(value) if isinstance(value, bytes) else "missing"
        raise ConversionError(f"{label}: section {name} length is {actual}, expected {expected}")
    return value


def iter_legacy_blocks(label: str, root: dict[str, Any]) -> Iterator[tuple[int, int, int, int, int]]:
    level = root.get("Level", root)
    if not isinstance(level, dict):
        raise ConversionError(f"{label}: legacy Level compound is missing")
    chunk_x = level.get("xPos")
    chunk_z = level.get("zPos")
    if not isinstance(chunk_x, int) or not isinstance(chunk_z, int):
        raise ConversionError(f"{label}: xPos/zPos are missing")
    sections = level.get("Sections", [])
    if not isinstance(sections, list):
        raise ConversionError(f"{label}: Sections is not an NBT list")

    for section_index, section in enumerate(sections):
        if not isinstance(section, dict):
            raise ConversionError(f"{label}: section {section_index} is not a compound")
        section_y = section.get("Y")
        if not isinstance(section_y, int):
            raise ConversionError(f"{label}: section {section_index} has no Y value")
        blocks = _section_bytes(section, "Blocks", SECTION_VOLUME, label)
        metadata = _section_bytes(section, "Data", NIBBLE_BYTES, label)
        add = section.get("Add")
        if add is not None and (not isinstance(add, bytes) or len(add) != NIBBLE_BYTES):
            actual = len(add) if isinstance(add, bytes) else "invalid"
            raise ConversionError(f"{label}: section Add length is {actual}, expected {NIBBLE_BYTES}")

        base_x = chunk_x * 16
        base_y = section_y * 16
        base_z = chunk_z * 16
        for index, low_id in enumerate(blocks):
            block_id = low_id | (nibble_at(add, index) << 8 if add is not None else 0)
            if block_id == 0:
                continue
            x = base_x + (index & 0x0F)
            y = base_y + (index >> 8)
            z = base_z + ((index >> 4) & 0x0F)
            yield x, y, z, block_id, nibble_at(metadata, index)


def convert(
    regions: Iterable[tuple[str, bytes]],
    *,
    unsupported: str,
    breakable: bool,
) -> tuple[list[ConvertedBlock], ConversionStats]:
    stats = ConversionStats()
    converted: list[ConvertedBlock] = []
    seen_chunks: set[tuple[int, int]] = set()

    for region_name, data in regions:
        stats.region_files += 1
        for label, root in iter_mca_roots(region_name, data, stats):
            level = root.get("Level", root)
            if not isinstance(level, dict):
                raise ConversionError(f"{label}: legacy Level compound is missing")
            chunk_key = (level.get("xPos"), level.get("zPos"))
            if not all(isinstance(value, int) for value in chunk_key):
                raise ConversionError(f"{label}: xPos/zPos are missing")
            if chunk_key in seen_chunks:
                raise ConversionError(f"{label}: duplicate chunk coordinates {chunk_key}")
            seen_chunks.add(chunk_key)

            for x, y, z, block_id, metadata in iter_legacy_blocks(label, root):
                stats.non_air_blocks += 1
                stats.include_position(x, y, z)
                variant = (block_id, metadata)
                stats.source_variants[variant] += 1
                mapping = map_minecraft_block(block_id, metadata)
                if mapping is None:
                    stats.unsupported_variants[variant] += 1
                    if unsupported == "skip":
                        stats.skipped_blocks += 1
                        continue
                    mapping = BlockMapping("StoneBlock" if unsupported == "stone" else "Solid")
                    stats.replaced_blocks += 1

                static_breakable = breakable and mapping.token != "BarrierBlock"
                converted.append(
                    ConvertedBlock(
                        x,
                        y,
                        z,
                        mapping.token,
                        -1,
                        static_breakable,
                        metadata & 0x0F,
                    )
                )
                stats.output_blocks += 1
                stats.output_tokens[mapping.token] += 1

            sections = level.get("Sections", [])
            if isinstance(sections, list):
                stats.sections += len(sections)

    if not converted:
        raise ConversionError("no output blocks remain after mapping")
    return converted, stats


def read_special_lines(path: Path) -> list[str]:
    if not path.exists():
        raise ConversionError(f"specials file does not exist: {path}")
    result: list[str] = []
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 6 or fields[0] != "special":
            raise ConversionError(f"{path}:{line_number}: expected 'special kind x y z team'")
        try:
            int(fields[2])
            int(fields[3])
            int(fields[4])
            int(fields[5])
        except ValueError as exc:
            raise ConversionError(f"{path}:{line_number}: special coordinates/team must be integers") from exc
        result.append(" ".join(fields))
    return result


def special_fields(line: str, source: str) -> tuple[str, int, int, int, int]:
    """Parse one canonical special line and retain its stable serialized form."""

    fields = line.split()
    if len(fields) != 6 or fields[0] != "special":
        raise ConversionError(f"{source}: expected 'special kind x y z team'")
    try:
        return fields[1], int(fields[2]), int(fields[3]), int(fields[4]), int(fields[5])
    except ValueError as exc:
        raise ConversionError(f"{source}: special coordinates/team must be integers") from exc


def validate_special_lines(specials: Iterable[str]) -> list[tuple[str, int, int, int, int]]:
    parsed: list[tuple[str, int, int, int, int]] = []
    positions: set[tuple[int, int, int]] = set()
    for index, line in enumerate(specials, start=1):
        kind, x, y, z, team_id = special_fields(line, f"special #{index}")
        position = (x, y, z)
        if position in positions:
            raise ConversionError(f"special #{index}: duplicate special position {position}")
        positions.add(position)
        parsed.append((kind, x, y, z, team_id))
    return parsed


def normalized_center(stats: ConversionStats, center_x: int | None, center_z: int | None) -> tuple[int, int]:
    min_x, max_x, _, _, min_z, max_z = stats.source_bounds
    # Integer coordinates cannot represent a half-block centre.  Floor division
    # yields a compact range around zero (for this archive: X -112..111,
    # Z -120..119) and is stable across repeated runs.
    return (
        center_x if center_x is not None else (min_x + max_x) // 2,
        center_z if center_z is not None else (min_z + max_z) // 2,
    )


def write_dbmap(
    output: Path,
    blocks: list[ConvertedBlock],
    *,
    center_x: int,
    center_z: int,
    baseline_y: int,
    biome: int,
    layout: int,
    collapse_minutes: float,
    specials: list[str],
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    # The order matches SaveCreativeMapDocument and makes generated maps
    # diff-friendly.  Stable token serialization is intentional: numeric enum
    # values change as BlockType grows, while names remain readable.
    blocks.sort(key=lambda block: (block.y, block.x, block.z))
    with output.open("w", encoding="utf-8", newline="\n") as file:
        file.write("daibedmap 1\n")
        file.write(f"biome {biome}\n")
        file.write(f"layout {layout}\n")
        if collapse_minutes > 0.0:
            file.write(f"collapse {collapse_minutes:g}\n")
        for special in specials:
            file.write(special + "\n")
        for block in blocks:
            file.write(
                f"block {block.x - center_x} {block.y - baseline_y} {block.z - center_z} "
                f"{block.token} {block.team_id} {1 if block.breakable else 0} {block.variant}\n"
            )


def _print_counter(
    heading: str,
    counter: collections.Counter[Any],
    formatter: callable,
    limit: int = 30,
) -> None:
    print(heading)
    if not counter:
        print("  (none)")
        return
    ordered = sorted(counter.items(), key=lambda item: (-item[1], formatter(item[0])))
    for key, count in ordered[:limit]:
        print(f"  {formatter(key)}: {count}")
    if len(ordered) > limit:
        print(f"  ... {len(ordered) - limit} more variants")


def print_colored_landmarks(
    blocks: Iterable[ConvertedBlock],
    *,
    center_x: int,
    center_z: int,
    baseline_y: int,
) -> None:
    """Report the four colour-coded castles used by default specials."""

    dye_names = ((14, "red"), (11, "blue"), (5, "lime"), (4, "yellow"))
    decorative_tokens = {"ColoredClayBlock", "ColoredGlassBlock", "WoolBlock"}
    print("Team-colour architecture landmarks (normalized):")
    for dye, name in dye_names:
        points = [
            (block.x - center_x, block.y - baseline_y, block.z - center_z)
            for block in blocks
            if block.token in decorative_tokens and block.variant == dye
        ]
        if not points:
            print(f"  {name}: (not present)")
            continue
        # A 2D projection joins vertical walls into one castle cluster without
        # merging the widely separated bases.  Lime has a secondary central
        # glass feature, so selecting the largest component is important.
        columns: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
        for point in points:
            columns.setdefault((point[0], point[2]), []).append(point)
        pending = set(columns)
        components: list[list[tuple[int, int, int]]] = []
        while pending:
            start = pending.pop()
            stack = [start]
            cells = [start]
            while stack:
                x, z = stack.pop()
                for dx in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        neighbour = (x + dx, z + dz)
                        if neighbour in pending:
                            pending.remove(neighbour)
                            stack.append(neighbour)
                            cells.append(neighbour)
            components.append([point for cell in cells for point in columns[cell]])
        components.sort(key=len, reverse=True)
        primary = components[0]
        xs, ys, zs = zip(*primary)
        print(
            f"  {name}: primary {len(primary)}/{len(points)} blocks; "
            f"X {min(xs)}..{max(xs)}, Y {min(ys)}..{max(ys)}, Z {min(zs)}..{max(zs)}"
        )
        if len(components) > 1:
            print(f"    secondary colour components: {len(components) - 1}")


def print_resource_landmarks(
    blocks: Iterable[ConvertedBlock],
    *,
    center_x: int,
    center_z: int,
    baseline_y: int,
) -> None:
    """Show material markers that guided the base/centre generator placement."""

    print("Resource-marker landmarks (normalized):")
    for token in ("LapisBlock", "DiamondBlock", "EmeraldBlock", "GoldBlock", "MetalBlock"):
        points = [
            (block.x - center_x, block.y - baseline_y, block.z - center_z)
            for block in blocks
            if block.token == token
        ]
        if not points:
            continue
        xs, ys, zs = zip(*points)
        print(
            f"  {token}: {len(points)} blocks; "
            f"X {min(xs)}..{max(xs)}, Y {min(ys)}..{max(ys)}, Z {min(zs)}..{max(zs)}"
        )


def print_special_report(specials: Iterable[tuple[str, int, int, int, int]]) -> None:
    special_list = list(specials)
    counts = collections.Counter(kind for kind, *_ in special_list)
    print("Default/selected gameplay specials:")
    print("  counts: " + ", ".join(f"{kind}={count}" for kind, count in sorted(counts.items())))
    for kind, x, y, z, team_id in special_list:
        print(f"  {kind:11s} ({x:4d}, {y:3d}, {z:4d}) team {team_id}")


def print_report(
    source: Path,
    stats: ConversionStats,
    *,
    blocks: Iterable[ConvertedBlock],
    specials: Iterable[tuple[str, int, int, int, int]],
    center_x: int,
    center_z: int,
    baseline_y: int,
    unsupported: str,
) -> None:
    min_x, max_x, min_y, max_y, min_z, max_z = stats.source_bounds
    print("Minecraft legacy Anvil -> DaiBed conversion report")
    print(f"source: {source}")
    print(f"regions: {stats.region_files}; chunks: {stats.chunks}; sections: {stats.sections}")
    print(f"Anvil compression types: {dict(sorted(stats.compression_types.items()))}")
    print(f"non-air source blocks: {stats.non_air_blocks}; output blocks: {stats.output_blocks}")
    print(f"source bounds: X {min_x}..{max_x}; Y {min_y}..{max_y}; Z {min_z}..{max_z}")
    print(
        "normalization: "
        f"x -= {center_x}; y -= {baseline_y}; z -= {center_z} "
        f"(colours are encoded in variant; unsupported: {unsupported})"
    )
    if stats.unsupported_variants:
        print(f"unsupported blocks: {sum(stats.unsupported_variants.values())}; "
              f"replaced: {stats.replaced_blocks}; skipped: {stats.skipped_blocks}")
    _print_counter(
        "Top source ID:data variants:",
        stats.source_variants,
        lambda item: f"{item[0]}:{item[1]}",
    )
    _print_counter("Output dbmap tokens:", stats.output_tokens, str, limit=100)
    _print_counter(
        "Unsupported ID:data variants:",
        stats.unsupported_variants,
        lambda item: f"{item[0]}:{item[1]}",
        limit=100,
    )
    print_colored_landmarks(
        blocks,
        center_x=center_x,
        center_z=center_z,
        baseline_y=baseline_y,
    )
    print_resource_landmarks(
        blocks,
        center_x=center_x,
        center_z=center_z,
        baseline_y=baseline_y,
    )
    print_special_report(specials)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert Minecraft Java 1.12 Blocks/Data/Add Anvil regions to a string-token DaiBed dbmap."
    )
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        help=f"ZIP, .mca, or world directory (default: {DEFAULT_INPUT})",
    )
    parser.add_argument(
        "-i", "--input",
        dest="input_path",
        type=Path,
        help="Named equivalent of the positional source argument.",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"Destination dbmap (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--world",
        help="World directory inside a ZIP when it contains more than one (for example Bedwars_castle).",
    )
    parser.add_argument("--report", action="store_true", help="Analyze and print a report without writing a dbmap.")
    parser.add_argument(
        "--unsupported",
        choices=("stone", "solid", "skip"),
        default="stone",
        help="How to handle source IDs without a mapping; stone preserves walkable geometry (default: stone).",
    )
    parser.add_argument(
        "--breakable",
        action="store_true",
        help="Mark imported geometry breakable. Default is unbreakable static arena geometry; barriers stay unbreakable.",
    )
    parser.add_argument(
        "--center-x",
        type=int,
        help="Source X coordinate to subtract. Defaults to the source non-air bounding-box centre.",
    )
    parser.add_argument(
        "--center-z",
        type=int,
        help="Source Z coordinate to subtract. Defaults to the source non-air bounding-box centre.",
    )
    parser.add_argument(
        "--baseline-y",
        type=int,
        default=64,
        help="Source Y coordinate to subtract (default: 64 for the supplied Minecraft map).",
    )
    parser.add_argument("--biome", type=int, default=4, help="DaiBed biome value to write (default: 4, ruins).")
    parser.add_argument("--layout", type=int, default=0, help="DaiBed layout value to write (default: 0, classic).")
    parser.add_argument(
        "--specials",
        type=Path,
        help="Optional normalized special-lines file to append to the default castle specials.",
    )
    parser.add_argument(
        "--no-castle-specials",
        action="store_true",
        help="Write geometry only (use this for a source world other than the supplied Bedwars_castle).",
    )
    args = parser.parse_args(argv)
    if args.source is not None and args.input_path is not None:
        parser.error("use either positional source or --input, not both")
    args.source = args.input_path or args.source or DEFAULT_INPUT
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        regions = read_region_files(args.source, args.world)
        blocks, stats = convert(
            regions,
            unsupported=args.unsupported,
            breakable=args.breakable,
        )
        center_x, center_z = normalized_center(stats, args.center_x, args.center_z)
        specials = [] if args.no_castle_specials else list(DEFAULT_CASTLE_SPECIALS)
        if args.specials:
            specials.extend(read_special_lines(args.specials))
        parsed_specials = validate_special_lines(specials)
        print_report(
            args.source,
            stats,
            blocks=blocks,
            specials=parsed_specials,
            center_x=center_x,
            center_z=center_z,
            baseline_y=args.baseline_y,
            unsupported=args.unsupported,
        )
        if args.report:
            return 0
        write_dbmap(
            args.output,
            blocks,
            center_x=center_x,
            center_z=center_z,
            baseline_y=args.baseline_y,
            biome=args.biome,
            layout=args.layout,
            collapse_minutes=28.0 if not args.no_castle_specials else 0.0,
            specials=specials,
        )
        print(f"written: {args.output} ({len(blocks)} blocks, {len(specials)} specials)")
        return 0
    except (ConversionError, OSError) as exc:
        print(f"conversion failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
