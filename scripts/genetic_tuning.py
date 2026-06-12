#!/usr/bin/env python3
"""Offline genetic tuning runner for DaiBed bot AI.

The game reads flat JSON keys such as "rusher.desiredBlocks" for a global
genome and "team2.rusher.desiredBlocks" for per-team tournament slots.
This runner exploits that by placing up to four candidate genomes into one
automatch run and scoring each team separately.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import queue
import random
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROLE_KEYS = ("defender", "rusher", "collector", "fighter")

DEFAULT_GENOME: dict[str, float | int | str] = {
    "id": "seed",
    "generation": 0,
    "fitness": 0.0,
    "defender.desiredBlocks": 24.0,
    "defender.retreatHealthCoreAlive": 34.0,
    "defender.retreatHealthFinalLife": 20.0,
    "defender.fightHealth": 26.0,
    "defender.lootReturnValue": 30.0,
    "defender.engageRange": 7.4,
    "defender.roleLockSeconds": 7.0,
    "defender.pressureBiasScale": 0.75,
    "defender.defenseBiasScale": 1.20,
    "defender.resourceBiasScale": 0.85,
    "defender.combatBiasScale": 0.90,
    "rusher.desiredBlocks": 36.0,
    "rusher.retreatHealthCoreAlive": 28.0,
    "rusher.retreatHealthFinalLife": 20.0,
    "rusher.fightHealth": 22.0,
    "rusher.lootReturnValue": 30.0,
    "rusher.engageRange": 4.6,
    "rusher.roleLockSeconds": 5.0,
    "rusher.pressureBiasScale": 1.20,
    "rusher.defenseBiasScale": 0.72,
    "rusher.resourceBiasScale": 0.72,
    "rusher.combatBiasScale": 0.95,
    "collector.desiredBlocks": 20.0,
    "collector.retreatHealthCoreAlive": 44.0,
    "collector.retreatHealthFinalLife": 20.0,
    "collector.fightHealth": 54.0,
    "collector.lootReturnValue": 14.0,
    "collector.engageRange": 3.1,
    "collector.roleLockSeconds": 6.0,
    "collector.pressureBiasScale": 0.55,
    "collector.defenseBiasScale": 0.90,
    "collector.resourceBiasScale": 1.25,
    "collector.combatBiasScale": 0.70,
    "fighter.desiredBlocks": 28.0,
    "fighter.retreatHealthCoreAlive": 30.0,
    "fighter.retreatHealthFinalLife": 20.0,
    "fighter.fightHealth": 28.0,
    "fighter.lootReturnValue": 30.0,
    "fighter.engageRange": 8.6,
    "fighter.roleLockSeconds": 4.6,
    "fighter.pressureBiasScale": 1.00,
    "fighter.defenseBiasScale": 0.95,
    "fighter.resourceBiasScale": 0.75,
    "fighter.combatBiasScale": 1.25,
    "intentLockScale": 1.0,
    "roleLockScale": 1.0,
    "fightRequiredMarginEasy": 24.0,
    "fightRequiredMarginNormal": 8.0,
    "fightRequiredMarginHard": -6.0,
    "retreatPowerMarginEasy": -30.0,
    "retreatPowerMarginNormal": -30.0,
    "retreatPowerMarginHard": -42.0,
    "allyAssistWeight": 0.52,
    "enemyAssistWeight": 0.48,
    "shieldPower": 13.0,
    "speedBoostPower": 10.0,
    "strategicDefenseUrgencyScale": 0.30,
    "repairUrgencyScale": 0.34,
    "strategicAttackUrgencyScale": 0.30,
    "lateAttackUrgencyScale": 0.20,
    "attackSlotBonus": 82.0,
    "attackSlotPenalty": -96.0,
    "breakCoordinationPenalty": 70.0,
    "pressureCoordinationPenalty": 85.0,
    "lateCoordinationPenalty": 80.0,
    "strategicEconomyBonus": 95.0,
    "personalEconomyBonus": 130.0,
    "strategicPressureEconomyPenalty": 55.0,
    "easyPlanCadence": 3.6,
    "normalPlanCadence": 2.4,
    "hardPlanCadence": 1.6,
    "earlyEconomySeconds": 40.0,
    "pressurePhaseSeconds": 42.0,
    "latePressureSeconds": 120.0,
    "allInSeconds": 185.0,
}

BOUNDS: dict[str, tuple[float, float]] = {
    ".desiredBlocks": (4.0, 64.0),
    ".retreatHealthCoreAlive": (4.0, 92.0),
    ".retreatHealthFinalLife": (4.0, 70.0),
    ".fightHealth": (4.0, 92.0),
    ".lootReturnValue": (4.0, 90.0),
    ".engageRange": (1.5, 14.0),
    ".roleLockSeconds": (1.0, 16.0),
    ".pressureBiasScale": (0.20, 2.20),
    ".defenseBiasScale": (0.20, 2.20),
    ".resourceBiasScale": (0.20, 2.20),
    ".combatBiasScale": (0.20, 2.20),
    "intentLockScale": (0.45, 1.80),
    "roleLockScale": (0.45, 1.80),
    "fightRequiredMarginEasy": (-20.0, 60.0),
    "fightRequiredMarginNormal": (-35.0, 45.0),
    "fightRequiredMarginHard": (-55.0, 30.0),
    "retreatPowerMarginEasy": (-80.0, 10.0),
    "retreatPowerMarginNormal": (-85.0, 5.0),
    "retreatPowerMarginHard": (-100.0, 0.0),
    "allyAssistWeight": (0.15, 0.95),
    "enemyAssistWeight": (0.15, 1.10),
    "shieldPower": (0.0, 40.0),
    "speedBoostPower": (0.0, 32.0),
    "strategicDefenseUrgencyScale": (0.05, 0.85),
    "repairUrgencyScale": (0.05, 0.95),
    "strategicAttackUrgencyScale": (0.05, 0.85),
    "lateAttackUrgencyScale": (0.0, 0.75),
    "attackSlotBonus": (-80.0, 220.0),
    "attackSlotPenalty": (-260.0, 40.0),
    "breakCoordinationPenalty": (0.0, 180.0),
    "pressureCoordinationPenalty": (0.0, 220.0),
    "lateCoordinationPenalty": (0.0, 220.0),
    "strategicEconomyBonus": (-80.0, 220.0),
    "personalEconomyBonus": (-80.0, 260.0),
    "strategicPressureEconomyPenalty": (-60.0, 180.0),
    "easyPlanCadence": (0.8, 8.0),
    "normalPlanCadence": (0.6, 6.0),
    "hardPlanCadence": (0.4, 5.0),
    "earlyEconomySeconds": (12.0, 80.0),
    "pressurePhaseSeconds": (18.0, 95.0),
    "latePressureSeconds": (70.0, 190.0),
    "allInSeconds": (110.0, 290.0),
}


def bound_for_key(key: str) -> tuple[float, float] | None:
    for suffix, bounds in BOUNDS.items():
        if key.endswith(suffix):
            return bounds
    return None


def clamp_gene(key: str, value: float) -> float:
    bounds = bound_for_key(key)
    if bounds is None:
        return value
    lo, hi = bounds
    return max(lo, min(hi, value))


def resolve_from_cwd(cwd: str, path: str) -> Path:
    source = Path(path)
    if source.is_absolute():
        return source
    return (Path(cwd) / source).resolve()


def mutate(genome: dict[str, Any], generation: int, scale: float) -> dict[str, Any]:
    child = copy.deepcopy(genome)
    for key in ("screeningFitness", "evaluationStage", "fitnessLast", "fitnessMean", "fitnessBestSeen", "evaluations"):
        child.pop(key, None)
    child["generation"] = generation
    child["fitness"] = 0.0
    for key, value in list(child.items()):
        if key in ("id", "generation", "fitness") or not isinstance(value, (int, float)):
            continue
        bounds = bound_for_key(key)
        if bounds is None:
            continue
        lo, hi = bounds
        sigma = (hi - lo) * scale
        child[key] = round(clamp_gene(key, float(value) + random.gauss(0.0, sigma)), 4)
    child["id"] = f"g{generation}_{random.randrange(1_000_000):06d}"
    return child


def crossover(a: dict[str, Any], b: dict[str, Any], generation: int) -> dict[str, Any]:
    child = copy.deepcopy(a)
    for key in ("screeningFitness", "evaluationStage", "fitnessLast", "fitnessMean", "fitnessBestSeen", "evaluations"):
        child.pop(key, None)
    for key, value in list(child.items()):
        if key in ("id", "generation", "fitness"):
            continue
        if isinstance(value, (int, float)) and random.random() < 0.5:
            child[key] = b[key]
    child["id"] = f"g{generation}_{random.randrange(1_000_000):06d}"
    child["generation"] = generation
    child["fitness"] = 0.0
    return child


def write_team_tuning(path: Path, candidates: list[dict[str, Any]]) -> None:
    payload: dict[str, Any] = {}
    for team_id, genome in enumerate(candidates):
        for key, value in genome.items():
            payload[f"team{team_id}.{key}"] = value
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def write_process_log(path: Path, payload: str | bytes | None) -> None:
    if payload is None:
        path.write_text("", encoding="utf-8")
        return
    if isinstance(payload, bytes):
        payload = payload.decode("utf-8", errors="replace")
    path.write_text(payload, encoding="utf-8")


def team_progress_from_timeline(run: dict[str, Any], team_id: int) -> dict[str, int]:
    progress = {
        "enemy_cores_destroyed": 0,
        "own_core_destroyed": 0,
        "credited_final_kills": 0,
        "credited_void_finals": 0,
        "first_core_damage": 0,
    }
    for event in run.get("timeline", []):
        event_type = event.get("type")
        event_team = int(event.get("teamId", -1))
        actor_team = int(event.get("actorTeamId", -1))
        if event_type == "coreDestroyed":
            if event_team == team_id:
                progress["own_core_destroyed"] += 1
            elif actor_team == team_id:
                progress["enemy_cores_destroyed"] += 1
        elif event_type == "firstCoreDamage" and actor_team == team_id and event_team != team_id:
            progress["first_core_damage"] += 1
        elif event_type == "finalDeath" and actor_team == team_id and event_team != team_id:
            progress["credited_final_kills"] += 1
        elif event_type == "voidFall" and actor_team == team_id and event_team != team_id and int(event.get("value", 0)) > 0:
            progress["credited_void_finals"] += 1
    return progress


def enemy_team_pressure(run: dict[str, Any], team_id: int) -> dict[str, float]:
    enemy_core_health_lost = 0.0
    enemy_eliminated_players = 0.0
    enemy_alive_players = 0.0
    for other in run.get("teams", []):
        other_id = int(other.get("teamId", -1))
        if other_id < 0 or other_id == team_id:
            continue
        core_max = max(1.0, float(other.get("coreMaxHealth", 1)))
        core_health = float(other.get("coreHealth", 0))
        enemy_core_health_lost += max(0.0, core_max - core_health) / core_max
        enemy_eliminated_players += float(other.get("eliminatedPlayers", 0))
        enemy_alive_players += float(other.get("alivePlayers", 0))
    return {
        "enemy_core_health_lost": enemy_core_health_lost,
        "enemy_eliminated_players": enemy_eliminated_players,
        "enemy_alive_players": enemy_alive_players,
    }


def team_score(run: dict[str, Any], team: dict[str, Any]) -> float:
    team_id = int(team.get("teamId", -1))
    won = 1.0 if run.get("winnerTeamId") == team.get("teamId") else 0.0
    core_alive = 1.0 if team.get("coreAlive") else 0.0
    timeout = 1.0 if run.get("timeout") else 0.0
    core_health = float(team.get("coreHealth", 0))
    core_max = max(1.0, float(team.get("coreMaxHealth", 1)))
    own_core_health = core_health / core_max
    timeline = team_progress_from_timeline(run, team_id)
    pressure = enemy_team_pressure(run, team_id)
    return (
        won * 2200.0
        + timeline["enemy_cores_destroyed"] * 620.0
        + timeline["credited_final_kills"] * 240.0
        + timeline["credited_void_finals"] * 180.0
        + timeline["first_core_damage"] * 85.0
        + pressure["enemy_core_health_lost"] * 120.0
        + pressure["enemy_eliminated_players"] * 70.0
        + float(team.get("coreDamage", 0)) * 1.55
        + float(team.get("kills", 0)) * 34.0
        + core_alive * 150.0
        + own_core_health * 80.0
        + float(team.get("alivePlayers", 0)) * 12.0
        - timeline["own_core_destroyed"] * 420.0
        - float(team.get("finalDeaths", 0)) * 85.0
        - float(team.get("deaths", 0)) * 16.0
        - float(team.get("eliminatedPlayers", 0)) * 95.0
        - timeout * 80.0
        - float(run.get("durationSeconds", 0.0)) * 0.035
    )


def merge_hall_of_fame(
    hall_of_fame: list[dict[str, Any]],
    elites: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_id: dict[str, dict[str, Any]] = {}
    for genome in hall_of_fame:
        genome_id = str(genome.get("id", ""))
        if genome_id:
            by_id[genome_id] = copy.deepcopy(genome)

    for genome in elites:
        genome_id = str(genome.get("id", ""))
        if not genome_id:
            continue

        incoming = copy.deepcopy(genome)
        incoming_score = float(incoming.get("fitness", 0.0))
        existing = by_id.get(genome_id)
        if existing is None:
            incoming["fitnessLast"] = incoming_score
            incoming["fitnessMean"] = incoming_score
            incoming["fitnessBestSeen"] = incoming_score
            incoming["evaluations"] = 1
            by_id[genome_id] = incoming
            continue

        previous_count = max(1, int(existing.get("evaluations", 1)))
        previous_mean = float(existing.get("fitnessMean", existing.get("fitness", 0.0)))
        mean = (previous_mean * previous_count + incoming_score) / float(previous_count + 1)
        incoming["fitnessLast"] = incoming_score
        incoming["fitnessMean"] = mean
        incoming["fitnessBestSeen"] = max(float(existing.get("fitnessBestSeen", previous_mean)), incoming_score)
        incoming["evaluations"] = previous_count + 1
        incoming["fitness"] = mean
        by_id[genome_id] = incoming

    merged = list(by_id.values())
    merged.sort(key=lambda item: float(item.get("fitnessMean", item.get("fitness", 0.0))), reverse=True)
    return merged[:16]


@dataclass
class TournamentTask:
    job_id: str
    group_index: int
    tuning_path: Path
    stats_path: Path
    score_indexes: list[int]
    runs: int
    speed: int
    minutes: int
    seed: int


class PersistentWorker:
    def __init__(self, index: int, args: argparse.Namespace, run_dir: Path):
        self.index = index
        self.args = args
        self.run_dir = run_dir
        self.process: subprocess.Popen[str] | None = None
        self.output: queue.Queue[str | None] = queue.Queue()
        self.stderr_file = (run_dir / f"worker_{index:02d}.stderr.log").open("a", encoding="utf-8")
        self.start()

    def start(self) -> None:
        self.stop()
        command = [str(self.args.exe_path), "--automatch-worker"]
        if self.args.biome:
            command.extend(["--biome", self.args.biome])
        self.output = queue.Queue()
        self.process = subprocess.Popen(
            command,
            cwd=self.args.cwd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            text=True,
            bufsize=1,
        )
        threading.Thread(target=self._read_output, daemon=True).start()
        ready = self.output.get(timeout=20.0)
        if ready != "WORKER_READY":
            raise RuntimeError(f"worker {self.index} failed to start: {ready!r}")

    def _read_output(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            self.output.put(None)
            return
        for line in process.stdout:
            self.output.put(line.rstrip("\r\n"))
        self.output.put(None)

    def run(self, task: TournamentTask) -> list[float]:
        process = self.process
        if process is None or process.poll() is not None or process.stdin is None:
            self.start()
            process = self.process
        assert process is not None and process.stdin is not None

        command = "\t".join(
            [
                task.job_id,
                str(task.runs),
                str(task.speed),
                str(task.minutes),
                str(task.seed),
                str(task.tuning_path),
                str(task.stats_path),
            ]
        )
        output_lines: list[str] = []
        try:
            process.stdin.write(command + "\n")
            process.stdin.flush()
            deadline = None
            if self.args.process_timeout_seconds > 0:
                deadline = time.monotonic() + self.args.process_timeout_seconds
            while True:
                timeout = None if deadline is None else max(0.01, deadline - time.monotonic())
                line = self.output.get(timeout=timeout)
                if line is None:
                    raise RuntimeError(f"worker {self.index} exited during {task.job_id}")
                output_lines.append(line)
                if line.startswith(f"WORKER_DONE\t{task.job_id}\t"):
                    if not line.endswith("\t1"):
                        raise RuntimeError(f"worker reported failed job: {line}")
                    break
                if line.startswith(f"WORKER_ERROR\t{task.job_id}\t"):
                    raise RuntimeError(line)
        except queue.Empty as exc:
            self.stop(force=True)
            raise TimeoutError(f"worker {self.index} timed out on {task.job_id}") from exc
        finally:
            write_process_log(task.tuning_path.with_suffix(".stdout.log"), "\n".join(output_lines))

        if not task.stats_path.exists():
            raise RuntimeError(f"worker completed without {task.stats_path.name}")
        stats = json.loads(task.stats_path.read_text(encoding="utf-8"))
        scores = [0.0, 0.0, 0.0, 0.0]
        counts = [0, 0, 0, 0]
        for run in stats.get("runs", []):
            for team in run.get("teams", []):
                team_id = int(team.get("teamId", -1))
                if 0 <= team_id < 4:
                    scores[team_id] += team_score(run, team)
                    counts[team_id] += 1
        return [scores[index] / max(1, counts[index]) for index in task.score_indexes]

    def stop(self, force: bool = False) -> None:
        process = self.process
        self.process = None
        if process is None or process.poll() is not None:
            return
        try:
            if not force and process.stdin is not None:
                process.stdin.write("QUIT\n")
                process.stdin.flush()
                process.wait(timeout=5.0)
            else:
                process.terminate()
                process.wait(timeout=5.0)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            process.kill()
            process.wait(timeout=5.0)

    def close(self) -> None:
        self.stop()
        self.stderr_file.close()


class WorkerPool:
    def __init__(self, args: argparse.Namespace, run_dir: Path):
        self.args = args
        self.workers = [PersistentWorker(index, args, run_dir) for index in range(args.workers)]
        self.available: queue.Queue[PersistentWorker] = queue.Queue()
        for worker in self.workers:
            self.available.put(worker)
        self.executor = ThreadPoolExecutor(max_workers=len(self.workers))

    def _run_task(self, task: TournamentTask) -> tuple[TournamentTask, list[float]]:
        worker = self.available.get()
        try:
            return task, worker.run(task)
        except Exception as exc:
            failure = {
                "reason": str(exc),
                "jobId": task.job_id,
                "tuning": str(task.tuning_path),
                "stats": str(task.stats_path),
            }
            task.tuning_path.with_suffix(".failed.json").write_text(
                json.dumps(failure, indent=2), encoding="utf-8"
            )
            print(f"tournament failed: {task.job_id}: {exc}")
            return task, [self.args.failure_penalty for _ in task.score_indexes]
        finally:
            self.available.put(worker)

    def run_all(self, tasks: list[TournamentTask]) -> list[tuple[TournamentTask, list[float]]]:
        futures = [self.executor.submit(self._run_task, task) for task in tasks]
        return [future.result() for future in as_completed(futures)]

    def close(self) -> None:
        self.executor.shutdown(wait=True)
        for worker in self.workers:
            worker.close()

    def __enter__(self) -> "WorkerPool":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def evaluation_seed(args: argparse.Namespace, generation: int, rotation: int, stage: str) -> int:
    stage_salt = 0xC2B2AE35 if stage == "final" else 0x27D4EB2F
    seed = (
        args.evaluation_seed
        + generation * 0x9E3779B9
        + rotation * 0x85EBCA6B
        + stage_salt
    ) & 0xFFFFFFFF
    return seed or 1


def evaluate_genomes(
    args: argparse.Namespace,
    pool: WorkerPool,
    run_dir: Path,
    generation: int,
    genomes: list[dict[str, Any]],
    runs: int,
    rotations: int,
    stage: str,
) -> list[float]:
    groups = [genomes[start:start + 4] for start in range(0, len(genomes), 4)]
    totals = [[0.0 for _ in group] for group in groups]
    counts = [[0 for _ in group] for group in groups]
    tasks: list[TournamentTask] = []

    for group_index, group in enumerate(groups):
        padded = [copy.deepcopy(item) for item in group]
        while len(padded) < 4:
            padded.append(copy.deepcopy(padded[-1]))
        for rotation in range(max(1, min(rotations, 4))):
            order = [(slot + rotation) % 4 for slot in range(4)]
            candidates = [copy.deepcopy(padded[index]) for index in order]
            stamp = time.time_ns()
            name = f"gen{generation:04d}_{stage}_g{group_index:03d}_r{rotation}_{stamp}"
            tuning_path = run_dir / f"{name}.json"
            stats_path = run_dir / f"{name}.stats.json"
            write_team_tuning(tuning_path, candidates)
            tasks.append(
                TournamentTask(
                    job_id=name,
                    group_index=group_index,
                    tuning_path=tuning_path,
                    stats_path=stats_path,
                    score_indexes=[order.index(index) for index in range(len(group))],
                    runs=max(1, runs),
                    speed=args.speed,
                    minutes=args.minutes,
                    seed=evaluation_seed(args, generation, rotation, stage),
                )
            )

    for task, scores in pool.run_all(tasks):
        for index, score in enumerate(scores):
            totals[task.group_index][index] += score
            counts[task.group_index][index] += 1

    result: list[float] = []
    for group_index, group in enumerate(groups):
        result.extend(
            totals[group_index][index] / max(1, counts[group_index][index])
            for index in range(len(group))
        )
    return result


def make_initial_population(size: int) -> list[dict[str, Any]]:
    population = [copy.deepcopy(DEFAULT_GENOME)]
    population[0]["id"] = "seed"
    while len(population) < size:
        population.append(mutate(DEFAULT_GENOME, 0, 0.055))
    return population


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="build/Release/DaiBed.exe")
    parser.add_argument("--cwd", default=".")
    parser.add_argument("--out", default="tuning_runs")
    parser.add_argument("--generations", type=int, default=20)
    parser.add_argument("--population", type=int, default=24)
    parser.add_argument("--elite", type=int, default=4)
    parser.add_argument("--runs-per-tournament", type=int, default=6)
    parser.add_argument("--screening-runs", type=int, default=1)
    parser.add_argument("--finalist-count", type=int, default=8)
    parser.add_argument("--hall-challengers", type=int, default=4)
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--speed", type=int, default=256)
    parser.add_argument("--minutes", type=int, default=14)
    parser.add_argument("--mutation", type=float, default=0.045)
    parser.add_argument("--biome", default="")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--failure-penalty", type=float, default=-1000000.0)
    parser.add_argument("--process-timeout-seconds", type=int, default=180)
    parser.add_argument("--screening-rotations", type=int, default=1)
    parser.add_argument("--slot-rotations", type=int, default=4)
    args = parser.parse_args()
    args.exe_path = resolve_from_cwd(args.cwd, args.exe)
    args.workers = max(1, args.workers)
    args.finalist_count = max(args.elite, min(args.population, args.finalist_count))

    if args.seed:
        random.seed(args.seed)
    args.evaluation_seed = args.seed or random.SystemRandom().randrange(1, 2**32)

    run_dir = resolve_from_cwd(args.cwd, args.out) / time.strftime("%Y%m%d_%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    population = make_initial_population(args.population)
    hall_of_fame: list[dict[str, Any]] = []

    print(
        f"workers={args.workers} screening={args.screening_runs}x{args.screening_rotations} "
        f"final={args.runs_per_tournament}x{args.slot_rotations} finalists={args.finalist_count}"
    )
    with WorkerPool(args, run_dir) as pool:
        for generation in range(args.generations):
            generation_started = time.monotonic()
            random.shuffle(population)
            scored = [copy.deepcopy(item) for item in population]
            screening_scores = evaluate_genomes(
                args,
                pool,
                run_dir,
                generation,
                scored,
                args.screening_runs,
                args.screening_rotations,
                "screen",
            )
            for genome, score in zip(scored, screening_scores):
                genome["screeningFitness"] = score
                genome["fitness"] = score
                genome["evaluationStage"] = "screening"

            screening_ranked = sorted(
                scored,
                key=lambda item: float(item.get("screeningFitness", 0.0)),
                reverse=True,
            )
            finalists = screening_ranked[: args.finalist_count]
            finalist_ids = {str(item.get("id", "")) for item in finalists}
            validation = [copy.deepcopy(item) for item in finalists]
            for champion in hall_of_fame[: max(0, args.hall_challengers)]:
                if str(champion.get("id", "")) not in finalist_ids:
                    validation.append(copy.deepcopy(champion))

            final_scores = evaluate_genomes(
                args,
                pool,
                run_dir,
                generation,
                validation,
                args.runs_per_tournament,
                args.slot_rotations,
                "final",
            )
            verified_by_id: dict[str, dict[str, Any]] = {}
            for genome, score in zip(validation, final_scores):
                genome["fitness"] = score
                genome["evaluationStage"] = "final"
                verified_by_id[str(genome.get("id", ""))] = genome

            verified_current: list[dict[str, Any]] = []
            for genome in scored:
                verified = verified_by_id.get(str(genome.get("id", "")))
                if verified is not None:
                    genome["fitness"] = verified["fitness"]
                    genome["evaluationStage"] = "final"
                    verified_current.append(genome)

            verified_current.sort(key=lambda item: float(item.get("fitness", 0.0)), reverse=True)
            elites = verified_current[: max(1, args.elite)]
            current_best = elites[0]
            hall_of_fame = merge_hall_of_fame(hall_of_fame, list(verified_by_id.values()))
            scored.sort(
                key=lambda item: (
                    item.get("evaluationStage") == "final",
                    float(item.get("fitness", 0.0)),
                ),
                reverse=True,
            )

            (run_dir / f"generation_{generation:04d}.json").write_text(
                json.dumps(scored, indent=2),
                encoding="utf-8",
            )
            (run_dir / "best.json").write_text(
                json.dumps(hall_of_fame[0], indent=2), encoding="utf-8"
            )
            elapsed = time.monotonic() - generation_started
            print(
                f"generation {generation}: "
                f"current_best={current_best['id']} fitness={current_best['fitness']:.2f} "
                f"hall_best={hall_of_fame[0]['id']} mean={hall_of_fame[0]['fitnessMean']:.2f} "
                f"elapsed={elapsed:.1f}s"
            )

            next_population = [copy.deepcopy(item) for item in elites]
            while len(next_population) < args.population:
                if random.random() < 0.35 and len(elites) >= 2:
                    child = crossover(random.choice(elites), random.choice(elites), generation + 1)
                else:
                    child = copy.deepcopy(random.choice(elites))
                next_population.append(mutate(child, generation + 1, args.mutation))
            population = next_population

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
