# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attach to a running Mandelbulber, or start ``mandelbulber2 --mcp``."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import time
from pathlib import Path


def session_candidates() -> list[Path]:
    override = os.environ.get("MANDELBULBER_SESSION")
    paths: list[Path] = []
    if override:
        paths.append(Path(override))
    home = Path.home()
    paths.append(home / ".mandelbulber" / "mcp-session.json")
    paths.append(home / "mandelbulber" / "mcp-session.json")
    return paths


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def read_session() -> dict | None:
    for path in session_candidates():
        if not path.is_file():
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        pid = int(data.get("pid") or 0)
        if pid_alive(pid) and data.get("socket"):
            return data
    return None


def mandelbulber_binary() -> str:
    override = os.environ.get("MANDELBULBER_BIN")
    if override:
        return override
    for candidate in (
        "/Applications/Mandelbulber2.app/Contents/MacOS/mandelbulber2",
        str(Path.home() / "Applications/Mandelbulber2.app/Contents/MacOS/mandelbulber2"),
    ):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return "mandelbulber2"


class Bridge:
    """One local socket to the app. Launched headless processes are stopped with us."""

    def __init__(self) -> None:
        self.process: subprocess.Popen | None = None
        self.socket: socket.socket | None = None
        self._buffer = b""
        self._next_id = 0

    def close(self) -> None:
        if self.socket is not None:
            try:
                self.socket.close()
            except OSError:
                pass
            self.socket = None
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None

    def ensure(self) -> None:
        if self.socket is not None:
            return
        session = read_session()
        launched = False
        if session is None:
            binary = mandelbulber_binary()
            try:
                self.process = subprocess.Popen(
                    [binary, "--mcp"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
            except FileNotFoundError as exc:
                raise RuntimeError(
                    "mandelbulber2 is not on PATH. Set MANDELBULBER_BIN to the executable. "
                    "On macOS that is Mandelbulber2.app/Contents/MacOS/mandelbulber2 inside the dmg."
                ) from exc
            launched = True
            deadline = time.time() + 20
            while time.time() < deadline:
                if self.process.poll() is not None:
                    output = b""
                    if self.process.stdout is not None:
                        output = self.process.stdout.read() or b""
                    raise RuntimeError(
                        "mandelbulber2 --mcp exited before opening a session.\n"
                        + output.decode("utf-8", "replace")[-2000:]
                    )
                session = read_session()
                if session is not None:
                    break
                time.sleep(0.1)
            if session is None:
                self.close()
                raise RuntimeError("Timed out waiting for the Mandelbulber session file.")
        if not launched:
            self.process = None
        self._connect(str(session["socket"]))

    def _connect(self, name: str) -> None:
        if os.name == "nt":
            raise RuntimeError(
                "The Windows named-pipe client is not implemented in this slice. "
                "The app still listens; use the Linux or macOS socket."
            )
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(180)
        sock.connect(name)
        self.socket = sock
        self._buffer = b""

    def call(self, method: str, params: dict | None = None, timeout: float = 180) -> dict:
        self.ensure()
        assert self.socket is not None
        self._next_id += 1
        payload = {"id": self._next_id, "method": method, "params": params or {}}
        self.socket.settimeout(timeout)
        self.socket.sendall(json.dumps(payload).encode("utf-8") + b"\n")
        while b"\n" not in self._buffer:
            chunk = self.socket.recv(1 << 20)
            if not chunk:
                self.socket = None
                raise ConnectionError("Mandelbulber closed the MCP socket.")
            self._buffer += chunk
        line, _, rest = self._buffer.partition(b"\n")
        self._buffer = rest
        message = json.loads(line.decode("utf-8"))
        if not message.get("ok", False):
            result = message.get("result") or {}
            if isinstance(result, dict) and result.get("applied"):
                result["error"] = message.get("error")
                return result
            raise RuntimeError(message.get("error") or "Mandelbulber rejected the command.")
        result = message.get("result")
        return result if isinstance(result, dict) else {"value": result}
