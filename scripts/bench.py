#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import logging
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, final

# Ensure scripts directory is on sys.path for shared imports
sys.path.insert(0, str(Path(__file__).resolve().parent))

from shared import (
    Colors,
    VoltCompiler,
    discover_source_files,
    run_process_sync,
)

logger = logging.getLogger("bench")

DEFAULT_VOLT_BIN = Path("build/source/Volt/Volt/volt")
DEFAULT_BENCH_DIR = Path("samples/Bench")
DEFAULT_WARMUP_RUNS = 1
DEFAULT_BENCH_RUNS = 3


@dataclass(frozen=True, slots=True)
class SingleOptResult:
    compile_time_ms: float
    exec_time_ms: float
    binary_size_bytes: int


@dataclass(slots=True)
class BenchResult:
    name: str
    file: str
    o0: SingleOptResult
    o2: SingleOptResult

    @property
    def speedup_o2(self) -> float:
        if self.o2.exec_time_ms <= 0:
            return 1.0
        return self.o0.exec_time_ms / self.o2.exec_time_ms

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class BenchmarkWorker:
    def __init__(self, compiler: VoltCompiler, warmup_runs: int, bench_runs: int):
        self.compiler = compiler
        self.warmup_runs = warmup_runs
        self.bench_runs = bench_runs

    def measure(self, sample_file: Path) -> BenchResult:
        name = sample_file.stem
        results: dict[int, SingleOptResult] = {}

        with tempfile.TemporaryDirectory() as tmpdir:
            for opt_level in (0, 2):
                bin_path = Path(tmpdir) / f"{name}_O{opt_level}"

                compile_times: list[float] = []
                total_compile_runs = self.warmup_runs + self.bench_runs
                for run_idx in range(total_compile_runs):
                    result = self.compiler.compile_sync(
                        sample_file, bin_path, opt_level=opt_level
                    )
                    if not result.success:
                        print(
                            f"{Colors.RED}Compilation failed for {name} -O {opt_level}:{Colors.RESET}\n"
                            + f"{result.combined_output}",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    if run_idx >= self.warmup_runs:
                        compile_times.append(result.elapsed_ms)

                avg_compile_ms = sum(compile_times) / len(compile_times)
                bin_size = bin_path.stat().st_size if bin_path.exists() else 0

                exec_times: list[float] = []
                total_exec_runs = self.warmup_runs + self.bench_runs
                for run_idx in range(total_exec_runs):
                    exec_result = run_process_sync([bin_path])
                    if not exec_result.success:
                        print(
                            f"{Colors.RED}Execution failed for {name} -O {opt_level}:{Colors.RESET}\n"
                            + f"{exec_result.combined_output}",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    if run_idx >= self.warmup_runs:
                        exec_times.append(exec_result.elapsed_ms)

                avg_exec_ms = sum(exec_times) / len(exec_times)
                results[opt_level] = SingleOptResult(
                    compile_time_ms=avg_compile_ms,
                    exec_time_ms=avg_exec_ms,
                    binary_size_bytes=bin_size,
                )

        return BenchResult(
            name=name,
            file=str(sample_file),
            o0=results[0],
            o2=results[2],
        )


@final
class BenchSuiteRunner:
    def __init__(
        self,
        volt_bin: Path,
        bench_dir: Path,
        warmup_runs: int = DEFAULT_WARMUP_RUNS,
        bench_runs: int = DEFAULT_BENCH_RUNS,
    ):
        self.compiler = VoltCompiler(volt_bin)
        self.bench_dir = bench_dir
        self.worker = BenchmarkWorker(self.compiler, warmup_runs, bench_runs)

    def discover_files(self) -> list[Path]:
        return discover_source_files(self.bench_dir, "*.vl")

    def run_all(self) -> list[BenchResult]:
        files = self.discover_files()
        if not files:
            print(
                f"{Colors.RED}No benchmarks found in {self.bench_dir}{Colors.RESET}",
                file=sys.stderr,
            )
            sys.exit(1)

        print(
            f"{Colors.CYAN}Running benchmarks with {self.compiler.volt_bin} ({len(files)} files)...{Colors.RESET}"
        )
        results: list[BenchResult] = []
        for file in files:
            print(f"  Measuring {file.name}...", flush=True)
            results.append(self.worker.measure(file))
        return results


class ConsoleReporter:
    @staticmethod
    def print_table(
        results: list[BenchResult], baseline: dict[str, Any] | None = None
    ) -> None:
        header = (
            f"\n{Colors.BOLD}{'Benchmark':<24} | "
            f"{'Compile -O0':<12} | "
            f"{'Compile -O2':<12} | "
            f"{'Exec -O0':<12} | "
            f"{'Exec -O2':<12} | "
            f"{'Speedup O2':<10}{Colors.RESET}"
        )
        print(header)
        print("-" * 92)

        for r in results:
            speedup = r.speedup_o2
            speedup_col = Colors.GREEN if speedup > 1.05 else ""
            line = (
                f"{r.name:<24} | "
                f"{r.o0.compile_time_ms:>8.1f} ms  | "
                f"{r.o2.compile_time_ms:>8.1f} ms  | "
                f"{r.o0.exec_time_ms:>8.1f} ms  | "
                f"{r.o2.exec_time_ms:>8.1f} ms  | "
                f"{speedup_col}{speedup:>8.2f}x{Colors.RESET}"
            )
            print(line)

            if baseline and r.name in baseline:
                base = baseline[r.name]
                comp_o0_diff = (
                    (r.o0.compile_time_ms - base["o0"]["compile_time_ms"])
                    / base["o0"]["compile_time_ms"]
                    * 100.0
                )
                comp_o2_diff = (
                    (r.o2.compile_time_ms - base["o2"]["compile_time_ms"])
                    / base["o2"]["compile_time_ms"]
                    * 100.0
                )
                exec_o0_diff = (
                    (r.o0.exec_time_ms - base["o0"]["exec_time_ms"])
                    / base["o0"]["exec_time_ms"]
                    * 100.0
                )
                exec_o2_diff = (
                    (r.o2.exec_time_ms - base["o2"]["exec_time_ms"])
                    / base["o2"]["exec_time_ms"]
                    * 100.0
                )

                def fmt_diff(d: float) -> str:
                    col = (
                        Colors.GREEN
                        if d < -3.0
                        else (Colors.RED if d > 3.0 else Colors.YELLOW)
                    )
                    return f"{col}{d:>+7.1f}%{Colors.RESET}"

                diff_line = (
                    f"  {'vs baseline':<22} | "
                    f"{fmt_diff(comp_o0_diff):>17}  | "
                    f"{fmt_diff(comp_o2_diff):>17}  | "
                    f"{fmt_diff(exec_o0_diff):>17}  | "
                    f"{fmt_diff(exec_o2_diff):>17}  |"
                )
                print(diff_line)

        print("-" * 92)


class BaselineManager:
    @staticmethod
    def load(path: Path) -> dict[str, Any] | None:
        if not path.exists():
            print(
                f"{Colors.YELLOW}Warning: Compare baseline file {path} not found.{Colors.RESET}"
            )
            return None
        with open(path, "r", encoding="utf-8") as file:
            return json.load(file)

    @staticmethod
    def save(path: Path, results: list[BenchResult]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        data = {r.name: r.to_dict() for r in results}
        with open(path, "w", encoding="utf-8") as file:
            json.dump(data, file, indent=2)
        print(f"{Colors.GREEN}Saved baseline to {path}{Colors.RESET}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Volt Compiler & Runtime Benchmark Runner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--volt",
        type=Path,
        default=DEFAULT_VOLT_BIN,
        help="Path to volt binary executable",
    )
    parser.add_argument(
        "--bench-dir",
        type=Path,
        default=DEFAULT_BENCH_DIR,
        help="Directory containing .vl benchmarks",
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        metavar="FILE",
        help="Save benchmark results to JSON baseline file",
    )
    parser.add_argument(
        "--compare",
        type=Path,
        metavar="FILE",
        help="Compare results against baseline JSON file",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=DEFAULT_WARMUP_RUNS,
        help="Number of warmup runs per benchmark",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=DEFAULT_BENCH_RUNS,
        help="Number of measured benchmark runs",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    compiler = VoltCompiler(args.volt)
    if not compiler.exists():
        print(
            f"{Colors.RED}Volt binary not found at {args.volt}. Build it first: ninja -C build{Colors.RESET}",
            file=sys.stderr,
        )
        sys.exit(1)

    runner = BenchSuiteRunner(
        volt_bin=args.volt,
        bench_dir=args.bench_dir,
        warmup_runs=args.warmup,
        bench_runs=args.runs,
    )
    results = runner.run_all()

    baseline_data = BaselineManager.load(args.compare) if args.compare else None
    ConsoleReporter.print_table(results, baseline_data)

    if args.baseline:
        BaselineManager.save(args.baseline, results)


if __name__ == "__main__":
    main()
