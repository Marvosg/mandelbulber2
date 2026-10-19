#!/usr/bin/env python3
# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Path rules that do not need a Mandelbulber process.

The Windows pipe client is exercised here only as a name. A real pipe was not
opened on this machine. The 100-byte socket limit is the same threshold the
C++ server uses before it falls back to a shorter path.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mandelbulber_mcp.bridge import (  # noqa: E402
    UNIX_SOCKET_PATH_LIMIT,
    unix_socket_path_too_long,
    windows_pipe_path,
)


class WindowsPipePath(unittest.TestCase):
    def test_qt_listen_name(self) -> None:
        self.assertEqual(
            windows_pipe_path("mandelbulber2-mike"),
            "\\\\.\\pipe\\mandelbulber2-mike",
        )

    def test_already_a_pipe(self) -> None:
        self.assertEqual(
            windows_pipe_path("\\\\.\\pipe\\mandelbulber2-mike"),
            "\\\\.\\pipe\\mandelbulber2-mike",
        )

    def test_forward_slashes(self) -> None:
        self.assertEqual(
            windows_pipe_path("//./pipe/mandelbulber2-mike"),
            "\\\\.\\pipe\\mandelbulber2-mike",
        )

    def test_strips_whitespace(self) -> None:
        self.assertEqual(windows_pipe_path("  mandelbulber2-user  "), "\\\\.\\pipe\\mandelbulber2-user")


class SocketLength(unittest.TestCase):
    def test_limit_matches_the_server(self) -> None:
        self.assertEqual(UNIX_SOCKET_PATH_LIMIT, 100)

    def test_short_linux_path_fits(self) -> None:
        path = "/home/ubuntu/.mandelbulber/mandelbulber2-1000.sock"
        self.assertFalse(unix_socket_path_too_long(path))

    def test_long_macos_home_needs_a_fallback(self) -> None:
        home = "/Users/" + ("n" * 70)
        path = f"{home}/mandelbulber/mandelbulber2-501.sock"
        self.assertGreaterEqual(len(path.encode("utf-8")), 100)
        self.assertTrue(unix_socket_path_too_long(path))

    def test_tmp_fallback_fits(self) -> None:
        self.assertFalse(unix_socket_path_too_long("/tmp/mandelbulber2-501.sock"))


if __name__ == "__main__":
    unittest.main()
