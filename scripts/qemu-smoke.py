#!/usr/bin/env python3
"""Headless, monitor-driven integration smoke test for NostaluxOS."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time


class SmokeFailure(RuntimeError):
    """A smoke-test assertion failed."""


KERNEL_RIP_BASE = 0x00100000
KERNEL_RIP_LIMIT = 0x10000000
PAGING_USER_BASE = 0x0000010000000000
PAGING_USER_LIMIT = 0x0000020000000000
RFLAGS_NT = 1 << 14
RFLAGS_PROBE_MARKER = 0x52464C4147534F4B
STACK_PROBE_MARKER = 0x535441434B424144


def remaining(deadline: float) -> float:
    return max(0.0, deadline - time.monotonic())


def require_time(deadline: float, operation: str) -> None:
    if remaining(deadline) <= 0:
        raise SmokeFailure(f"timed out while {operation}")


def copy_image(
    source: Path, destination: Path, lock_path: Path, deadline: float, lock_timeout: float
) -> str:
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_deadline = min(deadline, time.monotonic() + lock_timeout)
    with lock_path.open("a+b") as lock:
        while True:
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_SH | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if remaining(lock_deadline) <= 0:
                    raise SmokeFailure(
                        "the primary image is in use; close the interactive QEMU "
                        "session or wait for the build to finish"
                    )
                time.sleep(min(0.05, remaining(lock_deadline)))

        digest = hashlib.sha256()
        try:
            with source.open("rb") as reader, destination.open("xb") as writer:
                while True:
                    require_time(deadline, "copying the disposable image")
                    block = reader.read(1024 * 1024)
                    if not block:
                        break
                    writer.write(block)
                    digest.update(block)
                writer.flush()
                os.fsync(writer.fileno())
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    # QEMU also receives snapshot=on, but a read-only disposable base makes an
    # accidental direct write fail instead of reaching either disk image.
    destination.chmod(0o444)
    return digest.hexdigest()


class QmpClient:
    def __init__(self, connection: socket.socket, deadline: float):
        self.connection = connection
        self.deadline = deadline
        self.buffer = b""
        self.next_id = 1

    def _message(self) -> dict:
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line = self.buffer[:newline].strip()
                self.buffer = self.buffer[newline + 1 :]
                if line:
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError as error:
                        raise SmokeFailure(f"QMP returned invalid JSON: {error}") from error

            require_time(self.deadline, "waiting for QEMU monitor output")
            self.connection.settimeout(min(0.5, remaining(self.deadline)))
            try:
                received = self.connection.recv(65536)
            except socket.timeout:
                continue
            if not received:
                raise SmokeFailure("QEMU closed its monitor connection")
            self.buffer += received

    def greeting(self) -> None:
        while True:
            message = self._message()
            if "QMP" in message:
                return

    def execute(self, command: str, arguments: dict | None = None):
        request_id = self.next_id
        self.next_id += 1
        request: dict = {"execute": command, "id": request_id}
        if arguments is not None:
            request["arguments"] = arguments
        encoded = json.dumps(request, separators=(",", ":")).encode() + b"\n"
        self.connection.sendall(encoded)

        while True:
            message = self._message()
            if message.get("id") != request_id:
                continue
            if "error" in message:
                description = message["error"].get("desc", str(message["error"]))
                raise SmokeFailure(f"QMP {command} failed: {description}")
            return message.get("return")

    def hmp(self, command: str) -> str:
        result = self.execute(
            "human-monitor-command", {"command-line": command}
        )
        if not isinstance(result, str):
            raise SmokeFailure(f"QEMU monitor returned no text for {command!r}")
        return result


def connect_qmp(socket_path: Path, process: subprocess.Popen, deadline: float) -> QmpClient:
    while True:
        require_time(deadline, "connecting to the QEMU monitor")
        if process.poll() is not None:
            raise SmokeFailure(f"QEMU exited before monitor startup (status {process.returncode})")
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.connect(str(socket_path))
            client = QmpClient(connection, deadline)
            client.greeting()
            client.execute("qmp_capabilities")
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            connection.close()
            time.sleep(min(0.05, remaining(deadline)))


def register_value(registers: str, name: str) -> int | None:
    match = re.search(
        rf"\b{re.escape(name)}\s*=([0-9a-fA-F]+)\b", registers
    )
    return int(match.group(1), 16) if match else None


def wait_for_kernel(qmp: QmpClient, deadline: float) -> tuple[int, str]:
    while True:
        require_time(deadline, "waiting for the NostaluxOS shell")
        registers = qmp.hmp("info registers")
        rip = register_value(registers, "RIP")
        cr0 = register_value(registers, "CR0")
        efer = register_value(registers, "EFER")
        long_mode = (
            rip is not None
            and cr0 is not None
            and efer is not None
            and bool(cr0 & (1 << 31))
            and bool(efer & (1 << 10))
            and "CPL=0" in registers
            and "CS64" in registers
        )
        if (
            long_mode
            and KERNEL_RIP_BASE <= rip < KERNEL_RIP_LIMIT
            and re.search(r"\bHLT=1\b", registers)
        ):
            return rip, registers
        time.sleep(min(0.03, remaining(deadline)))


def wait_for_file(path: Path, deadline: float) -> None:
    while not path.is_file() or path.stat().st_size == 0:
        require_time(deadline, f"waiting for {path.name}")
        time.sleep(min(0.02, remaining(deadline)))


def validate_framebuffer(path: Path) -> tuple[int, int, int]:
    with path.open("rb") as ppm:
        magic = ppm.readline().strip()
        dimensions = ppm.readline().split()
        maximum = ppm.readline().strip()
        if magic != b"P6" or len(dimensions) != 2 or maximum != b"255":
            raise SmokeFailure("QEMU screendump is not an 8-bit binary PPM")
        width, height = (int(value) for value in dimensions)
        pixels = ppm.read()

    expected = width * height * 3
    if width < 640 or height < 480 or len(pixels) != expected:
        raise SmokeFailure(
            f"unexpected framebuffer dump: {width}x{height}, {len(pixels)} data bytes"
        )

    colors: dict[bytes, int] = {}
    for offset in range(0, len(pixels), 3):
        color = pixels[offset : offset + 3]
        colors[color] = colors.get(color, 0) + 1
    dominant = max(colors.values(), default=0)
    changed_pixels = width * height - dominant
    if len(colors) < 2 or changed_pixels < 128:
        raise SmokeFailure("the framebuffer is blank or effectively uniform")
    return width, height, len(colors)


KEY_NAMES = {" ": "spc", "-": "minus"}


def type_command(qmp: QmpClient, command: str, deadline: float) -> None:
    for character in command:
        require_time(deadline, f"typing {command!r}")
        key = KEY_NAMES.get(character, character)
        if (
            not ("a" <= key <= "z")
            and not ("0" <= key <= "9")
            and key not in KEY_NAMES.values()
        ):
            raise SmokeFailure(f"no smoke-test key mapping for {character!r}")
        qmp.hmp(f"sendkey {key}")
        time.sleep(min(0.035, remaining(deadline)))
    qmp.hmp("sendkey ret")


def wait_for_app(
    qmp: QmpClient,
    deadline: float,
    *,
    required_register_values: dict[str, int] | None = None,
    required_rflags_bits: int = 0,
) -> tuple[int, str]:
    required_register_values = required_register_values or {}
    while True:
        require_time(deadline, "waiting for the isolated app to execute")
        # Use one all-register snapshot for both privilege/RIP detection and
        # initial-state validation. A second monitor query could run after a
        # timer preemption and accidentally inspect kernel FPU state.
        registers = qmp.hmp("info registers -a")
        rip = register_value(registers, "RIP")
        app_mode = (
            rip is not None
            and "CPL=3" in registers
            and "CS64" in registers
            and PAGING_USER_BASE <= rip < PAGING_USER_LIMIT
        )
        registers_match = all(
            register_value(registers, name) == expected
            for name, expected in required_register_values.items()
        )
        rflags = register_value(registers, "RFL")
        if rflags is None:
            rflags = register_value(registers, "RFLAGS")
        rflags_match = (
            required_rflags_bits == 0
            or (
                rflags is not None
                and rflags & required_rflags_bits == required_rflags_bits
            )
        )
        if app_mode and registers_match and rflags_match:
            return rip, registers
        time.sleep(min(0.005, remaining(deadline)))


def observe_invalid_stack_probe(
    qmp: QmpClient, observation_deadline: float
) -> tuple[int, int] | None:
    # This probe faults on the first timer interrupt, so use the smaller
    # general-register snapshot and poll without the FPU dump.
    while remaining(observation_deadline) > 0:
        registers = qmp.hmp("info registers")
        rip = register_value(registers, "RIP")
        stack_pointer = register_value(registers, "RSP")
        marker = register_value(registers, "R15")
        if (
            rip is not None
            and stack_pointer is not None
            and marker == STACK_PROBE_MARKER
            and "CPL=3" in registers
            and "CS64" in registers
            and PAGING_USER_BASE <= rip < PAGING_USER_LIMIT
            and not PAGING_USER_BASE <= stack_pointer < PAGING_USER_LIMIT
        ):
            return rip, stack_pointer
    return None


def validate_initial_user_registers(registers: str) -> int:
    stack_pointer = register_value(registers, "RSP")
    if stack_pointer is None or stack_pointer & 0xF != 8:
        raise SmokeFailure(
            "ring-3 entry stack does not satisfy the SysV function-entry "
            "alignment"
        )

    expected_zero = (
        "RAX",
        "RBX",
        "RCX",
        "RDX",
        "RSI",
        "RDI",
        "RBP",
        "R8",
        "R9",
        "R10",
        "R11",
        "R12",
        "R13",
        "R14",
        "R15",
    )
    for name in expected_zero:
        value = register_value(registers, name)
        if value is None:
            raise SmokeFailure(f"QEMU omitted ring-3 register {name}")
        if value != 0:
            raise SmokeFailure(
                f"ring-3 process inherited nonzero {name}=0x{value:x}"
            )
    return stack_pointer


def validate_initial_extended_state(registers: str) -> None:
    expected_scalars = {
        "FCW": 0x037F,
        "FSW": 0,
        "FTW": 0,
        "MXCSR": 0x1F80,
    }
    for name, expected in expected_scalars.items():
        value = register_value(registers, name)
        if value is None:
            raise SmokeFailure(f"QEMU omitted extended register {name}")
        if value != expected:
            raise SmokeFailure(
                f"ring-3 process inherited unexpected {name}=0x{value:x}"
            )

    for index in range(8):
        match = re.search(
            rf"\bFPR{index}\s*=([0-9a-fA-F]{{16}})\s*"
            rf"([0-9a-fA-F]{{4}})\b",
            registers,
        )
        if match is None:
            raise SmokeFailure(f"QEMU omitted ring-3 FPR{index}")
        if int(match.group(1), 16) != 0 or int(match.group(2), 16) != 0:
            raise SmokeFailure(
                f"ring-3 process inherited nonzero FPR{index}"
            )

    for index in range(16):
        labels = (f"XMM{index:02d}", f"XMM{index}")
        match = None
        for label in labels:
            match = re.search(
                rf"\b{label}\s*=([0-9a-fA-F]{{16}})\s*"
                rf"([0-9a-fA-F]{{16}})\b",
                registers,
            )
            if match is not None:
                break
        if match is None:
            raise SmokeFailure(f"QEMU omitted ring-3 XMM{index}")
        if int(match.group(1), 16) != 0 or int(match.group(2), 16) != 0:
            raise SmokeFailure(
                f"ring-3 process inherited nonzero XMM{index}"
            )


def qemu_log_tail(log_path: Path, maximum: int = 4096) -> str:
    try:
        data = log_path.read_bytes()
    except OSError:
        return ""
    return data[-maximum:].decode(errors="replace").strip()


def stop_qemu(process: subprocess.Popen, qmp: QmpClient | None) -> None:
    if process.poll() is not None:
        return
    if qmp is not None:
        try:
            qmp.execute("quit")
        except (OSError, SmokeFailure):
            pass
    try:
        process.wait(timeout=1.5)
        return
    except subprocess.TimeoutExpired:
        process.terminate()
    try:
        process.wait(timeout=1.5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=1.5)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-args", default="")
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--image-lock", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--lock-timeout", type=float, default=3.0)
    parser.add_argument("--memory-mib", type=int, default=128)
    args = parser.parse_args()
    if args.timeout < 10:
        parser.error("--timeout must be at least 10 seconds")
    if args.lock_timeout < 0:
        parser.error("--lock-timeout cannot be negative")
    if args.memory_mib < 16:
        parser.error("--memory-mib must be at least 16")
    return args


def main() -> int:
    args = parse_arguments()
    image = args.image.resolve()
    lock_path = args.image_lock.resolve()
    if not image.is_file():
        raise SmokeFailure(f"required build artifact is missing: {image}")

    qemu = shutil.which(args.qemu)
    if qemu is None:
        candidate = Path(args.qemu)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            qemu = str(candidate.resolve())
        else:
            raise SmokeFailure(f"QEMU executable not found: {args.qemu}")

    started = time.monotonic()
    deadline = started + args.timeout
    process: subprocess.Popen | None = None
    qmp: QmpClient | None = None
    phase = "preparing the disposable image"

    with tempfile.TemporaryDirectory(prefix="nostalux-qemu-smoke-") as temporary:
        temp_dir = Path(temporary)
        disk_copy = temp_dir / "NostaluxOS-smoke.img"
        socket_path = temp_dir / "qmp.sock"
        screenshot = temp_dir / "boot.ppm"
        log_path = temp_dir / "qemu.log"
        digest = copy_image(
            image, disk_copy, lock_path, deadline, args.lock_timeout
        )

        command = [
            qemu,
            *shlex.split(args.qemu_args),
            "-name",
            "NostaluxOS smoke test",
            "-display",
            "none",
            "-monitor",
            "none",
            "-serial",
            "none",
            "-qmp",
            f"unix:{socket_path},server=on,wait=off",
            "-no-reboot",
            "-no-shutdown",
            "-nic",
            "none",
            "-m",
            str(args.memory_mib),
            "-machine",
            "pc,pcspk-audiodev=snd0",
            "-audiodev",
            "none,id=snd0",
            "-drive",
            f"if=ide,index=0,media=disk,format=raw,snapshot=on,file={disk_copy}",
        ]

        try:
            with log_path.open("wb") as qemu_log:
                process = subprocess.Popen(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=qemu_log,
                    stderr=subprocess.STDOUT,
                )
                qmp = connect_qmp(socket_path, process, deadline)

                # Reserve at least one third of the overall deadline for input
                # injection and observing ring 3.
                phase = "waiting for the boot shell"
                boot_deadline = min(deadline, started + args.timeout * 0.67)
                kernel_rip, _ = wait_for_kernel(qmp, boot_deadline)
                phase = "validating the boot framebuffer"
                qmp.hmp(f"screendump {screenshot}")
                wait_for_file(screenshot, deadline)
                width, height, colors = validate_framebuffer(screenshot)

                phase = "validating clean ring-3 entry state"
                type_command(qmp, "app hang-probe", deadline)
                app_rip, app_registers = wait_for_app(qmp, deadline)
                app_rsp = validate_initial_user_registers(app_registers)
                validate_initial_extended_state(app_registers)
                phase = "waiting for non-yielding app preemption"
                resumed_kernel_rip, _ = wait_for_kernel(qmp, deadline)

                phase = "launching the hostile RFLAGS probe"
                # The boot-time hello sample owns process ID 1.
                type_command(qmp, "appkill 2", deadline)
                wait_for_kernel(qmp, deadline)
                type_command(qmp, "app rflags-probe", deadline)
                rflags_rip, _ = wait_for_app(
                    qmp,
                    deadline,
                    required_register_values={
                        "R15": RFLAGS_PROBE_MARKER,
                    },
                    required_rflags_bits=RFLAGS_NT,
                )
                rflags_returns = 0
                for _ in range(2):
                    phase = "validating hostile RFLAGS interrupt return"
                    wait_for_kernel(qmp, deadline)
                    rflags_rip, _ = wait_for_app(
                        qmp,
                        deadline,
                        required_register_values={
                            "R15": RFLAGS_PROBE_MARKER,
                        },
                        required_rflags_bits=RFLAGS_NT,
                    )
                    rflags_returns += 1

                phase = "launching the invalid-stack probe"
                wait_for_kernel(qmp, deadline)
                type_command(qmp, "appkill 3", deadline)
                wait_for_kernel(qmp, deadline)
                stack_observation = None
                stack_attempts = 0
                for stack_attempts in range(1, 5):
                    type_command(qmp, "app stack-probe", deadline)
                    stack_observation = observe_invalid_stack_probe(
                        qmp,
                        min(deadline, time.monotonic() + 0.75),
                    )
                    if stack_observation is not None:
                        break
                    wait_for_kernel(qmp, deadline)
                if stack_observation is None:
                    raise SmokeFailure(
                        "could not observe the stack probe's invalid ring-3 RSP"
                    )
                stack_rip, invalid_rsp = stack_observation
                phase = "containing the invalid-stack return"
                recovered_kernel_rip, _ = wait_for_kernel(qmp, deadline)

                phase = "proving post-fault shell progress"
                type_command(qmp, "app hang-probe", deadline)
                recovered_app_rip, recovered_app_registers = wait_for_app(
                    qmp,
                    deadline,
                    required_register_values={"R15": 0},
                )
                recovered_app_rsp = validate_initial_user_registers(
                    recovered_app_registers
                )
                validate_initial_extended_state(recovered_app_registers)

                print(
                    f"PASS kernel: long mode, shell idle at RIP 0x{kernel_rip:x}"
                )
                print(
                    f"PASS framebuffer: {width}x{height}, {colors} rendered colors"
                )
                print(
                    "PASS app: hang-probe reached ring 3 with scrubbed GPR/FPU "
                    f"state and aligned RSP 0x{app_rsp:x} at RIP 0x{app_rip:x}"
                )
                print(
                    "PASS preemption: the non-yielding app returned control to "
                    f"the shell at RIP 0x{resumed_kernel_rip:x}"
                )
                print(
                    "PASS interrupt return: the marked NT-flag probe resumed "
                    f"{rflags_returns} times at RIP 0x{rflags_rip:x}"
                )
                print(
                    "PASS stack containment: invalid user RSP "
                    f"0x{invalid_rsp:x} at RIP 0x{stack_rip:x} was observed "
                    f"on attempt {stack_attempts} and returned control to "
                    f"the kernel at RIP 0x{recovered_kernel_rip:x}"
                )
                print(
                    "PASS recovery: the shell launched a fresh app with "
                    "scrubbed GPR/FPU state and aligned RSP "
                    f"0x{recovered_app_rsp:x} at RIP 0x{recovered_app_rip:x}"
                )
                print(
                    "PASS isolation: QEMU used a read-only disposable image "
                    f"copy (source SHA-256 {digest[:16]}...)"
                )
        except Exception as error:
            if process is not None:
                stop_qemu(process, qmp)
            log_tail = qemu_log_tail(log_path)
            if log_tail:
                print("QEMU log tail:", file=sys.stderr)
                print(log_tail, file=sys.stderr)
            if isinstance(error, SmokeFailure):
                raise SmokeFailure(f"{phase}: {error}") from error
            raise
        finally:
            if process is not None:
                stop_qemu(process, qmp)
            if qmp is not None:
                qmp.connection.close()

    print(f"QEMU smoke test passed in {time.monotonic() - started:.2f}s")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SmokeFailure, OSError, ValueError) as error:
        print(f"QEMU smoke test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
