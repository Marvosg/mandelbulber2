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

    Read this first when the user does not know fractal vocabulary. It has
    recipes for a Mandelbulb, a Mandelbox (scale -1.5, not the stored 2), a
    hybrid stack, a boolean, a keyframe sketch, and an orbit, plus how color,
    lights, and cheap effects fit together. Edits stay in the live session
    until save_settings is given an explicit path. This text does not need
    Mandelbulber to be running.
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
    Pass query to filter by name or description. Prefer set_material, set_light,
    set_camera, and the keyframe tools when those match what the user asked.
    This list is the raw names behind them. Light and material parameters appear
    after set_light or set_material has defined that id.
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
    effects are not modified. The render runs off the UI thread, so an open
    window stays responsive, and this call still waits and returns the PNG.
    draft caps detail at 0.5 and defaults the long side to 320. better caps
    detail at 1.0 and defaults the long side to 480. The long side of a draft
    or better preview is never above 640. Aspect ratio is kept. Iteration fog
    and glow stay as they are, because they are often the look. Refuses while
    another render is running. Call this after every meaningful change and tell
    the user what changed.
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
def get_image(max_edge: int = 0, path: str = "") -> list:
    """Return a PNG of the latest preview, still, or animation frame.

    max_edge above 0 downscales so the longer side is at most that many pixels.
    Use it to look at a frame, not as the archive file. path reads a specific
    PNG instead of the latest one. While a job is running this is the newest
    finished frame.
    """

    params: dict = {}
    if max_edge:
        params["max_edge"] = max_edge
    if path:
        params["path"] = path
    try:
        result = bridge.call("get_image", params or None)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return [f"Error: {exc}"]
    data = open(result["path"], "rb").read()
    return [_dump(result), Image(data=data, format="png")]


@server.tool()
def get_objects_tree() -> str:
    """The scene tree: which formulas, booleans, and primitives are actually drawn.

    Each node has id, type, parent_id, and object_id. A fractal node's object_id
    is its formula slot (1 to 9). A hybrid's fractal children are one stack, in
    sibling order. formula_iterations on that slot is how many times the formula
    repeats before the next child. boolean_add unions, boolean_mul keeps the
    overlap, boolean_sub carves later children out of the first. A primitive
    cannot sit inside a hybrid. parent_id 0 is the world.
    """

    return _call("get_objects_tree")


@server.tool()
def add_object(
    type: str,
    parent_id: int = 0,
    formula: str = "",
    formula_iterations: int = 0,
    primitive: str = "sphere",
    radius: float = 0,
    position: list | None = None,
    scale: float = 0,
    name: str = "",
) -> str:
    """Add a fractal, hybrid, boolean, or primitive node. One undo step.

    type is fractal, hybrid, boolean_add, boolean_mul, boolean_sub, or primitive.
    parent_id 0 is the world; otherwise it must be a hybrid or boolean.
    formula (internal or display name) is used when type is fractal, and that
    node consumes the next free slot of the nine. formula_iterations is how many
    times that formula repeats inside a hybrid stack. primitive is sphere, box,
    plane, torus, cylinder, cone, ellipsoid, prism, rectangle, circle, or water.
    radius applies when that primitive has one. position is [x, y, z].
    """

    params: dict = {"type": type, "parent_id": parent_id, "primitive": primitive}
    if formula:
        params["formula"] = formula
    if formula_iterations:
        params["formula_iterations"] = formula_iterations
    if radius:
        params["radius"] = radius
    if position is not None:
        params["position"] = position
    if scale:
        params["scale"] = scale
    if name:
        params["name"] = name
    return _call("add_object", params)


@server.tool()
def update_object(
    id: int,
    position: list | None = None,
    rotation: list | None = None,
    scale: float = 0,
    material: int = 0,
    enabled: bool | None = None,
    parent_id: int | None = None,
    name: str = "",
) -> str:
    """Move, rename, reparent, or reassign a node from get_objects_tree. One undo step.

    position and rotation are [x, y, z]. parent_id 0 is the world.
    """

    params: dict = {"id": id}
    if position is not None:
        params["position"] = position
    if rotation is not None:
        params["rotation"] = rotation
    if scale:
        params["scale"] = scale
    if material:
        params["material"] = material
    if enabled is not None:
        params["enabled"] = enabled
    if parent_id is not None:
        params["parent_id"] = parent_id
    if name:
        params["name"] = name
    return _call("update_object", params)


