# Mandelbulber2 MCP

The app listens on a local socket (`src/mcp_server.cpp`, `src/mcp_catalog.cpp`, `src/mcp_scene.cpp`, `mandelbulber2 --mcp`). The official Python MCP SDK adapter is `mcp/` and speaks stdio. Install and client config are in `mcp/README.md`.

The eight decisions in the last section are accepted as written: Python bridge, official MCP SDK, stdio plus `QLocalServer`, Cursor and Claude Desktop on the same machine, attach or launch `--mcp`, live-session edits with one undo step and widget sync, the dmg contains only the app, and the first coding change was the core slice. The scene slice is now in the same tree: objects, materials and lights, keyframes and flight, and render jobs.

The goal is the same shape as Blender MCP: an agent connected to the Mandelbulber the user already has open, able to set up formulas, camera, lighting, materials, effects, animation, and renders, and able to look at a cheap preview before committing to a long render. The user will not know parameter names, so the tools have to teach the agent.

## How the program is put together

Mandelbulber 2.35 is a Qt widgets application. The current build is **qmake + Qt 6**. `mandelbulber2/qmake/mandelbulber.pro` pulls in `common.pri`. C++ is 14 on Linux and Intel Macs, 17 on Apple Silicon (`CONFIG+=m1`).

`mandelbulber2/cmake/CMakeLists.txt` is behind the qmake build. It still asks for Qt 5, and its install section says macOS is unfinished. Do not use it for this work. The root README and `deploy/README-osx.txt` still describe Qt 5 and a hand-built libpng/gsl/tiff stack. Those instructions are stale. The Ubuntu helper script and CircleCI already use Qt 6.

Rough layout:

| Area | Where |
| --- | --- |
| Render, parameters, settings, CLI, netrender, animation | `mandelbulber2/src/` |
| Main window, docks, custom widgets | `mandelbulber2/qt/` (`*.ui` plus matching `.cpp`) |
| One C++ class per formula, plus the registry | `mandelbulber2/formula/definition/` |
| Per-formula widgets, icons, OpenCL kernels | `mandelbulber2/formula/ui`, `img`, `opencl` |
| Examples, materials, textures, icons | `mandelbulber2/deploy/share/mandelbulber2/` |

Startup (`src/main.cpp`) builds one main parameter container (`gPar`) and nine fractal containers (`gParFractal`, slots 0–8, shown to the user as fractal 1–9). It fills them from `InitParams` / `InitFractalParams`, builds the formula list, then either shows the GUI or runs a CLI action and exits. `--mcp` forces headless mode, loads an optional settings file, and stays in the Qt event loop instead of rendering a still and exiting. A settings file without `--mcp` or `--nogui` still opens the GUI.

### Parameters

`cParameterContainer` is a name-to-value map. Each value (`cOneParameter`) stores the current value, default, optional min/max, morph type (how keyframes interpolate it), an enum list, and an optional JavaScript expression. Types are bool, int, double, 3- and 4-vectors, RGB, and string. Gets and sets take a mutex.

`mandelbulber2 --list` prints the main container only: about 1,100 `key=default` lines, with no descriptions, ranges, or fractal-slot parameters.

Fractal parameters are not split per formula. `InitFractalParams` registers every formula's parameters on every slot. The formula's `.ui` file is what decides which of those controls are shown.

### Formulas

`DefineFractalList` registers on the order of 460 formulas (plus `none` and custom). Each entry has a display name (`Mandelbulb`), an internal name (`mandelbulb`), a distance-estimator type, a default bailout, and pointers to `formula/ui/<name>.ui` and an icon. The information panel in that `.ui` file is the best human description that exists: a short paragraph, a reference link, and example filenames. The same text is usually repeated as a comment at the top of the formula's `.cpp`.

### Scene graph

