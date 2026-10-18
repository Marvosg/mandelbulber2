# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""stdio MCP server. Tools talk to Mandelbulber over the local session socket."""

from __future__ import annotations

import atexit

from mcp.server.mcpserver import Image, MCPServer

from .bridge import Bridge
from .guidance import GUIDANCE

server = MCPServer("mandelbulber")
bridge = Bridge()
atexit.register(bridge.close)


def _dump(value: dict) -> str:
    import json

    return json.dumps(value, indent=2)


def _call(method: str, params: dict | None = None, timeout: float = 180) -> str:
    try:
        return _dump(bridge.call(method, params, timeout=timeout))
    except (RuntimeError, ConnectionError, OSError) as exc:
        return f"Error: {exc}"


@server.tool()
def get_guidance() -> str:
    """Teach a beginner before touching a parameter.

    Read this first when the user does not know fractal vocabulary. It has three
    starter recipes (classic Mandelbulb power 8, a Mandelbox, and what a
    two-formula hybrid actually is), what detail_level and N mean, which
    expensive effects to leave off, and the rule that edits stay in the live
    session until save_settings is given an explicit path. This text does not
    need Mandelbulber to be running.
    """

    return GUIDANCE


@server.tool()
def get_session() -> str:
    """Who is listening: an open window or a headless --mcp process, version, and whether a render is running."""

    return _call("get_session")


@server.tool()
def get_state(mode: str = "summary") -> str:
    """Read the live session.

    mode summary: formulas in use, camera, field of view, resolution, which
    effects are on, and the loaded file. mode changed: parameters that differ
    from the defaults (capped). mode both: both of those. Call this before the
    first edit so you do not overwrite a scene the user already built.
    """

    return _call("get_state", {"mode": mode})


@server.tool()
def list_formulas(query: str = "", limit: int = 20) -> str:
    """Search the formula list. There are hundreds; pass a word such as mandelbulb or box.

    Returns internal_name (what set_formula wants), the display name, and a short
    summary when the name itself did not match. Follow with describe_formula
    before changing anything specific to that formula.
    """

    return _call("list_formulas", {"query": query, "limit": limit})


@server.tool()
def describe_formula(name: str) -> str:
    """Explain one formula in plain language.

    Includes the info-panel description, reference links, example filenames,
    how its distance estimation behaves, the controls that formula's window
    actually shows (label, tooltip, default, range), and the common slot
    controls such as power, formula_iterations, and julia_mode. name is the
    internal name or the display name.
    """

    return _call("describe_formula", {"name": name})


@server.tool()
def list_parameters(area: str = "fractal", fractal_slot: int = 1, query: str = "", limit: int = 30) -> str:
    """List parameters the user can change, with short descriptions.

    area is camera, quality, lights, materials, effects, animation, or fractal.
    fractal means the formula currently in fractal_slot (1 to 9) plus shared
    slot controls. quality includes detail_level, DE_factor, and resolution.
    Pass query to filter by name or description. Materials and animation here
    are the raw parameters; friendly editors for them are a later slice.
    """

    return _call(
        "list_parameters",
        {"area": area, "fractal_slot": fractal_slot, "query": query, "limit": limit},
    )


@server.tool()
def describe_parameter(name: str, fractal_slot: int = 1) -> str:
    """One parameter: what it does, its type, current value, default, and range.

    fractal_slot matters for formula controls such as power. Camera, lights,
    and effects live on the main parameters and ignore the slot. Read the
    number_note before sending a value. Prefer JSON numbers over strings.
    """

    return _call("describe_parameter", {"name": name, "fractal_slot": fractal_slot})


@server.tool()
def list_examples(query: str = "", limit: int = 20) -> str:
    """Find shipped example .fract files by a word in the filename.

    load_example copies one into the live session. It does not edit the example.
    """

    return _call("list_examples", {"query": query, "limit": limit})


@server.tool()
def set_parameters(parameters: dict, fractal_slot: int = 1) -> str:
    """Change live parameters. One undo step for the whole batch.

    parameters is a JSON object of name to value. Numbers must be JSON numbers.
    Vectors are arrays of numbers, colors are [r, g, b] or #rrggbb. fractal_slot
    1 to 9 receives formula controls such as power when the name exists on that
    slot; otherwise the main parameters are used. Names like fractal2_power
    address a slot directly. Nothing is written to the user's file.
    """

    return _call("set_parameters", {"parameters": parameters, "fractal_slot": fractal_slot})


@server.tool()
def set_formula(formula: str, slot: int = 1) -> str:
    """Put a formula in slot 1 to 9. One undo step.

    formula is the internal name (mandelbulb) or the display name (Mandelbulb).
    Slot 1 is the default object and is what a fresh scene renders. The slot's
    formula integer is what the ray marcher reads. The objects-tree label is
    updated to match. Other slots do not appear in the picture until the tree
    uses them. A Mandelbox kept at the stored scale of 2 is an empty frame from
    the default camera; the starter recipe sets mandelbox_scale to -1.5.
    """

    return _call("set_formula", {"formula": formula, "slot": slot})


@server.tool(structured_output=False)
def render_preview(quality: str = "draft", long_side: int = 320) -> list:
    """Render a small PNG and return the image.

    Uses a throwaway copy: the user's resolution, detail_level, and expensive
    effects are not modified. draft caps detail at 0.6 and defaults the long
    side to 320. better caps detail at 1.0. The long side is never above 640.
    Aspect ratio is kept. Iteration fog and glow stay as they are, because they
    are often the look. Refuses while the open window is already rendering.
    Call this after every meaningful change and tell the user what changed.
    """

    if quality == "draft" and long_side == 320:
        params: dict = {"quality": quality}
    else:
        params = {"quality": quality, "long_side": long_side}
    try:
        result = bridge.call("render_preview", params, timeout=300)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return [f"Error: {exc}"]
    path = result["path"]
    data = open(path, "rb").read()
    note = _dump({key: value for key, value in result.items() if key != "path"})
    return [note, Image(data=data, format="png")]


@server.tool(structured_output=False)
def get_image() -> list:
    """Return the last preview PNG again, without rendering."""

    try:
        result = bridge.call("get_image")
    except (RuntimeError, ConnectionError, OSError) as exc:
        return [f"Error: {exc}"]
    data = open(result["path"], "rb").read()
    return [_dump(result), Image(data=data, format="png")]


@server.tool()
def load_settings(path: str) -> str:
    """Load a .fract file into the live session. One undo step. Does not modify the file."""

    return _call("load_settings", {"path": path})


@server.tool()
def load_example(name: str) -> str:
    """Load one shipped example by filename or relative path. One undo step.

    If the name matches more than one file, the error lists them. Ask the user
    which one. Loading does not write the example.
    """

    return _call("load_example", {"name": name})


@server.tool()
def save_settings(path: str, overwrite: bool = False) -> str:
    """Write the live session to an explicit path.

    Refuses the file that is currently loaded unless overwrite is true, and
    refuses anything inside the shared examples directory. Say which path you
    are writing before you call this. This is the only way this bridge saves.
    """

    return _call("save_settings", {"path": path, "overwrite": overwrite})


@server.tool()
def undo() -> str:
    """Undo the last session edit (set_parameters, set_formula, or a load)."""

    return _call("undo")


@server.tool()
def redo() -> str:
    """Redo an edit that was just undone."""

    return _call("redo")


def main() -> None:
    server.run(transport="stdio")


if __name__ == "__main__":
    main()