@server.tool()
def delete_object(id: int) -> str:
    """Remove a node. Its children are reattached to its parent. One undo step."""

    return _call("delete_object", {"id": id})


@server.tool()
def set_material(
    id: int = 1,
    surface_color: list | str | None = None,
    specular: float | None = None,
    specular_width: float | None = None,
    reflectance: float | None = None,
    metallic: bool | None = None,
    use_colors_from_palette: bool | None = None,
    surface_color_gradient: str = "",
    name: str = "",
) -> str:
    """Define a surface. Material 1 is the default. One undo step.

    surface_color is #rrggbb or [r, g, b]. The gradient string is the orbit
    palette, used when use_colors_from_palette is true. Iteration fog is a
    separate effect and is often the color you see; set iteration_fog_enable
    and iteration_fog_color_1 with set_parameters.
    """

    params: dict = {"id": id}
    if surface_color is not None:
        params["surface_color"] = surface_color
    if specular is not None:
        params["specular"] = specular
    if specular_width is not None:
        params["specular_width"] = specular_width
    if reflectance is not None:
        params["reflectance"] = reflectance
    if metallic is not None:
        params["metallic"] = metallic
    if use_colors_from_palette is not None:
        params["use_colors_from_palette"] = use_colors_from_palette
    if surface_color_gradient:
        params["surface_color_gradient"] = surface_color_gradient
    if name:
        params["name"] = name
    return _call("set_material", params)


@server.tool()
def set_light(
    id: int = 1,
    enabled: bool | None = None,
    intensity: float | None = None,
    position: list | None = None,
    color: list | str | None = None,
) -> str:
    """Place a light. Light 1 is the key light. One undo step.

    A fresh session has no lights until this is called. Passing position
    detaches the light from the camera. Raise intensity before brightness if
    the shape is only a silhouette. color is #rrggbb or [r, g, b].
    """

    params: dict = {"id": id}
    if enabled is not None:
        params["enabled"] = enabled
    if intensity is not None:
        params["intensity"] = intensity
    if position is not None:
        params["position"] = position
    if color is not None:
        params["color"] = color
    return _call("set_light", params)


@server.tool()
def set_camera(
    camera: list | None = None,
    target: list | None = None,
    rotation: list | None = None,
    fov: float | None = None,
    distance: float | None = None,
) -> str:
    """Move the eye. Vectors are [x, y, z]. fov is degrees. One undo step.

    The default eye is near [3, -6, 2], looking at [0, 0, 0]. Small moves,
    then a preview. If the frame goes empty, put the target back on the shape.
    """

    params: dict = {}
    if camera is not None:
        params["camera"] = camera
    if target is not None:
        params["target"] = target
    if rotation is not None:
        params["rotation"] = rotation
    if fov is not None:
        params["fov"] = fov
    if distance is not None:
        params["distance"] = distance
    return _call("set_camera", params)


@server.tool()
def render_still(
    path: str = "",
    quality: str = "full",
    width: int = 0,
    height: int = 0,
    long_side: int = 0,
) -> str:
    """Start a still render and return a job id immediately.

    quality full keeps the session's detail and effects, on a copy, so the open
    file's settings are not rewritten. quality draft caps detail at 0.5 and
    turns the expensive effects off. long_side above 0 rescales and is capped
    at 640 for a draft and 4096 for full. Poll render_status. get_image returns
    a PNG once the file exists. stop_render sets the stop flag with no dialog.
    """

    params: dict = {"quality": quality}
    if path:
        params["path"] = path
    if width:
        params["width"] = width
    if height:
        params["height"] = height
    if long_side:
        params["long_side"] = long_side
    return _call("render_still", params)


@server.tool()
def render_status(id: str = "") -> str:
    """Progress of a still or animation job: status, frame, progress, image path, error.

    Omit id to ask about the job that is running, or the last one started.
    done is true when status is finished, stopped, or failed.
    """

    return _call("render_status", {"id": id} if id else None)


@server.tool()
def stop_render() -> str:
    """Stop the running MCP render, or a render the open window started.

    Sets the stop flag. There is no confirmation dialog. render_status changes
    to stopped when the current frame notices.
    """

    return _call("stop_render")


@server.tool()
def get_animation() -> str:
    """Keyframe list, how many interpolated frames they produce, and flight-frame count.

    frames_per_keyframe is the pictures from that key toward the next. The last
    key contributes one frame. Flight frames are already one picture each.
    """

    return _call("get_animation")


@server.tool()
def add_keyframe(frames_per_keyframe: int = 4, index: int = -1) -> str:
    """Store the current camera, target, rotation, fov, and distance as a key. One undo step.

    frames_per_keyframe defaults to 4 so a sketch stays short. The application's
    own default of 100 is a long final. index -1 appends. Move the camera, then
    add another key. Two keys at 4 produce 5 interpolated frames.
    """

    params: dict = {"frames_per_keyframe": frames_per_keyframe}
    if index >= 0:
        params["index"] = index
    return _call("add_keyframe", params)


@server.tool()
def update_keyframe(index: int, frames_per_keyframe: int = 0) -> str:
    """Replace key `index` with the camera as it is right now. One undo step.

    Pass frames_per_keyframe to change how many pictures lead to the next key.
    """

    params: dict = {"index": index}
    if frames_per_keyframe:
        params["frames_per_keyframe"] = frames_per_keyframe
    return _call("update_keyframe", params)


@server.tool()
def delete_keyframe(index: int) -> str:
    """Remove one key. One undo step."""

    return _call("delete_keyframe", {"index": index})


@server.tool()
def set_flight_path(
    kind: str = "orbit",
    count: int = 8,
    radius: float | None = None,
    height: float | None = None,
    target: list | None = None,
    start: list | None = None,
    end: list | None = None,
    fov: float | None = None,
    frames: list | None = None,
) -> str:
    """Replace the flight path. Does not change keyframes. One undo step.

    kind orbit circles the target. radius defaults to the current camera
    distance, height defaults to the camera's z. kind flythrough moves the eye
    in a straight line from start to end; keep target on the shape. kind frames
    takes a list of {camera, target, fov} objects, at least two. count is the
    number of pictures for orbit and flythrough (2 to 360). Render with
    render_animation mode flight.
    """

    params: dict = {"kind": kind, "count": count}
    if radius is not None:
        params["radius"] = radius
    if height is not None:
        params["height"] = height
    if target is not None:
        params["target"] = target
    if start is not None:
        params["start"] = start
    if end is not None:
        params["end"] = end
    if fov is not None:
        params["fov"] = fov
    if frames is not None:
        params["frames"] = frames
    return _call("set_flight_path", params)


@server.tool()
def render_animation(
    mode: str = "keyframe",
    start: int = 0,
    end: int = 0,
    directory: str = "",
    quality: str = "draft",
    long_side: int = 160,
    max_frames: int = 24,
    width: int = 0,
    height: int = 0,
) -> str:
    """Start a keyframe or flight render. Returns a job id immediately.

    mode is keyframe or flight. end 0 means through the last frame. quality
    defaults to draft. long_side defaults to 160 so a sketch stays small; pass
    0 only when you also pass width and height. The range is refused when it is
    longer than max_frames (default 24, hard maximum 500). Frames are written
    as frame_NNNN.png. Poll render_status and pull frames with get_image.
    """

    params: dict = {
        "mode": mode,
        "start": start,
        "quality": quality,
        "max_frames": max_frames,
    }
    if end:
        params["end"] = end
    if directory:
        params["directory"] = directory
    if width and height:
        params["width"] = width
        params["height"] = height
    elif long_side:
        params["long_side"] = long_side
    return _call("render_animation", params, timeout=60)


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
    """Undo the last session edit (parameters, formula, tree, look, animation, or a load)."""

    return _call("undo")


@server.tool()
def redo() -> str:
    """Redo an edit that was just undone."""

    return _call("redo")


def main() -> None:
    server.run(transport="stdio")


if __name__ == "__main__":
    main()