The live scene is an objects tree, not only the nine formula slots. Nodes live in the main container as `node_0001_definition`, `node_0001_position`, and so on. A definition string looks like `mandelbulb 1,1,1,0,1` (name, id, type, parent, object id, order). Node types include fractal, hybrid, and boolean add / multiply / subtract, plus primitives. Loading an older `.fract` migrates `fractal_N` sections into this tree.

### What the GUI actually syncs

Widgets are not bound one-by-one in C++. `SynchronizeInterface` walks every dock and matches widget object names to parameter names. The name before the first `_` is the widget kind; the rest is the parameter. `spinbox_power` is `power`. `spinboxd_alpha_angle_offset` is `alpha_angle_offset`. Vector fields end in `_x`, `_y`, `_z`, `_w`.

`StartRender` **reads the widgets back into `gPar` before it renders**. Anything an agent writes into `gPar` without pushing it out to the widgets will be overwritten the next time the user (or the agent) hits Render. After a write, the MCP hook has to call `SynchronizeInterface(..., write)`.

Rendering itself copies the containers into `cRenderJob` and runs that job on a `QThread`. Changing parameters during a render does not affect the in-flight image. Stopping goes through `stopRequest`. `cInterface::StopRender()` pops a confirmation dialog once a render has been going for ten minutes, so the agent must set the flag directly and not call that slot.

There is an undo stack (`gUndo->Store`) and an autosave history under the data directory. Both should be used.

### Render paths that already exist

- GUI stills: `cInterface::StartRender` → `cRenderJob` on a worker thread, image in `mainImage`.
- Cheap GUI previews: `temporary_scale` and `image_preview_scale` shrink the resolution. `cRenderJob` also accepts a resolution divisor. `cRenderingConfiguration::ForceFastPreview` exists.
- CLI stills, flight (`-F` / `--flight`), keyframes (`-K` / `--keyframe`), queue, and voxel export: `cHeadless` calls the same `cRenderJob`. A still render is synchronous and the process then exits. There is no long-lived headless session today.
- OpenCL is optional (`mandelbulber-opencl.pro`, `-g` / `-G`). The CPU path is the one to test first.

`detail_level` is the main quality knob (1.0 means the smallest detail is about two pixels; higher is finer and slower). Ambient occlusion, depth of field, fog, reflections, and global illumination are ordinary parameters and can be turned down for a preview without saving that choice back to the project.

### Animation

- **Flight** (`cFlightAnimation`, `gAnimFrames`): a dense recorded path. Rendered with `--flight`.
- **Keyframes** (`cKeyframeAnimation`, `gKeyframes`): sparse keys. Subframes are interpolated with the morph type stored on each parameter (linear, Catmull-Rom, Akima, cubic, Steffen, plus angle variants). `framesPerKeyframe` is per key. Rendered with `--keyframe`.

Both serialize into the settings file as CSV-like `[frames]` and `[keyframes]` sections.

### Settings files

`.fract` files are INI-like text: `[main_parameters]`, `[fractal_N]`, `[frames]`, `[keyframes]`, and a `[description]`. Only non-default values are written. Values are locale-formatted. A file saved on a Polish locale contains `camera -0,15 -1,18 0,16`. Under `en_US` those commas do not parse as decimals, the camera collapses, and the render is empty. This was confirmed in this VM (see build status). The agent must go through the typed parameter API, not by editing `.fract` text.

## What is already there, and what is not

Nothing speaks MCP, JSON-RPC, or a local control socket.

| Piece | Useful for MCP? |
| --- | --- |
| CLI (`-n`, `-r`, `-o`, `-O key=value`, `--flight`, `--keyframe`, `--list`) | Good for batch renders and as a reference for the operations. It exits when the image is done, so it cannot drive a live GUI session. |
| Netrender (`src/netrender*.cpp`, TCP port 5555) | No. This is a binary protocol that hands scanlines to render slaves. It is not a scene API. |
| `deploy/share/mandelbulber2/scripts/mbSettings.php` | Offline editor for `.fract` text. Same locale trap as above. Not a live bridge. |
| `cScripts` (`QJSEngine` expressions stored on a parameter) | A feature to expose later ("set this parameter from an expression"). Not an automation API. |
| `cResourceHttpProvider` | Downloads textures. Unrelated. |

