#!/usr/bin/env python3
"""Transcribe roster voice MP4 files, align them to the script, and cut WAV cues."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import subprocess
import sys
from dataclasses import dataclass
from difflib import SequenceMatcher
from pathlib import Path
from typing import Any


DEFAULT_SLUGS = ["radon", "orbita", "brom", "konvoy", "likho", "witness"]
DEFAULT_MODEL_CACHE = (
    Path.home()
    / ".cache"
    / "huggingface"
    / "hub"
    / "models--openai--whisper-base"
    / "snapshots"
)


@dataclass
class ScriptLine:
    hero: str
    slug: str
    source: Path
    event: str
    event_index: int
    line_index: int
    text: str


@dataclass
class Word:
    token: str
    raw: str
    start: float
    end: float


@dataclass
class Match:
    start_index: int
    end_index: int
    confidence: float
    token_ratio: float
    char_ratio: float
    coverage: float
    asr_text: str


def log(message: str) -> None:
    print(message, flush=True)


def normalize_tokens(text: str) -> list[str]:
    text = text.lower().replace("\u0451", "\u0435")
    return re.findall(r"[0-9a-z\u0430-\u044f]+", text, flags=re.IGNORECASE)


def canonical_key(text: str) -> str:
    return "".join(normalize_tokens(text))


def parse_roster(path: Path, root: Path) -> tuple[list[ScriptLine], list[tuple[str, str, Path]]]:
    text = path.read_text(encoding="utf-8")
    mp4_by_key = {canonical_key(p.stem): p for p in root.glob("*.mp4")}
    lines: list[ScriptLine] = []
    heroes: list[tuple[str, str, Path]] = []
    hero = ""
    slug = ""
    source: Path | None = None
    event = ""
    event_counts: dict[tuple[str, str], int] = {}
    hero_line_count = 0
    hero_index = -1

    for raw in text.splitlines():
        line = raw.strip()
        if not line or all(ch == "=" for ch in line):
            continue

        if line.endswith(":"):
            event = line[:-1]
            continue

        if line.startswith("- "):
            if not hero or source is None or not event:
                raise ValueError(f"Script line before hero/event: {raw!r}")
            key = (hero, event)
            event_counts[key] = event_counts.get(key, 0) + 1
            hero_line_count += 1
            lines.append(
                ScriptLine(
                    hero=hero,
                    slug=slug,
                    source=source,
                    event=event,
                    event_index=event_counts[key],
                    line_index=hero_line_count,
                    text=line[2:].strip(),
                )
            )
            continue

        hero = line
        hero_index += 1
        hero_line_count = 0
        event = ""
        slug = DEFAULT_SLUGS[hero_index] if hero_index < len(DEFAULT_SLUGS) else canonical_key(hero)
        source = mp4_by_key.get(canonical_key(hero))
        if source is None:
            raise FileNotFoundError(f"No MP4 source found for roster hero {hero!r}")
        heroes.append((hero, slug, source))

    return lines, heroes


def model_name_or_path() -> str:
    if DEFAULT_MODEL_CACHE.exists():
        snapshots = sorted((p for p in DEFAULT_MODEL_CACHE.iterdir() if p.is_dir()), key=lambda p: p.name)
        if snapshots:
            return str(snapshots[-1])
    return "openai/whisper-base"


def run_ffmpeg(args: list[str]) -> None:
    command = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", *args]
    subprocess.run(command, check=True)


def extract_asr_audio(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    run_ffmpeg(
        [
            "-i",
            str(source),
            "-map",
            "0:a:0",
            "-vn",
            "-ac",
            "1",
            "-ar",
            "16000",
            "-c:a",
            "pcm_s16le",
            str(target),
        ]
    )


def transcribe_sources(
    heroes: list[tuple[str, str, Path]],
    work_dir: Path,
    force_asr: bool,
    torch_threads: int,
) -> dict[str, dict[str, Any]]:
    cached_results: dict[str, dict[str, Any]] = {}
    pending: list[tuple[str, str, Path]] = []
    for hero, slug, source in heroes:
        asr_json = work_dir / f"{slug}_asr.json"
        if asr_json.exists() and not force_asr:
            log(f"ASR cache: {slug}")
            cached_results[slug] = json.loads(asr_json.read_text(encoding="utf-8"))
        else:
            pending.append((hero, slug, source))

    if not pending:
        return cached_results

    if torch_threads > 0:
        import torch

        torch.set_num_threads(torch_threads)

    from transformers import pipeline

    model = model_name_or_path()
    log(f"Loading ASR model: {model}")
    recognizer = pipeline("automatic-speech-recognition", model=model, device=-1)

    results: dict[str, dict[str, Any]] = dict(cached_results)
    for hero, slug, source in pending:
        asr_json = work_dir / f"{slug}_asr.json"
        audio_path = work_dir / f"{slug}_mp4_16k.wav"
        log(f"Extracting audio: {source.name} -> {audio_path}")
        extract_asr_audio(source, audio_path)

        log(f"Transcribing: {slug} ({hero})")
        result = recognizer(
            str(audio_path),
            chunk_length_s=30,
            return_timestamps="word",
            generate_kwargs={"language": "russian", "task": "transcribe"},
        )
        asr_json.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        results[slug] = result
    return results


def words_from_asr(result: dict[str, Any]) -> list[Word]:
    words: list[Word] = []
    for chunk in result.get("chunks", []):
        timestamp = chunk.get("timestamp")
        if not timestamp or timestamp[0] is None:
            continue
        start = float(timestamp[0])
        end = float(timestamp[1] if timestamp[1] is not None else start + 0.1)
        if end < start:
            end = start + 0.1
        raw = str(chunk.get("text", "")).strip()
        tokens = normalize_tokens(raw)
        if not tokens:
            continue
        for token in tokens:
            words.append(Word(token=token, raw=raw, start=start, end=end))

    words.sort(key=lambda word: (word.start, word.end, word.token))
    deduped: list[Word] = []
    for word in words:
        if (
            deduped
            and word.token == deduped[-1].token
            and abs(word.start - deduped[-1].start) < 0.04
            and abs(word.end - deduped[-1].end) < 0.04
        ):
            continue
        deduped.append(word)
    return deduped


def score_tokens(script_tokens: list[str], window_tokens: list[str]) -> tuple[float, float, float, float]:
    if not script_tokens or not window_tokens:
        return 0.0, 0.0, 0.0, 0.0

    token_ratio = SequenceMatcher(None, script_tokens, window_tokens, autojunk=False).ratio()
    script_text = " ".join(script_tokens)
    window_text = " ".join(window_tokens)
    char_ratio = SequenceMatcher(None, script_text, window_text, autojunk=False).ratio()
    matcher = SequenceMatcher(None, script_tokens, window_tokens, autojunk=False)
    equal_tokens = sum(block.size for block in matcher.get_matching_blocks())
    coverage = equal_tokens / max(1, len(script_tokens))
    confidence = 0.52 * char_ratio + 0.30 * token_ratio + 0.18 * coverage
    return confidence, token_ratio, char_ratio, coverage


def window_raw_text(words: list[Word], start: int, end: int) -> str:
    raw: list[str] = []
    previous = ""
    for word in words[start : end + 1]:
        if word.raw != previous:
            raw.append(word.raw)
            previous = word.raw
    return " ".join(raw)


def find_best_match(words: list[Word], cursor: int, script_text: str, search_all: bool = False) -> Match | None:
    script_tokens = normalize_tokens(script_text)
    if not script_tokens or cursor >= len(words):
        return None

    script_len = len(script_tokens)
    min_len = max(1, math.floor(script_len * 0.45))
    max_len = min(44, max(script_len + 8, math.ceil(script_len * 2.2)))
    search_start = 0 if search_all else cursor
    search_limit = len(words) if search_all else min(len(words), cursor + max(130, script_len * 18))
    best: Match | None = None

    for start in range(search_start, search_limit):
        latest_end = min(len(words), start + max_len)
        for end_exclusive in range(start + min_len, latest_end + 1):
            window = words[start:end_exclusive]
            window_tokens = [word.token for word in window]
            confidence, token_ratio, char_ratio, coverage = score_tokens(script_tokens, window_tokens)
            distance_penalty = 0.0 if search_all else min(0.10, max(0, start - cursor) / 1400.0)
            adjusted = confidence - distance_penalty
            if best is None or adjusted > best.confidence:
                best = Match(
                    start_index=start,
                    end_index=end_exclusive - 1,
                    confidence=adjusted,
                    token_ratio=token_ratio,
                    char_ratio=char_ratio,
                    coverage=coverage,
                    asr_text=window_raw_text(words, start, end_exclusive - 1),
                )

    return best


def status_for(confidence: float, script_text: str, min_confidence: float, good_confidence: float) -> str:
    if confidence < min_confidence:
        return "missing"
    if confidence >= good_confidence:
        return "matched"
    if len(normalize_tokens(script_text)) <= 2 and confidence < good_confidence:
        return "low_confidence"
    return "low_confidence"


def overlap_fraction(a_start: int, a_end: int, b_start: int, b_end: int) -> float:
    overlap = max(0, min(a_end, b_end) - max(a_start, b_start) + 1)
    shorter = max(1, min(a_end - a_start + 1, b_end - b_start + 1))
    return overlap / shorter


def ffprobe_duration(source: Path) -> float:
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=nk=1:nw=1",
            str(source),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return float(result.stdout.strip())


def detect_voice_segments(
    source: Path,
    noise: str = "-35dB",
    min_silence: float = 0.18,
    min_segment: float = 0.12,
) -> list[tuple[float, float]]:
    duration = ffprobe_duration(source)
    command = [
        "ffmpeg",
        "-hide_banner",
        "-nostats",
        "-i",
        str(source),
        "-map",
        "0:a:0",
        "-vn",
        "-af",
        f"silencedetect=noise={noise}:d={min_silence}",
        "-f",
        "null",
        "-",
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8", errors="ignore")
    starts = [float(value) for value in re.findall(r"silence_start: ([0-9.]+)", result.stderr)]
    ends = [float(value) for value in re.findall(r"silence_end: ([0-9.]+)", result.stderr)]

    segments: list[tuple[float, float]] = []
    cursor = 0.0
    for silence_start, silence_end in zip(starts, ends):
        if silence_start - cursor >= min_segment:
            segments.append((cursor, silence_start))
        cursor = max(cursor, silence_end)
    if duration - cursor >= min_segment:
        segments.append((cursor, duration))
    return segments


def row_with_match(
    line: ScriptLine,
    match: Match | None,
    words: list[Word],
    status: str,
    pre_pad: float,
    post_pad: float,
) -> dict[str, Any]:
    start = ""
    end = ""
    duration = ""
    asr_text = ""
    token_ratio = ""
    char_ratio = ""
    coverage = ""
    match_start = ""
    match_end = ""
    confidence = match.confidence if match else 0.0

    if match and status != "missing":
        first = words[match.start_index]
        last = words[match.end_index]
        start_value = max(0.0, first.start - pre_pad)
        end_value = max(start_value + 0.18, last.end + post_pad)
        start = f"{start_value:.3f}"
        end = f"{end_value:.3f}"
        duration = f"{end_value - start_value:.3f}"
        asr_text = match.asr_text
        token_ratio = f"{match.token_ratio:.3f}"
        char_ratio = f"{match.char_ratio:.3f}"
        coverage = f"{match.coverage:.3f}"
        match_start = str(match.start_index)
        match_end = str(match.end_index)

    output_rel = f"assets/audio/voice/{line.slug}/{line.event}_{line.event_index:02d}.wav"
    return {
        "hero": line.hero,
        "slug": line.slug,
        "source_file": line.source.name,
        "event": line.event,
        "event_index": line.event_index,
        "line_index": line.line_index,
        "status": status,
        "confidence": f"{confidence:.3f}",
        "start": start,
        "end": end,
        "duration": duration,
        "output_file": output_rel if status != "missing" else "",
        "script_text": line.text,
        "asr_text": asr_text,
        "token_ratio": token_ratio,
        "char_ratio": char_ratio,
        "coverage": coverage,
        "_match_start": match_start,
        "_match_end": match_end,
    }


def apply_overlap_filter(rows: list[dict[str, Any]]) -> None:
    accepted: dict[str, list[dict[str, Any]]] = {}
    candidates = sorted(
        [row for row in rows if row["status"] != "missing" and row["_match_start"]],
        key=lambda row: float(row["confidence"]),
        reverse=True,
    )
    for row in candidates:
        slug = str(row["slug"])
        start = int(str(row["_match_start"]))
        end = int(str(row["_match_end"]))
        conflict = False
        for other in accepted.setdefault(slug, []):
            other_start = int(str(other["_match_start"]))
            other_end = int(str(other["_match_end"]))
            if overlap_fraction(start, end, other_start, other_end) >= 0.80:
                conflict = True
                break
        if conflict:
            row["status"] = "missing"
            row["output_file"] = ""
            row["start"] = ""
            row["end"] = ""
            row["duration"] = ""
            continue
        accepted[slug].append(row)


def apply_vad_fallback(
    rows: list[dict[str, Any]],
    lines: list[ScriptLine],
    root: Path,
    pre_pad: float,
    post_pad: float,
) -> None:
    lines_by_slug: dict[str, list[ScriptLine]] = {}
    for line in lines:
        lines_by_slug.setdefault(line.slug, []).append(line)

    for slug, slug_lines in lines_by_slug.items():
        slug_rows = [row for row in rows if row["slug"] == slug]
        matched = sum(1 for row in slug_rows if row["status"] != "missing")
        if slug != "witness" and matched > 0:
            continue
        if not slug_lines:
            continue

        source = root / slug_rows[0]["source_file"]
        segments = detect_voice_segments(source)
        if len(segments) < max(3, len(slug_lines) // 2):
            continue

        for row, segment in zip(slug_rows, segments):
            start_value = max(0.0, segment[0] - pre_pad)
            end_value = segment[1] + post_pad
            row["status"] = "vad_fallback"
            row["confidence"] = "0.500"
            row["start"] = f"{start_value:.3f}"
            row["end"] = f"{end_value:.3f}"
            row["duration"] = f"{end_value - start_value:.3f}"
            row["output_file"] = f"assets/audio/voice/{slug}/{row['event']}_{int(row['event_index']):02d}.wav"
            row["asr_text"] = ""
            row["token_ratio"] = ""
            row["char_ratio"] = ""
            row["coverage"] = ""


def align_lines(
    lines: list[ScriptLine],
    asr_results: dict[str, dict[str, Any]],
    min_confidence: float,
    good_confidence: float,
    pre_pad: float,
    post_pad: float,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    words_by_slug = {slug: words_from_asr(result) for slug, result in asr_results.items()}
    cursor_by_slug = {slug: 0 for slug in asr_results}

    for line in lines:
        words = words_by_slug.get(line.slug, [])
        cursor = cursor_by_slug.get(line.slug, 0)
        sequential = find_best_match(words, cursor, line.text, search_all=False)
        sequential_confidence = sequential.confidence if sequential else 0.0
        sequential_status = status_for(sequential_confidence, line.text, min_confidence, good_confidence)

        if sequential_status != "missing":
            match = sequential
            status = sequential_status
            if match:
                cursor_by_slug[line.slug] = match.end_index + 1
        else:
            match = find_best_match(words, 0, line.text, search_all=True)
            confidence = match.confidence if match else 0.0
            status = status_for(confidence, line.text, min_confidence, good_confidence)
        rows.append(row_with_match(line, match, words, status, pre_pad, post_pad))

    apply_overlap_filter(rows)
    root = lines[0].source.parent if lines else Path(".")
    apply_vad_fallback(rows, lines, root, pre_pad, post_pad)
    for row in rows:
        row.pop("_match_start", None)
        row.pop("_match_end", None)
    return rows


def cut_rows(root: Path, rows: list[dict[str, Any]], cut_confidence: float) -> int:
    count = 0
    for row in rows:
        if row["status"] == "missing":
            continue
        if row["status"] != "vad_fallback" and float(row["confidence"]) < cut_confidence:
            continue
        output = root / str(row["output_file"])
        output.parent.mkdir(parents=True, exist_ok=True)
        source = root / str(row["source_file"])
        start = float(row["start"])
        duration = float(row["duration"])
        run_ffmpeg(
            [
                "-i",
                str(source),
                "-map",
                "0:a:0",
                "-vn",
                "-ss",
                f"{start:.3f}",
                "-t",
                f"{duration:.3f}",
                "-ac",
                "1",
                "-ar",
                "48000",
                "-c:a",
                "pcm_s16le",
                str(output),
            ]
        )
        count += 1
    return count


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "hero",
        "slug",
        "source_file",
        "event",
        "event_index",
        "line_index",
        "status",
        "confidence",
        "start",
        "end",
        "duration",
        "output_file",
        "script_text",
        "asr_text",
        "token_ratio",
        "char_ratio",
        "coverage",
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows: list[dict[str, Any]]) -> None:
    by_slug: dict[str, dict[str, int]] = {}
    for row in rows:
        slug = str(row["slug"])
        status = str(row["status"])
        by_slug.setdefault(slug, {})
        by_slug[slug][status] = by_slug[slug].get(status, 0) + 1

    for slug, counts in by_slug.items():
        summary = " ".join(f"{status}={count}" for status, count in sorted(counts.items()))
        log(f"{slug}: {summary}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--script", type=Path, default=Path("roster voice lines.txt"))
    parser.add_argument("--work-dir", type=Path, default=Path("build/voice_work"))
    parser.add_argument("--review-csv", type=Path, default=Path("voice_cuts_review.csv"))
    parser.add_argument("--force-asr", action="store_true")
    parser.add_argument("--no-cut", action="store_true")
    parser.add_argument("--torch-threads", type=int, default=0)
    parser.add_argument("--min-confidence", type=float, default=0.46)
    parser.add_argument("--good-confidence", type=float, default=0.68)
    parser.add_argument("--cut-confidence", type=float, default=0.46)
    parser.add_argument("--pre-pad", type=float, default=0.12)
    parser.add_argument("--post-pad", type=float, default=0.25)
    args = parser.parse_args()

    root = args.root.resolve()
    script_path = args.script if args.script.is_absolute() else root / args.script
    work_dir = args.work_dir if args.work_dir.is_absolute() else root / args.work_dir
    review_csv = args.review_csv if args.review_csv.is_absolute() else root / args.review_csv

    lines, heroes = parse_roster(script_path, root)
    log(f"Roster lines: {len(lines)}")
    for hero, slug, source in heroes:
        hero_total = sum(1 for line in lines if line.slug == slug)
        log(f"{slug}: {hero_total} scripted lines from {source.name}")

    asr_results = transcribe_sources(heroes, work_dir, args.force_asr, args.torch_threads)
    rows = align_lines(
        lines,
        asr_results,
        args.min_confidence,
        args.good_confidence,
        args.pre_pad,
        args.post_pad,
    )
    write_csv(review_csv, rows)
    log(f"Review CSV: {review_csv}")
    summarize(rows)

    if not args.no_cut:
        cut_count = cut_rows(root, rows, args.cut_confidence)
        log(f"Cut WAV files: {cut_count}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise
