#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import enum
import json
import logging
import os
import re
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shared import (
    Colors,
    ProcessResult,
    VoltCompiler,
    discover_source_files,
    run_process_async,
)

logger = logging.getLogger("valgrind_check")

DEFAULT_VOLT_BIN = Path("build/source/Volt/Volt/volt")
DEFAULT_SAMPLES_GLOB = "samples/Tests/*/**/*.vl"
DEFAULT_MAX_JOBS = os.cpu_count() or 4
VALGRIND_ERROR_EXIT_CODE = 99


class TestStatus(enum.StrEnum):
    PASSED = "PASSED"
    LEAKED = "LEAKED"
    MEMORY_ERROR = "MEMORY_ERROR"
    BUILD_FAILED = "BUILD_FAILED"
    EXECUTION_FAILED = "EXECUTION_FAILED"


@dataclass(frozen=True, slots=True)
class LeakMetrics:
    definitely_lost: int = 0
    indirectly_lost: int = 0
    possibly_lost: int = 0
    still_reachable: int = 0

    @property
    def total_bytes(self) -> int:
        return (
            self.definitely_lost
            + self.indirectly_lost
            + self.possibly_lost
            + self.still_reachable
        )

    @property
    def has_leaks(self) -> bool:
        return self.total_bytes > 0


@dataclass(slots=True)
class TestResult:
    file_path: Path
    status: TestStatus
    duration_seconds: float
    metrics: LeakMetrics = LeakMetrics()
    error_message: str | None = None
    raw_valgrind_output: str | None = None

    def to_dict(self) -> dict:
        data = asdict(self)
        data["file_path"] = str(self.file_path)
        data["status"] = self.status.value
        return data


class ValgrindAnalyzer:
    _LEAK_REGEX = re.compile(
        r"(definitely lost|indirectly lost|possibly lost|still reachable):\s+([0-9,]+)\s+bytes"
    )

    @classmethod
    def analyze(cls, output: str) -> LeakMetrics:
        if "all heap blocks were freed -- no leaks are possible" in output:
            return LeakMetrics()

        counts: dict[str, int] = {
            "definitely lost": 0,
            "indirectly lost": 0,
            "possibly lost": 0,
            "still reachable": 0,
        }

        for category, bytes_str in cls._LEAK_REGEX.findall(output):
            amount = int(bytes_str.replace(",", ""))
            if category in counts:
                counts[category] = amount

        return LeakMetrics(
            definitely_lost=counts["definitely lost"],
            indirectly_lost=counts["indirectly lost"],
            possibly_lost=counts["possibly lost"],
            still_reachable=counts["still reachable"],
        )


class ValgrindRunner:
    @staticmethod
    async def run(binary_path: Path) -> ProcessResult:
        cmd = [
            "valgrind",
            "--leak-check=full",
            "--show-leak-kinds=all",
            f"--error-exitcode={VALGRIND_ERROR_EXIT_CODE}",
            str(binary_path),
        ]
        return await run_process_async(cmd)


class TestCaseWorker:
    def __init__(self, compiler: VoltCompiler):
        self.compiler = compiler

    async def execute(
        self, file_path: Path, semaphore: asyncio.Semaphore
    ) -> TestResult:
        async with semaphore:
            start_time = time.perf_counter()

            with tempfile.NamedTemporaryFile(delete=False, suffix=".out") as tmp:
                temp_bin = Path(tmp.name)

            try:
                build_result = await self.compiler.compile_async(file_path, temp_bin)
                if not build_result.success:
                    return TestResult(
                        file_path=file_path,
                        status=TestStatus.BUILD_FAILED,
                        duration_seconds=time.perf_counter() - start_time,
                        error_message=build_result.combined_output,
                    )

                valgrind_result = await ValgrindRunner.run(temp_bin)
                valgrind_out = valgrind_result.combined_output
                metrics = ValgrindAnalyzer.analyze(valgrind_out)
                duration = time.perf_counter() - start_time

                if valgrind_result.returncode == VALGRIND_ERROR_EXIT_CODE:
                    return TestResult(
                        file_path=file_path,
                        status=TestStatus.MEMORY_ERROR,
                        duration_seconds=duration,
                        metrics=metrics,
                        raw_valgrind_output=valgrind_out,
                    )

                if metrics.has_leaks:
                    return TestResult(
                        file_path=file_path,
                        status=TestStatus.LEAKED,
                        duration_seconds=duration,
                        metrics=metrics,
                        raw_valgrind_output=valgrind_out,
                    )

                return TestResult(
                    file_path=file_path,
                    status=TestStatus.PASSED,
                    duration_seconds=duration,
                    metrics=metrics,
                )

            except Exception as exc:
                return TestResult(
                    file_path=file_path,
                    status=TestStatus.EXECUTION_FAILED,
                    duration_seconds=time.perf_counter() - start_time,
                    error_message=str(exc),
                )
            finally:
                if temp_bin.exists():
                    temp_bin.unlink()


class TestSuiteRunner:
    def __init__(self, volt_bin: Path, glob_pattern: str, max_jobs: int):
        self.compiler = VoltCompiler(volt_bin)
        self.glob_pattern = glob_pattern
        self.max_jobs = max_jobs
        self.worker = TestCaseWorker(self.compiler)

    def discover_files(self) -> list[Path]:
        return discover_source_files(Path.cwd(), self.glob_pattern)

    async def run_all(self) -> list[TestResult]:
        files = self.discover_files()
        if not files:
            logger.warning(
                "No files matched the specified pattern: %s", self.glob_pattern
            )
            return []

        logger.info("Discovered %d test file(s). Starting execution...", len(files))
        semaphore = asyncio.Semaphore(self.max_jobs)

        async with asyncio.TaskGroup() as tg:
            tasks = [tg.create_task(self.worker.execute(f, semaphore)) for f in files]

        return [task.result() for task in tasks]