The right base is `gPar` / `gParFractal` / `gKeyframes` / `gAnimFrames`, `cSettings` for load/save, `cRenderJob` for pictures, and `SynchronizeInterface` plus `gUndo` for the open window.

## Linux build (this VM)

Host: Ubuntu 24.04, 4 cores. Packages: `qt6-base-dev`, `qt6-tools-dev`, `qt6-multimedia-dev`, `qt6-declarative-dev`, `libpng-dev`, `libgsl-dev`, `liblzo2-dev`, `libtiff-dev`, `libjpeg-dev`, `libopenexr-dev`, `libsndfile1-dev`.

```
qmake6 mandelbulber2/qmake/mandelbulber.pro CONFIG+=release
make -j2
```

Built out of tree in `/tmp/mb-build` with Qt 6.4.2. No source changes. OpenCL was left off (`mandelbulber.pro`, not `mandelbulber-opencl.pro`). The link succeeded; the binary is `mandelbulber2` 2.35-dev, about 13 MB.

qmake writes generated `ui_*.h` into `mandelbulber2/qt/`. Those are gitignored.

The binary finds data in `../share/mandelbulber2` relative to itself, then falls back to `/usr/share/mandelbulber2`. A symlink farm under `/tmp/share/mandelbulber2` (formula, language, examples, textures, and the rest) was enough. First launch prints "cannot move folder" for `~/.mandelbulber/*` because the one-time upgrade looks for directories that do not exist yet. Harmless.

Headless check, `QT_QPA_PLATFORM=offscreen`, no X server:

- `mandelbulber2 --version` → `Mandelbulber 2.35-dev`.
- `mandelbulber001.fract` (comma decimals) at 200×112 produced a near-empty PNG. Cause: locale `en_US` versus a file written with decimal commas. Not a build failure.
- `examples/Graeme McLaren collection .../mbulbPow2_v2.fract` (period decimals) at 240×240, with `detail_level`, ambient occlusion, and reflections turned down, finished in about 0.3 s and wrote a real fractal PNG to `/tmp/mb-pow2.png`.

CLI statistics used to print `Total number of iters: 0` because the objects-tree distance function never wrote `totalIters`. That assignment is in place. A 48×36 headless render of `mandelbulb001.fract` with `--stats` reported about 1.5e6 iterations.

## Recommended shape

Two processes, same idea as Blender MCP.

```
Cursor or Claude Desktop
        │  MCP over stdio (the client launches this process)
        ▼
Python MCP server  (small, in this repo)
        │  newline-delimited JSON over a local socket
        ▼
Mandelbulber already running
   QLocalServer on the Qt main thread
   reads/writes gPar, gParFractal, keyframes, flight
   pushes changes into the widgets
   runs cRenderJob for previews and finals
```

Why this split, in one paragraph: Cursor and Claude Desktop start the MCP server themselves and talk to it on stdin/stdout. They cannot attach that pipe to a Mandelbulber the user launched from a menu or a dock. So the process they start has to be a small adapter, and the app has to be listening on a local socket. Putting a full MCP implementation inside the C++ app would also mean tracking a protocol that moves (tool results, image content, streamable HTTP) in a codebase that has no JSON library for this today.

**In the app:** a `QLocalServer`, started from `main` after the parameters exist, in both GUI and a new headless mode. Socket name `mandelbulber2-<user id>`. On startup, write `mcp-session.json` into the existing data directory (`~/.mandelbulber` on Linux, `~/mandelbulber` on macOS) with the socket name, pid, version, and whether a GUI is up. Replace the file on each launch so the adapter finds the newest instance. Do not listen on TCP. A local socket is not reachable from the network, which matters because the socket can load and overwrite the open scene.

