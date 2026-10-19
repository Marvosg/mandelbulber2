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
    names = ("Mandelbulber2.app", "mandelbulber2.app")
    roots = (Path("/Applications"), Path.home() / "Applications")
    for root in roots:
        for name in names:
            candidate = root / name / "Contents" / "MacOS" / "mandelbulber2"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
    return "mandelbulber2"


# Qt QLocalServer::listen("name") on Windows is the pipe \\.\pipe\name.
# macOS sockaddr_un.sun_path is 104 bytes including the NUL. The app falls
# back when the preferred path is at least this many UTF-8 bytes. The session
# file then carries the path that was actually opened.
UNIX_SOCKET_PATH_LIMIT = 100


def windows_pipe_path(name: str) -> str:
    """Map a Qt local-server name to a Windows named-pipe path.

    ``mandelbulber2-user`` becomes ``\\\\.\\pipe\\mandelbulber2-user``. A value
    that is already a pipe path is returned in the backslash form CreateFileW
    expects.
    """

    text = name.strip()
    normalized = text.replace("/", "\\")
    prefix = "\\\\.\\pipe\\"
    if normalized.lower().startswith(prefix.lower()):
        return prefix + normalized[len(prefix) :]
    return prefix + text


def unix_socket_path_too_long(path: str, limit: int = UNIX_SOCKET_PATH_LIMIT) -> bool:
    """True when a Unix socket path may not fit in sockaddr_un on macOS."""

    return len(path.encode("utf-8")) >= limit


class Bridge:
    """One local socket to the app. Launched headless processes are stopped with us."""

    def __init__(self) -> None:
        self.process: subprocess.Popen | None = None
        self.socket: _UnixTransport | _WindowsPipe | None = None
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
            self.socket = _WindowsPipe(windows_pipe_path(name))
        else:
            self.socket = _UnixTransport(name)
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


class _UnixTransport:
    def __init__(self, path: str, timeout: float = 180) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(path)

    def settimeout(self, timeout: float) -> None:
        self._sock.settimeout(timeout)

    def sendall(self, data: bytes) -> None:
        self._sock.sendall(data)

    def recv(self, size: int) -> bytes:
        return self._sock.recv(size)

    def close(self) -> None:
        self._sock.close()


class _WindowsPipe:
    """Client for the named pipe Qt opens on Windows. Not used on Linux or macOS."""

    def __init__(self, path: str, timeout: float = 180) -> None:
        import ctypes
        from ctypes import wintypes

        self._ctypes = ctypes
        self._timeout = timeout
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.WriteFile.argtypes = [
            wintypes.HANDLE,
            wintypes.LPCVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            wintypes.LPVOID,
        ]
        kernel.WriteFile.restype = wintypes.BOOL
        kernel.ReadFile.argtypes = [
            wintypes.HANDLE,
            wintypes.LPVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            wintypes.LPVOID,
        ]
        kernel.ReadFile.restype = wintypes.BOOL
        kernel.PeekNamedPipe.argtypes = [
            wintypes.HANDLE,
            wintypes.LPVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
        ]
        kernel.PeekNamedPipe.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        self._kernel = kernel
        generic = 0x80000000 | 0x40000000  # GENERIC_READ | GENERIC_WRITE
        handle = kernel.CreateFileW(path, generic, 0, None, 3, 0, None)  # OPEN_EXISTING
        invalid = wintypes.HANDLE(-1).value
        if handle is None or int(handle) in (0, int(invalid)):
            err = ctypes.get_last_error()
            raise ConnectionError(
                f"Could not open the Mandelbulber pipe {path} (Windows error {err}). "
                "Start Mandelbulber, or set MANDELBULBER_BIN so this adapter can launch it."
            )
        self._handle = handle

    def settimeout(self, timeout: float) -> None:
        self._timeout = timeout

    def sendall(self, data: bytes) -> None:
        written = self._ctypes.c_ulong(0)
        buffer = self._ctypes.create_string_buffer(data)
        ok = self._kernel.WriteFile(self._handle, buffer, len(data), self._ctypes.byref(written), None)
        if not ok or written.value != len(data):
            err = self._ctypes.get_last_error()
            raise ConnectionError(f"Writing to the Mandelbulber pipe failed (Windows error {err}).")

    def recv(self, size: int) -> bytes:
        deadline = time.time() + self._timeout
        available = self._ctypes.c_ulong(0)
        while time.time() < deadline:
            ok = self._kernel.PeekNamedPipe(
                self._handle, None, 0, None, self._ctypes.byref(available), None
            )
            if not ok:
                err = self._ctypes.get_last_error()
                raise ConnectionError(f"The Mandelbulber pipe closed (Windows error {err}).")
            if available.value:
                to_read = min(size, int(available.value))
                buffer = self._ctypes.create_string_buffer(to_read)
                read = self._ctypes.c_ulong(0)
                ok = self._kernel.ReadFile(self._handle, buffer, to_read, self._ctypes.byref(read), None)
                if not ok:
                    err = self._ctypes.get_last_error()
                    raise ConnectionError(f"Reading the Mandelbulber pipe failed (Windows error {err}).")
                return buffer.raw[: read.value]
            time.sleep(0.02)
        raise TimeoutError("Timed out waiting for Mandelbulber on the named pipe.")

    def close(self) -> None:
        if self._handle is not None:
            self._kernel.CloseHandle(self._handle)
            self._handle = None


def call_detached(method: str, params: dict | None = None, timeout: float = 30) -> dict:
    """One command on a second connection. Used to poll status while a render holds the first."""

    session = read_session()
    if session is None or not session.get("socket"):
        raise RuntimeError("No Mandelbulber session.")
    client = Bridge()
    client._connect(str(session["socket"]))
    try:
        return client.call(method, params, timeout=timeout)
    finally:
        if client.socket is not None:
            try:
                client.socket.close()
            except OSError:
                pass
            client.socket = None
