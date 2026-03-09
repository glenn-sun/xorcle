#!/usr/bin/env python3
from __future__ import annotations

import re
import signal
import subprocess
import time
from dataclasses import dataclass
from typing import Optional, Sequence


RESULT_RE = re.compile(r"^s\s+(SATISFIABLE|UNSATISFIABLE)\s*$", re.MULTILINE)
STATUS_LINE_RE = re.compile(r"(?im)^s\s+(.+?)\s*$")
TIME_RE = re.compile(r"(?im)^c\s+Time:\s*(\S+)\s+s\s*$")
DECISIONS_RE = re.compile(r"(?im)^c\s+decisions\s*:\s*(\S+)\b")
OOM_MARKERS = (
    "std::bad_alloc",
    "bad alloc",
    "cannot allocate memory",
    "out of memory",
    "memory exhausted",
    "memoryerror",
)
OOM_SIGNALS = {
    signal.SIGKILL,
    signal.SIGABRT,
    signal.SIGSEGV,
    signal.SIGBUS,
}
MEMORY_POLL_INTERVAL_S = 0.05


@dataclass
class ProcRun:
    returncode: Optional[int]
    stdout: str
    stderr: str
    elapsed_wall_s: float
    timed_out: bool
    memory_limited: bool
    signal_number: Optional[int]
    error_message: str = ""

    @property
    def combined_output(self) -> str:
        if self.stdout and self.stderr:
            return self.stdout + "\n" + self.stderr
        return self.stdout or self.stderr


@dataclass(frozen=True)
class StatusParse:
    status: Optional[str]
    error: str = ""


@dataclass(frozen=True)
class StatusAndTimeParse:
    status: Optional[str]
    time_seconds: Optional[float]
    status_error: str = ""
    time_error: str = ""


@dataclass(frozen=True)
class IntegerMetricParse:
    value: Optional[int]
    error: str = ""


def _classify_memory_limit(
    *,
    returncode: Optional[int],
    output: str,
    memory_limit_bytes: Optional[int],
) -> tuple[bool, Optional[int]]:
    if memory_limit_bytes is None:
        return False, None
    signal_number = -returncode if returncode is not None and returncode < 0 else None
    if signal_number in OOM_SIGNALS:
        return True, signal_number
    lowered = output.lower()
    if any(marker in lowered for marker in OOM_MARKERS):
        return True, signal_number
    return False, signal_number


