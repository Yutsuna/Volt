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

logger = logging.getLogger("valgrind_check")

DEFAULT_VOLT_BIN = Path("build/source/Volt/Volt/volt")
DEFAULT_SAMPLES_GLOB = "samples/Tests/*/**/*.vl"
DEFAULT_MAX_JOBS = os.cpu_count() or 4


class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    CYAN = "\033[96m"
    BOLD = "\033[1m"
    RESET = "\033[0m"


class TestStatus(enum.StrEnum):
    PASSED = "PASSED"
    LEAKED = "LEAKED"
    BUILD_FAILED = "BUILD_FAILED"
    EXECUTION_FAILED = "EXECUTION_FAILED"


@dataclass(frozen=True)
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


@dataclass
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


class VoltCompiler:
    def __init__(self, volt_bin: Path):
        self.volt_bin = volt_bin

    async def compile(self, source_file: Path, output_binary: Path) -> tuple[bool, str]:
        cmd = [str(self.volt_bin), "build", str(source_file), "-o", str(output_binary)]

        process = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate()

        success = process.returncode == 0
        error_output = (stderr or stdout).decode(errors="ignore") if not success else ""
        return success, error_output


class ValgrindRunner:
    @staticmethod
    async def run(binary_path: Path) -> tuple[int, str]:
        cmd = [
            "valgrind",
            "--leak-check=full",
            "--show-leak-kinds=all",
            str(binary_path),
        ]

        process = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate()
        combined_output = stderr.decode(errors="ignore") + "\n" + stdout.decode(errors="ignore")
        return process.returncode, combined_output


class TestCaseWorker:
    def __init__(self, compiler: VoltCompiler):
        self.compiler = compiler

    async def execute(self, file_path: Path, semaphore: asyncio.Semaphore) -> TestResult:
        async with semaphore:
            start_time = time.perf_counter()

            with tempfile.NamedTemporaryFile(delete=False, suffix=".out") as tmp:
                temp_bin = Path(tmp.name)

            try:
                compiled, build_err = await self.compiler.compile(file_path, temp_bin)
                if not compiled:
                    return TestResult(
                        file_path=file_path,
                        status=TestStatus.BUILD_FAILED,
                        duration_seconds=time.perf_counter() - start_time,
                        error_message=build_err,
                    )

                _, valgrind_out = await ValgrindRunner.run(temp_bin)
                metrics = ValgrindAnalyzer.analyze(valgrind_out)

                duration = time.perf_counter() - start_time

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
        return [Path(p).resolve() for p in Path().glob(self.glob_pattern)]

    async def run_all(self) -> list[TestResult]:
        files = self.discover_files()
        if not files:
            logger.warning("No files matched the specified pattern: %s", self.glob_pattern)
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
                print(f"[{Colors.GREEN}PASSED{Colors.RESET}] {result.file_path} {duration_str}")
            case TestStatus.LEAKED:
                print(f"[{Colors.RED}LEAKED{Colors.RESET}] {result.file_path} {duration_str}")
                print(f"  └─ Total Leaked: {result.metrics.total_bytes} bytes")
                print(f"     ├── Definitely Lost : {result.metrics.definitely_lost} B")
                print(f"     ├── Indirectly Lost : {result.metrics.indirectly_lost} B")
                print(f"     ├── Possibly Lost   : {result.metrics.possibly_lost} B")
                print(f"     └── Still Reachable : {result.metrics.still_reachable} B")
                if verbose and result.raw_valgrind_output:
                    print(f"\n--- Valgrind Log [{result.file_path.name}] ---\n{result.raw_valgrind_output}")
            case TestStatus.BUILD_FAILED:
                print(f"[{Colors.YELLOW}BUILD FAILED{Colors.RESET}] {result.file_path} {duration_str}")
                if result.error_message:
                    print(f"  └─ Error: {result.error_message.strip()}")
            case TestStatus.EXECUTION_FAILED:
                print(f"[{Colors.RED}EXEC ERROR{Colors.RESET}] {result.file_path} {duration_str}")
                if result.error_message:
                    print(f"  └─ Details: {result.error_message}")

    @classmethod
    def print_summary(cls, results: list[TestResult]) -> None:
        total = len(results)
        passed = sum(1 for r in results if r.status == TestStatus.PASSED)
        leaked = sum(1 for r in results if r.status == TestStatus.LEAKED)
        build_err = sum(1 for r in results if r.status == TestStatus.BUILD_FAILED)
        exec_err = sum(1 for r in results if r.status == TestStatus.EXECUTION_FAILED)

        print(f"\n{Colors.BOLD}{Colors.CYAN}=== TEST SUITE SUMMARY ==={Colors.RESET}")
        print(f"Total Evaluated : {total}")
        print(f"Passed          : {Colors.GREEN}{passed}{Colors.RESET}")
        print(f"Memory Leaks    : {Colors.RED}{leaked}{Colors.RESET}")
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

    if not args.volt_bin.exists():
        print(f"{Colors.RED}Error: Volt binary not found at '{args.volt_bin}'{Colors.RESET}", file=sys.stderr)
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
        print(f"\n{Colors.YELLOW}Execution interrupted by user. Exiting...{Colors.RESET}")
        sys.exit(130)


if __name__ == "__main__":
    main()
