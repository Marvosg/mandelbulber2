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

The adapter attaches to a Mandelbulber that is already open. If none is running, it starts `mandelbulber2 --mcp`, which is a headless session with the same socket. Set `MANDELBULBER_BIN` when the executable is not on `PATH`. On macOS, qmake names the bundle `mandelbulber2.app`. A renamed copy may be `Mandelbulber2.app`. The adapter checks both, in `/Applications` and in `~/Applications`:

```text
/Applications/mandelbulber2.app/Contents/MacOS/mandelbulber2
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

The step-by-step build, plus Cursor and Claude Desktop, is below under "macOS: build the app, then connect Cursor or Claude". Homebrew Qt 6. Do not follow the Qt 5 steps in `deploy/README-osx.txt`. Do not bundle this Python package into the dmg.

## What the tools cover

Formulas and their parameters, the objects tree (hybrids, booleans, primitives), materials, lights, camera, `probe_view` (where a spot in the picture is, and the ranked square-cell grid), `look_around` (interesting directions, including just off frame), `navigate_camera` (the navigation buttons, plus a cell range, orbit around a spot, zoom, thirds framing, and a move list), `place_in_view` (a light, object, focus, or fog on that spot), `measure_view` and `fit_to_view` (size a primitive or wrap limits from two spots), `frame_scene` (pull the camera back when a preview is empty), keyframes, orbit and fly-through paths, a draft PNG preview with an annotated grid, still and animation jobs, resume of an interrupted render, a contact sheet of animation frames, load, save, undo, and the toolbar presets (`list_presets`, `inspect_preset`, `save_preset`, `load_preset`, `delete_preset`). Agent presets sit after a separator and are the only cards the agent can delete. `get_guidance` has the starter recipes, including the Mandelbox scale trap. See `docs/mcp-design.md`.

When the MCP client sends a progress token, preview, still, and animation report progress. Without a token, still and animation return a job id and you poll `render_status`. Preview always returns the PNG.

`undo` stops at the scene from when this Mandelbulber process finished starting. It does not walk undo files from an older session. The window's Undo menu still can.

On Windows the application listens on a named pipe (`\\.\pipe\mandelbulber2-<user>`). This adapter opens that pipe. The path mapping is unit-tested. A connection to a real Windows build was not run here.

## macOS: build the app, then connect Cursor or Claude

Do this on the Mac. This repository's Linux checks did not compile or launch the Mac app.

1. Install tools. Homebrew Qt 6, not the Qt 5 steps later in `deploy/README-osx.txt`.

   ```bash
   brew install llvm qt gsl lzo jpeg libpng libtiff libsndfile
   ```

2. OpenCL headers. The Mac `.pro` files include `cl.hpp`.

   ```bash
   git clone https://github.com/KhronosGroup/OpenCL-CLHPP.git /tmp/OpenCL-CLHPP
   mkdir -p "$HOME/mandelbulber2/build/OpenCL"
   cp -R /tmp/OpenCL-CLHPP/include/CL/. "$HOME/mandelbulber2/build/OpenCL/"
   cp "$HOME/mandelbulber2/build/OpenCL/opencl.hpp" "$HOME/mandelbulber2/build/OpenCL/cl.hpp"
   ```

   OpenCL.framework supplies the library. Do not link `-lOpenCL`.

3. Generate the Makefile from `mandelbulber2/qmake`. `$(brew --prefix qt)/bin/qmake` is the binary if `qmake` is not on `PATH`. Pass the OpenCL include directory.

   Apple Silicon (`uname -m` is `arm64`):

   ```bash
   cd "$HOME/mandelbulber2/build"
   "$(brew --prefix qt)/bin/qmake" "$HOME/mandelbulber2/mandelbulber2/qmake/mandelbulber-opencl-m1.pro" \
     QMAKE_CXXFLAGS+="-I$HOME/mandelbulber2/build -DCL_SILENCE_DEPRECATION -DCL_TARGET_OPENCL_VERSION=120" \
     QMAKE_CFLAGS+="-I$HOME/mandelbulber2/build -DCL_SILENCE_DEPRECATION -DCL_TARGET_OPENCL_VERSION=120"
   ```

   That project is C++17, deployment target 12.0, headers under `/opt/homebrew`, OpenMP off.

   Intel:

   ```bash
   cd "$HOME/mandelbulber2/build"
   "$(brew --prefix qt)/bin/qmake" "$HOME/mandelbulber2/mandelbulber2/qmake/mandelbulber-opencl-mac.pro" \
     QMAKE_CXXFLAGS+="-I$HOME/mandelbulber2/build -DCL_SILENCE_DEPRECATION -DCL_TARGET_OPENCL_VERSION=120" \
     QMAKE_CFLAGS+="-I$HOME/mandelbulber2/build -DCL_SILENCE_DEPRECATION -DCL_TARGET_OPENCL_VERSION=120"
   ```

   C++14. CircleCI symlinks Homebrew LLVM to `/usr/local/opt/llvm` (`sudo mkdir -p /usr/local/opt && sudo ln -s "$(brew --prefix llvm)" /usr/local/opt/llvm`).

   `SOURCES += $$ROOT/src/*.cpp` is expanded when qmake runs. Run qmake again after new source files appear. `mcp_scene.cpp` and `mcp_server.cpp` are picked up by that wildcard.

4. Build and package.

   ```bash
   make -j"$(sysctl -n hw.ncpu)"
   "$(brew --prefix qt)/bin/macdeployqt" mandelbulber2.app -dmg
   ```

   The binary is `mandelbulber2.app/Contents/MacOS/mandelbulber2`. `macos.pri` copies formulas, examples, and `data/` (including `mcp-dock-tooltips.json`) into `Contents/Resources`. Do not put this Python package in the dmg.

5. Optional: copy the app to `/Applications`. The data directory the app uses is `~/mandelbulber` (not `~/.mandelbulber`). The session file is `~/mandelbulber/mcp-session.json`. If that home path is very long, the socket moves to a shorter path and the session file records it.

6. Install the adapter on the Mac (Python 3.10+):

   ```bash
   pip install "$HOME/mandelbulber2/mcp"
   ```

   or `uv tool install --from "$HOME/mandelbulber2/mcp" mandelbulber-mcp`.

7. Cursor. Project `.cursor/mcp.json`, or Cursor Settings → MCP. Use the path from step 4 if you did not copy the app.

   ```json
   {
     "mcpServers": {
       "mandelbulber": {
         "command": "mandelbulber-mcp",
         "env": {
           "MANDELBULBER_BIN": "/Applications/mandelbulber2.app/Contents/MacOS/mandelbulber2"
         }
       }
     }
   }
   ```

   Restart Cursor, or reload MCP servers. Open Mandelbulber yourself if you want the agent to drive the window. If it is not running, the adapter starts `mandelbulber2 --mcp`.

8. Claude Desktop. Edit `~/Library/Application Support/Claude/claude_desktop_config.json` (create it if needed) with the same `mcpServers` block. If `mandelbulber-mcp` is not on the desktop app's `PATH`, set `command` to the output of `which mandelbulber-mcp`. Quit and reopen Claude Desktop.