def _current_rss_bytes(pid: int) -> Optional[int]:
    try:
        completed = subprocess.run(
            ["/bin/ps", "-o", "rss=", "-p", str(pid)],
            capture_output=True,
            text=True,
            timeout=1.0,
            shell=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    raw = (completed.stdout or "").strip()
    if not raw:
        return None
    try:
        return int(raw) * 1024
    except ValueError:
        return None


def _run_with_memory_watch(
    cmd: Sequence[str],
    timeout_s: Optional[float],
    memory_limit_bytes: int,
) -> ProcRun:
    start = time.perf_counter()
    try:
        process = subprocess.Popen(
            list(cmd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            shell=False,
        )
    except OSError as exc:
        return ProcRun(
            returncode=None,
            stdout="",
            stderr="",
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=False,
            memory_limited=False,
            signal_number=None,
            error_message=str(exc),
        )

    timed_out = False
    memory_limited = False
    signal_number: Optional[int] = None
    try:
        while True:
            returncode = process.poll()
            if returncode is not None:
                break
            elapsed = time.perf_counter() - start
            if timeout_s is not None and elapsed >= timeout_s:
                timed_out = True
                process.kill()
                signal_number = signal.SIGKILL
                break
            rss_bytes = _current_rss_bytes(process.pid)
            if rss_bytes is not None and rss_bytes > memory_limit_bytes:
                memory_limited = True
                process.kill()
                signal_number = signal.SIGKILL
                break
            time.sleep(MEMORY_POLL_INTERVAL_S)
        stdout, stderr = process.communicate()
    except Exception:
        process.kill()
        stdout, stderr = process.communicate()
        raise

    elapsed_wall_s = time.perf_counter() - start
    returncode = process.returncode
    if timed_out:
        return ProcRun(
            returncode=returncode,
            stdout=stdout or "",
            stderr=stderr or "",
            elapsed_wall_s=elapsed_wall_s,
            timed_out=True,
            memory_limited=False,
            signal_number=signal_number,
            error_message=f"timeout after {timeout_s}s",
        )
    if memory_limited:
        return ProcRun(
            returncode=returncode,
            stdout=stdout or "",
            stderr=stderr or "",
            elapsed_wall_s=elapsed_wall_s,
            timed_out=False,
            memory_limited=True,
            signal_number=signal_number,
            error_message=f"memory limit exceeded ({memory_limit_bytes} bytes)",
        )

    memory_limited, detected_signal = _classify_memory_limit(
        returncode=returncode,
        output=(stdout or "") + "\n" + (stderr or ""),
        memory_limit_bytes=memory_limit_bytes,
    )
    return ProcRun(
        returncode=returncode,
        stdout=stdout or "",
        stderr=stderr or "",
        elapsed_wall_s=elapsed_wall_s,
        timed_out=False,
        memory_limited=memory_limited,
        signal_number=detected_signal,
    )


def run_process(
    cmd: Sequence[str],
    timeout_s: Optional[float],
    memory_limit_bytes: Optional[int] = None,
) -> ProcRun:
    if memory_limit_bytes is not None:
        return _run_with_memory_watch(cmd, timeout_s, memory_limit_bytes)
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            list(cmd),
            capture_output=True,
            text=True,
            timeout=timeout_s,
            shell=False,
        )
        return ProcRun(
            returncode=completed.returncode,
            stdout=completed.stdout or "",
            stderr=completed.stderr or "",
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=False,
            memory_limited=False,
            signal_number=(-completed.returncode if completed.returncode < 0 else None),
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        return ProcRun(
            returncode=None,
            stdout=stdout,
            stderr=stderr,
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=True,
            memory_limited=False,
            signal_number=None,
            error_message=f"timeout after {timeout_s}s",
        )
    except OSError as exc:
        return ProcRun(
            returncode=None,
            stdout="",
            stderr="",
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=False,
            memory_limited=False,
            signal_number=None,
            error_message=str(exc),
        )


def parse_solver_status_details(output: str) -> StatusParse:
    match = RESULT_RE.search(output)
    if match is not None:
        return StatusParse("SAT" if match.group(1) == "SATISFIABLE" else "UNSAT")
    malformed_match = STATUS_LINE_RE.search(output)
    if malformed_match is None:
        return StatusParse(None)
    return StatusParse(None, f"unrecognized solver status line: {malformed_match.group(0).strip()!r}")


def parse_solver_status(output: str) -> Optional[str]:
    return parse_solver_status_details(output).status


def parse_solver_status_and_time_details(output: str) -> StatusAndTimeParse:
    status_parse = parse_solver_status_details(output)
    time_seconds = None
    time_error = ""
    match = TIME_RE.search(output)
    if match:
        try:
            time_seconds = float(match.group(1))
        except ValueError:
            time_error = f"malformed solver time metric: {match.group(1)!r}"
    return StatusAndTimeParse(
        status=status_parse.status,
        time_seconds=time_seconds,
        status_error=status_parse.error,
        time_error=time_error,
    )


def parse_solver_status_and_time_from_console(output: str) -> tuple[Optional[str], Optional[float]]:
    parsed = parse_solver_status_and_time_details(output)
    return parsed.status, parsed.time_seconds


def parse_decisions_from_output_details(output: str) -> IntegerMetricParse:
    match = DECISIONS_RE.search(output)
    if not match:
        return IntegerMetricParse(None)
    try:
        return IntegerMetricParse(int(match.group(1)))
    except ValueError:
        return IntegerMetricParse(None, f"malformed solver decisions metric: {match.group(1)!r}")


def parse_decisions_from_output(output: str) -> Optional[int]:
    return parse_decisions_from_output_details(output).value
