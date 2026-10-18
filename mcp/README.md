# Mandelbulber MCP

This is the Python side of the Mandelbulber bridge. The application listens on a local socket. This package speaks MCP over stdio and forwards tool calls to that socket. Cursor and Claude Desktop start this process. They do not open a TCP port.

The socket is compiled into Mandelbulber. This package is installed separately and is not inside the macOS dmg.

## Install

```bash
uv tool install --from /path/to/mandelbulber2/mcp mandelbulber-mcp
```

or:

```bash
pip install /path/to/mandelbulber2/mcp
```

Requires Python 3.10+ and the official `mcp` package (2.x).

The adapter attaches to a Mandelbulber that is already open. If none is running, it starts `mandelbulber2 --mcp`, which is a headless session with the same socket. Set `MANDELBULBER_BIN` when the executable is not on `PATH`. On macOS the binary inside the app bundle is:

```text
/Applications/Mandelbulber2.app/Contents/MacOS/mandelbulber2
```

A process this adapter started is stopped when the MCP server exits. An already-open window is left running.

Session file:

- Linux: `~/.mandelbulber/mcp-session.json`
- macOS: `~/mandelbulber/mcp-session.json`

## Cursor

`.cursor/mcp.json` in the project, or the global MCP config:

```json
{
  "mcpServers": {
    "mandelbulber": {
      "command": "mandelbulber-mcp",
      "env": {
        "MANDELBULBER_BIN": "/Applications/Mandelbulber2.app/Contents/MacOS/mandelbulber2"
      }
    }
  }
}
```

Leave `MANDELBULBER_BIN` out when `mandelbulber2` is already on `PATH`. On Linux a typical value is `/usr/bin/mandelbulber2`.

## Claude Desktop

`~/.config/Claude/claude_desktop_config.json` on Linux, `~/Library/Application Support/Claude/claude_desktop_config.json` on macOS, or `%APPDATA%\Claude\claude_desktop_config.json` on Windows:

```json
{
  "mcpServers": {
    "mandelbulber": {
      "command": "mandelbulber-mcp",
      "env": {
        "MANDELBULBER_BIN": "/Applications/Mandelbulber2.app/Contents/MacOS/mandelbulber2"
      }
    }
  }
}
```

If the console script is not on the desktop app's `PATH`, use the full path from `which mandelbulber-mcp`, or:

```json
{
  "mcpServers": {
    "mandelbulber": {
      "command": "uvx",
      "args": ["--from", "/path/to/mandelbulber2/mcp", "mandelbulber-mcp"]
    }
  }
}
```

## macOS application build

Use the qmake projects CircleCI already builds. Homebrew Qt 6, not the Qt 5 steps in the old `deploy/README-osx.txt`.

```bash
brew install llvm qt gsl lzo jpeg libpng libtiff libsndfile
```

`qt` from Homebrew is Qt 6. Clone Khronos OpenCL-CLHPP and copy `include/CL` so `cl.hpp` exists. OpenCL.framework supplies the library.

From `mandelbulber2/qmake`:

- Apple Silicon: `qmake mandelbulber-opencl-m1.pro` (C++17, deployment target 12.0, `/opt/homebrew`, OpenMP off).
- Intel: `qmake mandelbulber-opencl-mac.pro` (C++14, clang from `/usr/local/opt/llvm`).

Then `make` and `macdeployqt mandelbulber2.app -dmg`.

`macos.pri` copies formulas, examples, materials, and the other share files into `Contents/Resources`, including `data/mcp-dock-tooltips.json`. That file is the dock-tooltip text for an installed app. A developer tree with `qt/` next to the formulas still prefers the live `.ui` files. The socket is already in the binary. Do not bundle this Python package into the dmg. `macdeployqt` will not produce a working embedded interpreter.

New `mandelbulber2/src/*.cpp` files are picked up by `SOURCES += $$ROOT/src/*.cpp` only when qmake is run again.

## What the tools cover

Formulas and their parameters, the objects tree (hybrids, booleans, primitives), materials, lights, camera, keyframes, orbit and fly-through paths, a draft PNG preview, still and animation jobs with status, stop, and a downscaled image, plus load, save, and undo. `get_guidance` has the starter recipes. See `docs/mcp-design.md`.

On Windows the application listens on a named pipe. This adapter's pipe client is not written yet, so Windows still needs that piece before the tools can connect.