Commands are one JSON object per line, with an id, a method, and args. Replies are one JSON object, or a short error. Big images are not stuffed into that line: the app writes a PNG in a temp directory and returns the path; the Python side reads it and turns it into an MCP image. All handlers run on the Qt main thread (`BlockingQueuedConnection` from the socket's thread, or a queued call if the socket already lives on the main thread).

**Python side:** the official MCP Python SDK, stdio transport. It is the configuration both Cursor (`mcp.json`) and Claude Desktop already know how to launch. The server is GPL-3, same as the app; the SDK itself is MIT. Ship it as a directory in the repo (`mcp/` or similar) and document `uv run` / `pip install`. Do not vendor a Python runtime.

If `mcp-session.json` is missing or the pid is dead, the Python server starts `mandelbulber2 --mcp` (headless, event loop stays up, same socket, no widgets) and connects to that. If a GUI is already up, it attaches and does not start a second copy.

### Thread and UI rules

- Mutations and render start/stop happen on the GUI thread.
- A write updates the container, stores one undo snapshot, then `SynchronizeInterface(..., write)` so the open window matches.
- A render copies parameters the way `cRenderJob` already does. Stop by setting `stopRequest`, never via the dialog in `StopRender`.
- A preview uses a throwaway copy of the parameters (smaller resolution, coarser `detail_level`, heavy effects off) so the user's quality settings stay put.
- Headless `--mcp` skips the widget sync and just keeps the containers.
- Loading a file goes through `cSettings::LoadFromFile` + `Decode`, then a widget sync, same as the File menu.

### Tool list

Keep the list short. Discovery tools carry the knowledge; one write tool covers most knobs, because almost everything is already a parameter.

**Finding things**

- `list_formulas(query, limit)` — display name, internal name, one-line description. Search, do not dump all ~460.
- `describe_formula(name)` — description, reference, distance-estimator type, default bailout, the parameters that formula's `.ui` actually shows (label, tooltip, type, default, min/max, enum), and example files that use it.
- `list_parameters(area)` — areas such as camera, render quality, lights, materials, fog/DOF/AO, animation, objects tree. Same metadata.
- `describe_parameter(name)`
- `list_examples(query)` / `describe_example(path)` — bundled examples are the real recipes.
- `get_guidance()` — a short built-in note: how hybrids and booleans fit together, what `detail_level` means, how to preview cheaply, and three or four starter stacks (plain Mandelbulb, Mandelbox, a two-formula hybrid, a boolean). Written for the agent, maintained next to the Python server.

The catalog is built inside the app, where the containers, the formula list, and the `.ui` files already are. Widget tooltips and the formula information label are the human text. `parameterNames.txt` is an older alias list and is not the source of truth.

**Looking and editing**

- `get_session` — GUI or headless, version, whether a render is running, path of the loaded file if any.
- `get_state(detail)` — `summary` (formulas in use, camera, resolution, which effects are on) or `changed` (non-default parameters). Not the full 1,100 keys unless asked.
- `set_parameters(changes)` — list of `{name, value, fractal_slot?}`. Unknown names and out-of-range values return an error for that item and do not apply it. One undo step for the batch.
- `set_formula(slot, name)` — picks a formula the way the combo box does, including its default bailout.
- `load_example(name)` / `load_settings(path)` / `save_settings(path)` — load replaces the live session and is undoable. Save writes only the path the caller gives. It does not silently overwrite the file the user opened.
- `undo` / `redo`

**Scene structure** (thin helpers over `node_*` parameters, so the agent is not hand-building definition strings)

- `get_objects_tree`
- `add_object` / `update_object` / `delete_object` — fractal, hybrid, boolean, primitive
- `set_material` — materials are `matN_*` parameters; this groups the ones people actually mean (base color, gradient, specular, textures)

**Camera, render, animation**

- `set_camera` — position, target, rotation, field of view. Convenience on top of `set_parameters`.
- `render_preview(width, height, quality)` — blocking, returns a PNG as an MCP image. Default something like 320 px on the long side, coarse detail, AO/DOF/volumetrics off. Hard cap on pixels so a model cannot request an 8K preview by accident.
- `render_still(width, height, path)` — starts a real render, returns immediately with a job id.
- `render_status` / `stop_render`
- `get_image(max_edge)` — the current image, downscaled, as an MCP image. For looking, not for archival.
- `get_animation` / `add_keyframe` / `update_keyframe` / `delete_keyframe` / `set_flight_frames`
- `render_animation(mode, start_frame, end_frame, directory)` — `mode` is `keyframe` or `flight`. Returns a job id. The agent polls `render_status` and can pull a contact sheet of a few frames, not every frame at full size.

Long renders should also emit MCP progress notifications from the Python server, fed by the progress lines the app already computes for the CLI status bar.

### What the first implementation slice should contain

This slice is implemented.

1. Socket, session file, GUI-thread dispatch, undo (`cUndo::CommitNow`), widget sync.
2. `list_formulas`, `describe_formula`, `list_parameters`, `describe_parameter`, `get_guidance`, `get_state`, `set_parameters`, `set_formula`.
3. `render_preview` and `get_image` returning real PNGs. The preview is a copy: it caps `detail_level`, turns off ambient occlusion, depth of field, volumetric fog, ray-traced reflections, and Monte Carlo global illumination, and defines material 1 when the session has none so the PNG is not black. The session itself is not changed.
4. `load_example`, `load_settings`, `save_settings`, `undo`, `redo`.

`widget_roundtrip` is a socket method, not an MCP tool. It reads the widgets back into the session the way a GUI render does, so a test can prove an edit survived that read-back.

The stored default `mandelbox_scale` is 2. From the default camera that value draws an empty frame. `get_guidance` tells the agent to set `mandelbox_scale` to -1.5, which is the classic Amazing Box and does show up.

Numbers from the socket are parsed in the C locale. A string with one decimal comma and no dot is accepted. A token that contains both separators is rejected. `cMultiVal::Store(QString)` still uses the system locale; the MCP path does not call it.

### Scene slice

Implemented in `src/mcp_scene.cpp` and the matching Python tools.

- Objects tree: `get_objects_tree`, `add_object`, `update_object`, `delete_object`. Types are fractal, hybrid, boolean_add, boolean_mul, boolean_sub, and primitive. A hybrid's fractal children are one stack; `formula_iterations` is the repeat count. Booleans are real renderer nodes. A primitive cannot be a child of a hybrid. Fractal nodes use the nine existing slots.
- Looks: `set_material`, `set_light`, and the existing effects parameters (`iteration_fog_*`, `glow_*`, brightness). `get_guidance` has recipes for a clay-and-fog look as well as the shapes.
- Animation: `get_animation`, `add_keyframe`, `update_keyframe`, `delete_keyframe`, `set_flight_path` (orbit, fly-through, or an explicit frame list). Keyframe tools default `frames_per_keyframe` to 4. The application's own default of 100 is left alone. Interpolation is the parameter morph (Akima for the camera). The last key contributes one frame.
- Jobs: `render_still` and `render_animation` return a job id. `render_status`, `stop_render`, and `get_image` (optional `max_edge`) follow them. Animation ranges default to a cap of 24 frames and refuse anything over `max_frames` (hard maximum 500). Output is `frame_NNNN.png`.
- `render_preview` still returns a PNG to the client. The render runs on a worker thread. The socket waits in a `QEventLoop`, so the window keeps handling events, including `stop_render` and `render_status`. Mutations are refused while a job is running. Stop sets the job's flag. It does not call `cInterface::StopRender` (that raises a dialog). A window render is stopped with `stopRequest` only when that window is actually rendering.
- The socket is created before the autosave-recovery dialog. The undo baseline is stored after the dialog, so the first undo is the scene the user kept.
- Keyframe and flight rendering does not call the GUI's render slots. Those pop confirmation dialogs and are null in `--mcp`. Each frame is a still of a copy.

### Polish before a macOS build

- A stop that is already true when `cRenderJob::Execute` starts is kept for MCP jobs (`PreserveStopRequest`). Window renders still clear the flag, so the next GUI render after a stop is not stuck. `stop_before_execute` is a socket parameter used by the test, not an MCP tool. A preview with that flag ends `stopped`.
- `render_preview`, `render_still`, and `render_animation` emit MCP progress notifications when the client supplies a progress token. Without a token, still and animation return a job id immediately and `render_status` is the fallback. Preview always waits and returns the PNG. Progress for a preview is polled on a second connection, because the first socket is inside the render call.
- `render_animation` accepts `contact_sheet`. It writes `contact_sheet.png` when the job ends. `get_contact_sheet` builds the same strip on demand from finished frames. Thumbnails are plain images, with a short white mark instead of text, so a headless process does not need a font.
- The Python adapter opens the Windows named pipe with `ctypes` (`CreateFileW`, `ReadFile`, `WriteFile`, `PeekNamedPipe`). `windows_pipe_path` maps Qt's listen name `mandelbulber2-<user>` to `\\.\pipe\mandelbulber2-<user>`. That mapping is unit-tested on Linux. A live Windows pipe was not connected from this VM.
- MCP `undo` stops at the scene from when this process finished starting (after autosave recovery). Older undo files stay on disk for the window's Undo menu. They are not deleted.
- `frame_scene` measures the fractal on the CPU (the same walk as Reset view, without a navigation widget or OpenCL) and pulls the camera back. `get_guidance` still says to set `mandelbox_scale` to -1.5. Scale 2 from the default camera is inside the solid; `frame_scene` then a draft preview produced a non-black box.
- On macOS `sockaddr_un.sun_path` is 104 bytes including the NUL. If `~/mandelbulber/mandelbulber2-<uid>.sock` is 100 UTF-8 bytes or longer, the server listens on the temp directory, or on `/tmp/mandelbulber2-<uid>.sock` if that is also long. The session file stores the path that was opened. The adapter does not guess. Data directory on macOS is `~/mandelbulber`, not `~/.mandelbulber`. The adapter looks for `Mandelbulber2.app` and `mandelbulber2.app` under `/Applications` and `~/Applications`. qmake's `TARGET` is `mandelbulber2`, so a local build is `mandelbulber2.app`.

Fractal nodes still use the nine existing slots. The container is not grown.

## macOS build plan

Target the qmake OpenCL projects that CircleCI already builds. Do not revive the Qt 5 README.

From `.circleci/config.yml` (macOS, Xcode 27):

1. `brew install llvm qt gsl lzo jpeg libpng libtiff libsndfile tree p7zip wget`
2. `qt` from Homebrew is Qt 6. Qt 6.4 is the version this Linux build used; stay on Homebrew Qt 6, not Qt 5.
3. Clone Khronos OpenCL-CLHPP and copy `include/CL` so `cl.hpp` exists (the Mac `.pro` files expect that name). OpenCL.framework supplies the library; do not link `-lOpenCL`.
4. Apple Silicon: `qmake mandelbulber-opencl-m1.pro` (`CONFIG+=m1`, C++17, deployment target 12.0, headers under `/opt/homebrew`, OpenMP left off in `common.pri`).
5. Intel: `qmake mandelbulber-opencl-mac.pro` with clang from `/usr/local/opt/llvm` (CircleCI symlinks Homebrew LLVM there). C++14.
6. `make`, then `macdeployqt mandelbulber2.app -dmg`.

`qmake/macos.pri` copies formula, examples, materials, icons, sounds, textures, toolbar, data, docs, and language into `Contents/Resources`. `SHARED_DIR_IS_APP_DIR` makes the app load them from the bundle. A socket server compiled into the binary needs no extra bundle entries. Dock-widget tooltips for an installed app live in `deploy/share/mandelbulber2/data/mcp-dock-tooltips.json`, which is copied because `data/` is already in that list. Formula `.ui` files are already in the bundle. A developer tree that still has `qt/` next to `formula/` prefers those live `.ui` tooltips; the JSON fills whatever is missing, including a macOS app that does not ship `qt/*.ui`. Run qmake again after adding `src/*.cpp` files. The Python adapter stays out of the dmg.

The Python MCP server stays **outside** the `.app`. `macdeployqt` will not produce a working embedded Python, and a relocatable interpreter is a separate packaging project. Document, for both Mac and Linux:

- install the MCP package with `uv` or `pip`
- a Cursor `mcp.json` / Claude Desktop config snippet that runs it
- the app must be running, or the server will launch `mandelbulber2 --mcp` from `PATH` (on macOS, `mandelbulber2.app` or `Mandelbulber2.app` under `Contents/MacOS/`). The exact build and client steps are in `mcp/README.md`.

Data directory on macOS is `~/mandelbulber`, not `~/.mandelbulber`. The session file goes there so the adapter and the bundle agree.

OpenCL on the Mac build is for rendering speed. The MCP does not need a different code path; `opencl_enabled` is just another parameter. First testing stays on the CPU build, which is what this VM ran.

## Decisions to make before coding

These are the choices that change the design. The recommendation is the one this note is written around.

1. **Bridge versus a server inside the app.** Recommendation: Python MCP process plus a local socket in Mandelbulber. Alternative: Mandelbulber itself speaks MCP over HTTP on localhost and the client is configured with a URL, so there is no Python. That saves a runtime and makes packaging simpler, at the cost of implementing and keeping up with MCP inside Qt. A pure stdio server inside the app cannot attach to a GUI the user already opened, because the client owns stdin.

2. **Language of the MCP process.** Recommendation: Python, official MCP SDK. Alternative: TypeScript SDK, same architecture. Only matters if the answer to 1 is the bridge.

3. **How the client connects.** Recommendation: stdio from Cursor/Claude Desktop to the Python process, and a `QLocalServer` (not a TCP port) from that process to the app. Alternative, if 1 is in-app: streamable HTTP on `127.0.0.1` with a random port written into the session file.

4. **Which clients to target first.** Recommendation: both Cursor and Claude Desktop, stdio only, no client-specific tools. Say if you also want a remote/HTTP client on another machine. That would reopen the "no TCP" choice.

5. **May the agent run with no window open?** Recommendation: yes. If a GUI session file is live, attach to it. Otherwise start `mandelbulber2 --mcp` headless, same tools, no widget sync. Alternative: refuse until the GUI is open.

6. **How hard to protect the open project.** Recommendation: edits change the live session and push one undo step; they do not write the user's file. `save_settings` requires a path and will not overwrite the loaded file unless that path is explicit. `load_example` / `load_settings` replace the session, record undo, and rely on the existing history folder. Alternative: a GUI switch that must be on before the socket accepts writes. Alternative: every agent change auto-saves a copy next to the original.

7. **Ship the MCP server inside the macOS app/dmg?** Recommendation: no. The dmg contains Mandelbulber with the socket compiled in. The Python adapter is installed separately and documented. Alternative: bundle a Python runtime in the app, which is a large packaging detour before any tool works.

8. **How much of the tool list the next run should implement.** Recommendation: the first slice in "What the first implementation slice should contain" (socket, catalog, set parameters, preview image, load/save, undo), proven against a real render. Objects tree, materials, keyframes, and flight are the following slice on the same socket. Alternative: implement the full tool list in one pass.