class ConsoleReporter:
    @staticmethod
    def print_result(result: TestResult, verbose: bool = False) -> None:
        duration_str = f"({result.duration_seconds:.2f}s)"

        match result.status:
            case TestStatus.PASSED:
                print(
                    f"[{Colors.GREEN}PASSED{Colors.RESET}] {result.file_path} {duration_str}"
                )
            case TestStatus.LEAKED:
                print(
                    f"[{Colors.RED}LEAKED{Colors.RESET}] {result.file_path} {duration_str}"
                )
                print(f"  └─ Total Leaked: {result.metrics.total_bytes} bytes")
                print(f"     ├── Definitely Lost : {result.metrics.definitely_lost} B")
                print(f"     ├── Indirectly Lost : {result.metrics.indirectly_lost} B")
                print(f"     ├── Possibly Lost   : {result.metrics.possibly_lost} B")
                print(f"     └── Still Reachable : {result.metrics.still_reachable} B")
                if verbose and result.raw_valgrind_output:
                    print(
                        f"\n--- Valgrind Log [{result.file_path.name}] ---\n{result.raw_valgrind_output}"
                    )
            case TestStatus.MEMORY_ERROR:
                print(
                    f"[{Colors.RED}MEMORY ERROR{Colors.RESET}] {result.file_path} {duration_str}"
                )
                print(
                    "  └─ valgrind reported an error (invalid read/write, double free, etc.) — rerun with -v for the log"
                )
                if verbose and result.raw_valgrind_output:
                    print(
                        f"\n--- Valgrind Log [{result.file_path.name}] ---\n{result.raw_valgrind_output}"
                    )
            case TestStatus.BUILD_FAILED:
                print(
                    f"[{Colors.YELLOW}BUILD FAILED{Colors.RESET}] {result.file_path} {duration_str}"
                )
                if result.error_message:
                    print(f"  └─ Error: {result.error_message.strip()}")
            case TestStatus.EXECUTION_FAILED:
                print(
                    f"[{Colors.RED}EXEC ERROR{Colors.RESET}] {result.file_path} {duration_str}"
                )
                if result.error_message:
                    print(f"  └─ Details: {result.error_message}")

    @classmethod
    def print_summary(cls, results: list[TestResult]) -> None:
        total = len(results)
        passed = sum(1 for r in results if r.status == TestStatus.PASSED)
        leaked = sum(1 for r in results if r.status == TestStatus.LEAKED)
        mem_err = sum(1 for r in results if r.status == TestStatus.MEMORY_ERROR)
        build_err = sum(1 for r in results if r.status == TestStatus.BUILD_FAILED)
        exec_err = sum(1 for r in results if r.status == TestStatus.EXECUTION_FAILED)

        print(f"\n{Colors.BOLD}{Colors.CYAN}=== TEST SUITE SUMMARY ==={Colors.RESET}")
        print(f"Total Evaluated : {total}")
        print(f"Passed          : {Colors.GREEN}{passed}{Colors.RESET}")
        print(f"Memory Leaks    : {Colors.RED}{leaked}{Colors.RESET}")
        print(f"Memory Errors   : {Colors.RED}{mem_err}{Colors.RESET}")
        print(f"Build Failures  : {Colors.YELLOW}{build_err}{Colors.RESET}")
        print(f"Exec Errors     : {Colors.RED}{exec_err}{Colors.RESET}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Parallel Volt compiler memory leak test runner powered by Valgrind.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--volt-bin",
        type=Path,
        default=DEFAULT_VOLT_BIN,
        help="Path to the Volt binary executable",
    )
    parser.add_argument(
        "--glob",
        type=str,
        default=DEFAULT_SAMPLES_GLOB,
        help="Glob pattern matching test files",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=DEFAULT_MAX_JOBS,
        help="Maximum concurrent test jobs",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Display full Valgrind output on memory leak detection",
    )
    parser.add_argument(
        "--json-report",
        type=Path,
        metavar="FILE",
        help="Export full test output in JSON format to specified path",
    )
    return parser.parse_args()


async def async_main() -> int:
    args = parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    compiler = VoltCompiler(args.volt_bin)
    if not compiler.exists():
        print(
            f"{Colors.RED}Error: Volt binary not found at '{args.volt_bin}'{Colors.RESET}",
            file=sys.stderr,
        )
        return 2

    runner = TestSuiteRunner(
        volt_bin=args.volt_bin,
        glob_pattern=args.glob,
        max_jobs=args.jobs,
    )

    results = await runner.run_all()

    if not results:
        return 0

    print()
    for res in results:
        ConsoleReporter.print_result(res, verbose=args.verbose)

    ConsoleReporter.print_summary(results)

    if args.json_report:
        report_data = [r.to_dict() for r in results]
        args.json_report.write_text(json.dumps(report_data, indent=2))
        print(f"\nReport exported to: {args.json_report.resolve()}")

    has_failures = any(r.status != TestStatus.PASSED for r in results)
    return 1 if has_failures else 0


def main() -> None:
    try:
        sys.exit(asyncio.run(async_main()))
    except KeyboardInterrupt:
        print(
            f"\n{Colors.YELLOW}Execution interrupted by user. Exiting...{Colors.RESET}"
        )
        sys.exit(130)


if __name__ == "__main__":
    main()
