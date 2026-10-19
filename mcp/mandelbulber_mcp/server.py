# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""stdio MCP server. Tools talk to Mandelbulber over the local session socket."""

from __future__ import annotations

import asyncio
import atexit
import time
from string import Template

from mcp.server.mcpserver import Context, Image, MCPServer

from .bridge import Bridge, call_detached
from .guidance import GUIDANCE

server = MCPServer("mandelbulber")
bridge = Bridge()
atexit.register(bridge.close)


def _dump(value: dict) -> str:
    import json

    return json.dumps(value, indent=2)


def _color_error(value: object, label: str) -> str | None:
    message = (
        f"{label} must be #rrggbb or [r, g, b]. Components are 0 to 255, or 0 to 65535."
    )
    if isinstance(value, str):
        text = value.strip()
        hex_digits = "0123456789abcdefABCDEF"
        if len(text) == 7 and text.startswith("#") and all(character in hex_digits for character in text[1:]):
            return None
        return message
    if isinstance(value, (list, tuple)) and len(value) == 3:
        numbers: list[float] = []
        for component in value:
            if isinstance(component, bool) or not isinstance(component, (int, float)):
                return message
            numbers.append(float(component))
        if all(0 <= component <= 255 for component in numbers):
            return None
        if all(0 <= component <= 65535 for component in numbers):
            return None
    return message


def _is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _vec3_error(value: object, label: str) -> str | None:
    if isinstance(value, (list, tuple)) and len(value) == 3 and all(_is_number(item) for item in value):
        return None
    return f"{label} must be [x, y, z]."


def _int_error(value: object, label: str, low: int, high: int) -> str | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < low or value > high:
        return f"{label} must be an integer from {low} to {high}."
    return None


def _number_error(value: object, label: str, low: float | None = None, high: float | None = None) -> str | None:
    if not _is_number(value):
        return f"{label} must be a number."
    number = float(value)
    if low is not None and number < low:
        return f"{label} must be at least {low}."
    if high is not None and number > high:
        return f"{label} must be at most {high}."
    return None


def _choice_error(value: object, label: str, choices: tuple[str, ...]) -> str | None:
    if isinstance(value, str) and value in choices:
        return None
    return f"{label} must be {', '.join(choices)}."


def _gradient_error(value: object) -> str | None:
    message = 'A gradient is position and color pairs, like "0 fd6029 9000 fff59b".'
    if not isinstance(value, str):
        return message
    parts = value.split()
    if len(parts) < 2 or len(parts) % 2:
        return message
    hex_digits = "0123456789abcdefABCDEF"
    for index, part in enumerate(parts):
        if index % 2 == 0:
            try:
                float(part)
            except ValueError:
                return message
        elif len(part) != 6 or any(character not in hex_digits for character in part):
            return message
    return None


def _call(method: str, params: dict | None = None, timeout: float = 180) -> str:
    try:
        return _dump(bridge.call(method, params, timeout=timeout))
    except (RuntimeError, ConnectionError, OSError) as exc:
        return f"Error: {exc}"


def _has_progress_token(ctx: Context) -> bool:
    try:
        meta = ctx.request_context.meta
    except (AttributeError, ValueError, LookupError):
        return False
    if not meta:
        return False
    token = None
    if isinstance(meta, dict):
        token = meta.get("progress_token", meta.get("progressToken"))
    else:
        token = getattr(meta, "progress_token", None)
        if token is None:
            token = getattr(meta, "progressToken", None)
    return token is not None and not isinstance(token, bool)


async def _report(ctx: Context, progress: float, message: str) -> None:
    try:
        await ctx.report_progress(progress, 1.0, message or None)
    except (RuntimeError, ValueError, ConnectionError, OSError):
        return


async def _await_job(ctx: Context, started: dict, wait: bool = True) -> dict:
    """Poll until the job finishes, emitting MCP progress when a token was supplied.

    Without a token, or with wait false, the caller returns the job id immediately
    and the agent uses render_status. With a token and wait true, this waits and
    returns the final status. After 3 minutes it returns the current status and
    leaves the job running.
    """

    job_id = str(started.get("id") or "")
    if not wait or not job_id or not _has_progress_token(ctx):
        return started
    await _report(ctx, float(started.get("progress") or 0), str(started.get("message") or "started"))
    last = -1.0
    last_status = started
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        status = await asyncio.to_thread(bridge.call, "render_status", {"id": job_id}, 30)
        last_status = status
        progress = float(status.get("progress") or 0)
        message = str(status.get("message") or status.get("status") or "")
        if status.get("cancelled_by_user"):
            message = str(status.get("error") or "The user cancelled this render.")
        done = bool(status.get("done")) or status.get("status") in {
            "finished",
            "stopped",
            "failed",
            "cancelled",
        }
        if abs(progress - last) > 1e-6 or done:
            await _report(ctx, 1.0 if done else progress, message)
            last = progress
        if done:
            return status
        await asyncio.sleep(0.25)
    pending = dict(last_status)
    pending["note"] = (
        "Still running after 3 minutes. The job was not stopped. "
        "Call render_status with this id and wait true to hold until it finishes. "
        "That return is the completion notice. A tool call that has already returned cannot notify you later."
    )
    return pending


async def _wait_for_completion(ctx: Context | None, job_id: str) -> dict:
    """Hold the tool call open until the job finishes.

    The returned status is the completion notice. Progress notifications during
    the wait are only delivered while this call is still open.
    """

    last = -1.0
    while True:
        status = await asyncio.to_thread(
            bridge.call, "render_status", {"id": job_id} if job_id else None, 30
        )
        progress = float(status.get("progress") or 0)
        message = str(status.get("message") or status.get("status") or "")
        if status.get("cancelled_by_user"):
            message = str(status.get("error") or "The user cancelled this render.")
        done = bool(status.get("done")) or status.get("status") in {
            "finished",
            "stopped",
            "failed",
            "cancelled",
        }
        if ctx is not None and (abs(progress - last) > 1e-6 or done):
            await _report(ctx, 1.0 if done else progress, message)
            last = progress
        if done or not status.get("id"):
            return status
        await asyncio.sleep(0.5)


async def _preview_with_progress(ctx: Context, params: dict) -> dict:
    task = asyncio.create_task(asyncio.to_thread(bridge.call, "render_preview", params, 300))
    await _report(ctx, 0.0, "starting")
    last = -1.0
    while not task.done():
        status: dict | None = None
        try:
            status = await asyncio.to_thread(call_detached, "render_status", None, 15)
        except (RuntimeError, ConnectionError, OSError, TimeoutError):
            status = None
        if isinstance(status, dict):
            progress = float(status.get("progress") or 0)
            if progress != last:
                await _report(ctx, progress, str(status.get("message") or ""))
                last = progress
        try:
            await asyncio.wait_for(asyncio.shield(task), 0.3)
        except asyncio.TimeoutError:
            pass
    result = await task
    message = "finished"
    if isinstance(result, dict) and result.get("cancelled_by_user"):
        message = str(result.get("error") or "The user cancelled this render.")
    await _report(ctx, 1.0, message)
    return result


@server.tool()
def get_guidance() -> str:
    """Teach a beginner before touching a parameter.

    Read this first when the user does not know fractal vocabulary. It has
    recipes for a Mandelbulb, a Mandelbox (scale -1.5, or frame_scene if a
    preview is empty), a hybrid stack, a boolean, a keyframe sketch, and an
    orbit, plus how color, lights, and cheap effects fit together. Numbers
    such as the default camera, power, fog, and core count are read from the
    running program, not typed into this text. Edits stay in the live session
    until save_settings is given an explicit path.
    """

    import json

    raw = _call("program_facts")
    if raw.startswith("Error:"):
        return raw
    facts = json.loads(raw)
    facts.setdefault("gradient_default", "(available once material 1 exists)")
    return Template(GUIDANCE).safe_substitute({key: str(value) for key, value in facts.items()})


@server.tool()
def get_session() -> str:
    """Who is listening: an open window or a headless --mcp process, version, and whether a render is running.

    checkpoints lists an interrupted still or animation. resume_dialog is set
    while the startup question is on screen. resume_render answers it.
    """

    return _call("get_session")


@server.tool()
def get_state(mode: str = "summary") -> str:
    """Read the live session.

    mode summary: formulas in use, camera, field of view, resolution, which
    effects are on, and the loaded file. The effects object also includes
    depth-of-field focus and radius, basic-fog visibility, the antialiasing
    grid, the saved limit_CPU_cores and threads_priority, the core count and
    priority the next MCP render will use, and live_render_threads. mode changed: parameters
    that differ from the defaults (capped). mode both: both of those. Call this
    before the first edit so you do not overwrite a scene the user already built.
    """

    error = _choice_error(mode, "mode", ("summary", "changed", "both"))
    if error:
        return f"Error: {error}"
    return _call("get_state", {"mode": mode})


@server.tool()
def list_formulas(query: str = "", limit: int = 20) -> str:
    """Search the formula list. There are hundreds; pass a word such as mandelbulb or box.

    Returns internal_name (what set_formula wants), the display name, and a short
    summary when the name itself did not match. Follow with describe_formula
    before changing anything specific to that formula.
    """

    error = _int_error(limit, "limit", 1, 10000)
    if error:
        return f"Error: {error}"
    return _call("list_formulas", {"query": query, "limit": limit})


@server.tool()
def describe_formula(name: str) -> str:
    """Explain one formula in plain language.

    Includes the info-panel description, reference links, example filenames,
    how its distance estimation behaves, where the shape usually lives
    (where, and typical_radius when that radius is known), the controls that
    formula's window actually shows (label, tooltip, default, range), and the
    common slot controls such as power, formula_iterations, and julia_mode.
    name is the internal name or the display name.
    """

    return _call("describe_formula", {"name": name})


@server.tool()
def list_parameters(area: str = "fractal", fractal_slot: int = 1, query: str = "", limit: int = 30) -> str:
    """List parameters the user can change, with short descriptions.

    area is camera, quality, lights, materials, effects, animation, or fractal.
    fractal means the formula currently in fractal_slot (1 to 9) plus shared
    slot controls. quality includes detail_level, DE_factor, and resolution.
    Pass query to filter by name or description. Prefer probe_view,
    navigate_camera, place_in_view, set_material, set_light, set_camera, and
    the keyframe tools when those match what the user asked.
    This list is the raw names behind them. Light and material parameters appear
    after set_light or set_material has defined that id.
    """

    error = _choice_error(
        area,
        "area",
        ("camera", "quality", "lights", "materials", "effects", "animation", "fractal", "all"),
    ) or _int_error(fractal_slot, "fractal_slot", 1, 9) or _int_error(limit, "limit", 1, 10000)
    if error:
        return f"Error: {error}"
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

    error = _int_error(fractal_slot, "fractal_slot", 1, 9)
    if error:
        return f"Error: {error}"
    if not isinstance(name, str) or not name.strip():
        return "Error: name must be a parameter name."
    return _call("describe_parameter", {"name": name, "fractal_slot": fractal_slot})


@server.tool()
def list_examples(query: str = "", limit: int = 20) -> str:
    """Find shipped example .fract files by a word in the filename.

    load_example copies one into the live session. It does not edit the example.
    """

    error = _int_error(limit, "limit", 1, 10000)
    if error:
        return f"Error: {error}"
    return _call("list_examples", {"query": query, "limit": limit})


@server.tool()
def set_parameters(parameters: dict, fractal_slot: int = 1) -> str:
    """Change live parameters. One undo step for the whole batch.

    parameters is a JSON object of name to value. Numbers must be JSON numbers.
    Vectors are arrays of numbers. A parameter whose name contains "color" or
    "colour" must be #rrggbb or [r, g, b]; anything else is rejected before it
    is sent. fractal_slot
    1 to 9 receives formula controls such as power when the name exists on that
    slot; otherwise the main parameters are used. Names like fractal2_power
    address a slot directly. Nothing is written to the user's file.

    limit_CPU_cores, from 1 to the machine's logical core count, is how many
    cores the next render uses. Lower it to keep the machine cooler.
    threads_priority is 0 lowest, 1 low, 2 normal, 3 high. Low keeps the UI
    responsive and does not cut heat the way fewer cores do. Both are
    application settings, not part of a .fract preset. Until one of them is
    set here, an MCP render uses one core under the machine limit and the
    lowest priority, and that default is not written into the preference.
    An explicit change is saved with the application settings when the app quits.
    """

    error = _int_error(fractal_slot, "fractal_slot", 1, 9)
    if error:
        return f"Error: {error}"
    if not isinstance(parameters, dict) or not parameters:
        return "Error: parameters must be an object of name to value."
    for name, value in parameters.items():
        if not isinstance(name, str) or not name.strip():
            return "Error: each parameter name must be a non-empty string."
        lowered = name.lower()
        if "gradient" in lowered:
            gradient = _gradient_error(value)
            if gradient:
                return f"Error: {name}: {gradient}"
            continue
        if "color" in lowered or "colour" in lowered:
            color = _color_error(value, name)
            if color:
                return f"Error: {color}"
        if lowered == "threads_priority" or lowered.endswith("_threads_priority"):
            priority = _int_error(value, name, 0, 3)
            if priority:
                return f"Error: {priority}"
        if lowered == "limit_cpu_cores" or lowered.endswith("_limit_cpu_cores"):
            cores = _int_error(value, name, 1, 4096)
            if cores:
                return f"Error: {cores}"
    return _call("set_parameters", {"parameters": parameters, "fractal_slot": fractal_slot})


@server.tool()
def set_formula(formula: str, slot: int = 1) -> str:
    """Put a formula in slot 1 to 9. One undo step.

    formula is the internal name (mandelbulb) or the display name (Mandelbulb).
    Slot 1 is the shape a fresh scene draws. Other slots stay unused until the
    objects tree points at them. A Mandelbox kept at the stored scale of 2 is
    an empty frame from the default camera, because the eye is inside the
    solid. Set     mandelbox_scale to -1.5, or call frame_scene and preview again.
    """

    error = _int_error(slot, "slot", 1, 9)
    if error:
        return f"Error: {error}"
    if not isinstance(formula, str) or not formula.strip():
        return "Error: formula must be an internal name or a display name."
    return _call("set_formula", {"formula": formula, "slot": slot})


@server.tool(structured_output=False)
async def render_preview(quality: str = "draft", long_side: int = 320, ctx: Context = None) -> list:
    """Render a small PNG and return the image.

    The note includes a ranked grid of square cells. column and row are
    0-based. ranked is every cell, best first. label is sky, inside, edge,
    ridge, pocket, slope, or flat. Nothing is drawn on the image. probe_view
    with no arguments returns the same grid.

    Uses a throwaway copy: the user's resolution, detail_level, and expensive
    effects are not modified. The render runs off the UI thread, so an open
    window stays responsive, and this call still waits and returns the PNG.
    draft caps detail at 0.5 and defaults the long side to 320. better caps
    detail at 1.0 and defaults the long side to 480. The long side of a draft
    or better preview is never above 640. Aspect ratio is kept. Iteration fog,
    glow, and basic fog stay as they are. Draft and better turn off ambient
    occlusion, depth of field, volumetric fog, ray-traced reflections, and
    Monte Carlo global illumination, on the copy only. Refuses while another
    render is running. Call this after every meaningful change and tell the
    user what changed. When the client sends a progress token, progress
    notifications are reported while this waits. render_status remains the
    fallback on a second look.
    """

    error = _choice_error(quality, "quality", ("draft", "better", "full")) or _int_error(
        long_side, "long_side", 16, 4096
    )
    if error:
        return [f"Error: {error}"]
    if quality == "draft" and long_side == 320:
        params: dict = {"quality": quality}
    else:
        params = {"quality": quality, "long_side": long_side}
    try:
        if ctx is not None and _has_progress_token(ctx):
            result = await _preview_with_progress(ctx, params)
        else:
            result = await asyncio.to_thread(bridge.call, "render_preview", params, 300)
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

    error = _int_error(max_edge, "max_edge", 0, 8192)
    if error:
        return [f"Error: {error}"]
    if path and not isinstance(path, str):
        return ["Error: path must be a file path."]
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

    error = (
        _choice_error(
            type, "type", ("fractal", "hybrid", "boolean_add", "boolean_mul", "boolean_sub", "primitive")
        )
        or _int_error(parent_id, "parent_id", 0, 100000)
        or _choice_error(
            primitive,
            "primitive",
            (
                "sphere",
                "box",
                "plane",
                "torus",
                "cylinder",
                "cone",
                "ellipsoid",
                "prism",
                "rectangle",
                "circle",
                "water",
            ),
        )
        or _int_error(formula_iterations, "formula_iterations", 0, 250)
    )
    if error:
        return f"Error: {error}"
    if radius:
        error = _number_error(radius, "radius", 0)
        if error:
            return f"Error: {error}"
    if position is not None:
        error = _vec3_error(position, "position")
        if error:
            return f"Error: {error}"
    if scale:
        error = _number_error(scale, "scale")
        if error:
            return f"Error: {error}"
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

    error = _int_error(id, "id", 1, 100000)
    if error:
        return f"Error: {error}"
    if position is not None:
        error = _vec3_error(position, "position")
        if error:
            return f"Error: {error}"
    if rotation is not None:
        error = _vec3_error(rotation, "rotation")
        if error:
            return f"Error: {error}"
    if scale:
        error = _number_error(scale, "scale")
        if error:
            return f"Error: {error}"
    if material:
        error = _int_error(material, "material", 1, 99)
        if error:
            return f"Error: {error}"
    if enabled is not None and not isinstance(enabled, bool):
        return "Error: enabled must be true or false."
    if parent_id is not None:
        error = _int_error(parent_id, "parent_id", 0, 100000)
        if error:
            return f"Error: {error}"
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

    error = _int_error(id, "id", 1, 100000)
    if error:
        return f"Error: {error}"
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

    error = _int_error(id, "id", 1, 99)
    if error:
        return f"Error: {error}"
    params: dict = {"id": id}
    if surface_color is not None:
        error = _color_error(surface_color, "surface_color")
        if error:
            return f"Error: {error}"
        params["surface_color"] = surface_color
    if specular is not None:
        error = _number_error(specular, "specular", 0)
        if error:
            return f"Error: {error}"
        params["specular"] = specular
    if specular_width is not None:
        error = _number_error(specular_width, "specular_width", 0)
        if error:
            return f"Error: {error}"
        params["specular_width"] = specular_width
    if reflectance is not None:
        error = _number_error(reflectance, "reflectance", 0)
        if error:
            return f"Error: {error}"
        params["reflectance"] = reflectance
    if metallic is not None and not isinstance(metallic, bool):
        return "Error: metallic must be true or false."
    if metallic is not None:
        params["metallic"] = metallic
    if use_colors_from_palette is not None and not isinstance(use_colors_from_palette, bool):
        return "Error: use_colors_from_palette must be true or false."
    if use_colors_from_palette is not None:
        params["use_colors_from_palette"] = use_colors_from_palette
    if surface_color_gradient:
        error = _gradient_error(surface_color_gradient)
        if error:
            return f"Error: {error}"
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

    error = _int_error(id, "id", 1, 16)
    if error:
        return f"Error: {error}"
    if enabled is not None and not isinstance(enabled, bool):
        return "Error: enabled must be true or false."
    if intensity is not None:
        error = _number_error(intensity, "intensity", 0)
        if error:
            return f"Error: {error}"
    if position is not None:
        error = _vec3_error(position, "position")
        if error:
            return f"Error: {error}"
    params: dict = {"id": id}
    if enabled is not None:
        params["enabled"] = enabled
    if intensity is not None:
        params["intensity"] = intensity
    if position is not None:
        params["position"] = position
    if color is not None:
        error = _color_error(color, "color")
        if error:
            return f"Error: {error}"
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
    then a preview. If the frame goes empty, call frame_scene before you
    invent a new camera.
    """

    if camera is not None:
        error = _vec3_error(camera, "camera")
        if error:
            return f"Error: {error}"
    if target is not None:
        error = _vec3_error(target, "target")
        if error:
            return f"Error: {error}"
    if rotation is not None:
        error = _vec3_error(rotation, "rotation")
        if error:
            return f"Error: {error}"
    if fov is not None:
        error = _number_error(fov, "fov", 0, 720)
        if error:
            return f"Error: {error}"
    if distance is not None:
        error = _number_error(distance, "distance", 0)
        if error:
            return f"Error: {error}"
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
def frame_scene(target: list | None = None) -> str:
    """Pull the camera back until the shape fits in the frame. One undo step.

    Use this when a preview is empty or nearly black and you have not moved
    the camera on purpose. It measures the fractal and places the eye far
    enough back to see the surface, keeping the current viewing direction.
    target defaults to [0, 0, 0], which is where a fresh formula sits. It does
    not change the formula. A Mandelbox left at scale 2 often needs this,
    because the default camera is inside that solid; the other fix is
    mandelbox_scale -1.5. Then render_preview.
    """

    if target is not None:
        error = _vec3_error(target, "target")
        if error:
            return f"Error: {error}"
    params: dict = {}
    if target is not None:
        params["target"] = target
    return _call("frame_scene", params)


def _screen_error(value: object, label: str) -> str | None:
    if (
        isinstance(value, (list, tuple))
        and len(value) == 2
        and all(_is_number(item) for item in value)
        and all(0 <= float(item) <= 1 for item in value)
    ):
        return None
    return f"{label} must be [x, y] from 0 to 1, origin at the top left."


def _ray_error(value: object, label: str) -> str | None:
    if (
        isinstance(value, (list, tuple))
        and len(value) == 2
        and all(_is_number(item) for item in value)
        and all(-1 <= float(item) <= 2 for item in value)
    ):
        return None
    return f"{label} must be [x, y]. 0, 0 is the top left. -1 to 2 reaches outside the picture."


_PREFER = ("detail", "silhouette", "pocket", "fold", "space")
_WEIGHTS = ("fold", "contrast", "edge", "ridge", "pocket", "flat", "sky", "inside")


def _prefer_error(value: object) -> str | None:
    if value is None:
        return None
    if isinstance(value, str):
        if value not in _PREFER:
            return "prefer must be detail, silhouette, pocket, fold, or space."
        return None
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "preset":
                if item not in _PREFER:
                    return "preset must be detail, silhouette, pocket, fold, or space."
                continue
            if key not in _WEIGHTS:
                return f"unknown weight {key}. Weights are fold, contrast, edge, ridge, pocket, flat, sky, and inside."
            if not _is_number(item):
                return f"{key} must be a number."
            limit = 1 if key == "flat" else 2
            if not 0 <= float(item) <= limit:
                return f"{key} must be from 0 to {limit}."
        return None
    return "prefer is a name, or an object of weights."


def _index_range(value: object, label: str) -> str | None:
    if isinstance(value, bool) or not _is_number(value) and not isinstance(value, (list, tuple)):
        return f"{label} is an index, or [first, last], 0-based."
    items = (value,) if _is_number(value) else value
    if len(items) not in (1, 2) or not all(_is_number(item) and float(item) == int(item) and int(item) >= 0 for item in items):
        return f"{label} is an index, or [first, last], 0-based."
    return None


@server.tool()
def probe_view(
    x: float | None = None,
    y: float | None = None,
    at: str = "",
    column: int | None = None,
    row: int | None = None,
    grid: int = 0,
    prefer: str | dict | None = None,
) -> str:
    """Where a spot in the picture is, in the same coordinates as the viewfinder.

    Call it with no arguments after render_preview. That returns the ranked
    square cells: column and row are 0-based, ranked is best first, and label
    is sky, inside, edge, ridge, pocket, slope, or flat. prefer re-ranks that
    grid without another render, and the choice sticks until the next preview.
    detail favors a silhouette or a fold, and sky stays last. silhouette favors
    a shape sticking into the sky. Sky beside that shape is kept, and the
    protrusion still ranks first. pocket favors a recess. fold favors surface
    detail and ignores the sky. space favors open sky next to a shape. prefer
    may also be weights: fold, contrast, edge, ridge, pocket, flat, sky, and
    inside. flat is 0 to 1. The others are 0 to 2. Labels do not change.

    column and row march that one cell and return its world position. x and y
    are fractions of the frame. 0, 0 is the top left. -1 to 2 reaches outside
    the picture, which is how a look_around point is used. at is center,
    crosshair, left, right, top, bottom, upper_left, upper_right, lower_left,
    or lower_right. grid 1 to 5 marches a live square of that many cells on a
    side. Prefer the preview ranking.

    depth is the distance from the eye to the surface along that ray. It comes
    from the distance estimator, so you do not need a finished z-buffer.
    from_camera splits that point into forward, right, and up. position is the
    world point to pass to look_at or place_in_view. Do not round it and retype
    it into set_camera. node_id is the objects-tree node that was hit.
    preview_matches false means the camera or fov changed. viewport_depth is
    the last window image and can be stale. hit false means the ray found no
    surface.
    """

    error = _prefer_error(prefer)
    if error:
        return f"Error: {error}"
    if prefer is not None and any((x is not None, at, column is not None, grid)):
        return "Error: prefer re-ranks the preview grid. Call probe_view with prefer and no point."
    if grid:
        error = _int_error(grid, "grid", 1, 5)
        if error:
            return f"Error: {error}"
    if (x is None) != (y is None):
        return "Error: pass both x and y, or neither."
    if (column is None) != (row is None):
        return "Error: pass both column and row, or neither."
    chosen = [name for name, value in (("point", x is not None), ("at", bool(at)), ("cell", column is not None), ("grid", bool(grid))) if value]
    if len(chosen) > 1:
        return "Error: pass one of x and y, at, column and row, or grid."
    if x is not None:
        error = _ray_error([x, y], "point")
        if error:
            return f"Error: {error}"
    if column is not None:
        error = _index_range(column, "column") or _index_range(row, "row")
        if error:
            return f"Error: {error}"
    if at and not isinstance(at, str):
        return "Error: at must be a place name such as right or center."
    params: dict = {}
    if x is not None:
        params["x"] = x
        params["y"] = y
    if at:
        params["at"] = at
    if column is not None:
        params["column"] = int(column)
        params["row"] = int(row)
    if grid:
        params["grid"] = grid
    if prefer is not None:
        params["prefer"] = prefer
    return _call("probe_view", params)


@server.tool()
def look_around(prefer: str | dict | None = None) -> str:
    """Look for a more interesting direction without moving the camera.

    Casts a coarse fan across the frame and about half a frame past each edge.
    directions are ranked. best is the highest score. prefer is the same set
    as probe_view: detail, silhouette, pocket, fold, space, or weights.
    silhouette looks for a shape sticking into the sky. space looks for open
    sky beside a shape. outside true means that x and y are not in the current
    picture. Pass that point to navigate_camera toward or look_at, then
    render_preview. This does not record an undo step.
    """

    error = _prefer_error(prefer)
    if error:
        return f"Error: {error}"
    params: dict = {}
    if prefer is not None:
        params["prefer"] = prefer
    return _call("look_around", params)


def _spot_error(value: object, label: str) -> str | None:
    if isinstance(value, (list, tuple)) and len(value) == 3 and all(_is_number(item) for item in value):
        return None
    return _ray_error(value, label)


@server.tool()
def navigate_camera(
    move: str = "",
    turn: str = "",
    orbit: str = "",
    toward: list | None = None,
    away: bool = False,
    look_at: list | bool | None = None,
    around: list | None = None,
    frame: str = "",
    zoom: str = "",
    columns: list | int | None = None,
    rows: list | int | None = None,
    moves: list | None = None,
    steps: int = 1,
    step: float | None = None,
) -> str:
    """Move the camera the way the navigation buttons do. One undo step.

    Pass exactly one of move, turn, orbit, toward, look_at, zoom, frame, or
    moves. These are relative. You do not need the absolute camera decimals.

    move is forward, backward, left, right, up, or down. The eye and the
    target move together, so the viewing direction stays. One step is half the
    distance from the eye to the surface (camera_movement_step, default 0.5),
    measured with the same distance estimate the buttons use. step replaces
    that fraction for this call. steps repeats the click, up to 50.

    turn is left, right, up, down, roll_left, or roll_right. The eye stays
    put. step is degrees and defaults to 15. orbit is left, right, up, or
    down and circles the target by that same angle. around is [x, y] or a
    world [x, y, z]: the eye then circles that surface point, as a viewfinder
    drag does, and the point stays in frame.

    zoom in or out changes the field of view by step degrees (default 5).

    toward is [x, y], origin at the top left. -1 to 2 reaches outside the
    picture, which is how a look_around point is used. The eye moves along
    that ray toward the surface and looks at it, stopping short of the
    surface. away true backs up along the same ray. look_at [x, y] aims at
    that spot without moving the eye. look_at [x, y, z] aims at a world point
    from probe_view. look_at true aims at a cell range.

    columns and rows select preview cells, 0-based. Each is an index, or
    [first, last] inclusive. Alone, that range is the toward target. With
    frame, the range is the spot you park on the intersection. With orbit,
    the eye circles the range. With look_at true, the eye stays put and aims
    at the range. The center of the range is used, or the best hit inside it
    when the center misses.

    frame puts a spot on a thirds or golden intersection: left_third,
    right_third, upper_third, lower_third, upper_left, upper_right,
    lower_left, lower_right, center, or golden_left, golden_right,
    golden_upper_left, golden_upper_right, golden_lower_left,
    golden_lower_right. look_at is the spot. Omit look_at to use the center
    of the picture. The eye moves around the spot and keeps the distance.

    moves is a list of those actions, up to 20, stored as one undo step.
    Then render_preview.
    """

    frames = (
        "center",
        "left_third",
        "right_third",
        "upper_third",
        "lower_third",
        "upper_left",
        "upper_right",
        "lower_left",
        "lower_right",
        "golden_left",
        "golden_right",
        "golden_upper_left",
        "golden_upper_right",
        "golden_lower_left",
        "golden_lower_right",
    )
    if (columns is None) != (rows is None):
        return "Error: pass both columns and rows."
    if columns is not None:
        error = _index_range(columns, "columns") or _index_range(rows, "rows")
        if error:
            return f"Error: {error}"
        if toward is not None or around is not None or (look_at not in (None, True)):
            return "Error: pass columns and rows, or a screen point, not both."
    if moves is not None:
        if any((move, turn, orbit, toward, look_at, around, frame, zoom, columns is not None)):
            return "Error: pass moves, or one action, not both."
        if not isinstance(moves, list) or not moves or len(moves) > 20:
            return "Error: moves must be 1 to 20 actions."
        cleaned = []
        for item in moves:
            if not isinstance(item, dict):
                return "Error: each move is an object, such as {\"move\": \"forward\"}."
            if "moves" in item:
                return "Error: a move list cannot contain another move list."
            cleaned.append(item)
        params: dict = {"moves": cleaned}
        return _call("navigate_camera", params)
    range_toward = columns is not None and not any((move, turn, orbit, frame, zoom, look_at is True))
    chosen = [
        name
        for name, value in (
            ("move", move),
            ("turn", turn),
            ("orbit", orbit),
            ("toward", toward or range_toward),
            ("look_at", (look_at if not frame else None) or (look_at is True)),
            ("frame", frame),
            ("zoom", zoom),
        )
        if value
    ]
    if len(chosen) != 1:
        return "Error: pass one of move, turn, orbit, toward, look_at, zoom, frame, or moves."
    if look_at is True and columns is None:
        return "Error: look_at true needs columns and rows. Or pass look_at as a point."
    if around is not None and not orbit:
        return "Error: around is used with orbit."
    error = _int_error(steps, "steps", 1, 50)
    if error:
        return f"Error: {error}"
    if step is not None:
        error = _number_error(step, "step", 0)
        if error:
            return f"Error: {error}"
        if step == 0:
            return "Error: step must be greater than 0."
    if away and not toward and columns is None:
        return "Error: away is used with toward."
    params: dict = {"steps": steps}
    if move:
        error = _choice_error(move, "move", ("forward", "backward", "back", "left", "right", "up", "down"))
        if error:
            return f"Error: {error}"
        params["move"] = move
    if turn:
        error = _choice_error(turn, "turn", ("left", "right", "up", "down", "roll_left", "roll_right"))
        if error:
            return f"Error: {error}"
        params["turn"] = turn
    if orbit:
        error = _choice_error(orbit, "orbit", ("left", "right", "up", "down"))
        if error:
            return f"Error: {error}"
        params["orbit"] = orbit
    if toward is not None:
        error = _ray_error(toward, "toward")
        if error:
            return f"Error: {error}"
        params["toward"] = [float(toward[0]), float(toward[1])]
        if away:
            params["away"] = True
    elif away:
        params["away"] = True
    if look_at is True:
        params["look_at"] = True
    elif look_at is not None:
        if isinstance(look_at, (list, tuple)) and len(look_at) == 3 and all(_is_number(item) for item in look_at):
            params["look_at"] = [float(item) for item in look_at]
        else:
            error = _ray_error(look_at, "look_at")
            if error:
                return "Error: look_at must be [x, y], a world [x, y, z], or true with columns and rows."
            params["look_at"] = [float(look_at[0]), float(look_at[1])]
    if columns is not None:
        params["columns"] = int(columns) if _is_number(columns) else [int(item) for item in columns]
        params["rows"] = int(rows) if _is_number(rows) else [int(item) for item in rows]
    if around is not None:
        error = _spot_error(around, "around")
        if error:
            return "Error: around must be [x, y] in the picture, or a world [x, y, z]."
        params["around"] = [float(item) for item in around]
    if frame:
        error = _choice_error(frame, "frame", frames)
        if error:
            return f"Error: {error}"
        params["frame"] = frame
    if zoom:
        error = _choice_error(zoom, "zoom", ("in", "out"))
        if error:
            return f"Error: {error}"
        params["zoom"] = zoom
    if step is not None:
        params["step"] = step
    return _call("navigate_camera", params)


@server.tool()
def place_in_view(
    kind: str,
    x: float | None = None,
    y: float | None = None,
    position: list | None = None,
    id: int = 1,
    behind: bool = False,
    offset: float | None = None,
    role: str = "",
) -> str:
    """Put a light, an object, focus, or fog on a spot in the picture. One undo step.

    kind light places that light (id defaults to 1). role key puts it up and
    camera-right of the surface normal. role fill is the other side and
    dimmer. role pair does both, using id and the next light. role rim, or
    behind true, puts it past the surface along the view ray. role camera
    keeps the light on the eye, up and to the right; its position is camera
    space, x right, y up, z forward. With no role the light sits just in
    front of the spot. Intensity is based on at least a quarter of the depth,
    so a light against the surface still shows up in a draft.

    kind object with id from get_objects_tree sets that node's origin on the
    surface. Add the sphere first, then place it. kind focus sets DOF_focus
    to the depth of that spot. kind fog sets basic_fog_visibility to that depth.

    Pass x and y (0 to 1, origin at the top left) or position, the world
    point probe_view returned. offset is how far to lift a light or object
    toward the camera. For a light behind the surface it is how far past the
    surface to search. Then render_preview.
    """

    error = _choice_error(kind, "kind", ("light", "object", "focus", "fog"))
    if error:
        return f"Error: {error}"
    if (x is None) != (y is None):
        return "Error: pass both x and y, or neither."
    if (x is None) == (position is None):
        return "Error: pass x and y, or position from probe_view, not both."
    if x is not None:
        error = _screen_error([x, y], "point")
        if error:
            return f"Error: {error}"
    if position is not None:
        error = _vec3_error(position, "position")
        if error:
            return f"Error: {error}"
    if kind == "light":
        error = _int_error(id, "id", 1, 16)
        if error:
            return f"Error: {error}"
    if kind == "object":
        error = _int_error(id, "id", 1, 100000)
        if error:
            return f"Error: {error}"
    if behind and kind != "light":
        return "Error: behind is for a light."
    if role:
        error = _choice_error(role, "role", ("key", "fill", "rim", "camera", "pair"))
        if error:
            return f"Error: {error}"
        if kind != "light":
            return "Error: role is for a light."
        if role == "pair":
            error = _int_error(id, "id", 1, 15)
            if error:
                return "Error: a pair needs id and the next light, so id must be 15 or less."
    if offset is not None:
        error = _number_error(offset, "offset", 0)
        if error:
            return f"Error: {error}"
    params: dict = {"kind": kind, "id": id}
    if x is not None:
        params["x"] = x
        params["y"] = y
    if position is not None:
        params["position"] = position
    if behind:
        params["behind"] = True
    if offset is not None:
        params["offset"] = offset
    if role:
        params["role"] = role
    return _call("place_in_view", params)


def _spot_params(value: object, label: str) -> tuple[list | None, str | None]:
    if isinstance(value, (list, tuple)) and len(value) == 3 and all(_is_number(item) for item in value):
        return [float(item) for item in value], None
    error = _screen_error(value, label)
    if error:
        return None, error
    return [float(value[0]), float(value[1])], None


@server.tool()
def measure_view(a: list, b: list) -> str:
    """Distance between two spots in the picture.

    a and b are [x, y] (origin at the top left) or world [x, y, z] points from
    probe_view. Returns the straight-line distance, the midpoint, and both
    world positions. Use the distance with fit_to_view. This does not edit
    the scene.
    """

    first, error = _spot_params(a, "a")
    if error:
        return f"Error: {error}"
    second, error = _spot_params(b, "b")
    if error:
        return f"Error: {error}"
    return _call("measure_view", {"a": first, "b": second})


@server.tool()
def fit_to_view(
    kind: str,
    id: int = 0,
    a: list | None = None,
    b: list | None = None,
    x: float | None = None,
    y: float | None = None,
    position: list | None = None,
    at: str = "",
    diameter: bool = False,
    padding: float = 1.2,
) -> str:
    """Size a primitive or wrap the limits around a feature. One undo step.

    kind radius sets the primitive radius (or size, or the node scale) of id
    to the distance between a and b. diameter true uses half of that, for two
    points on opposite edges. Add the sphere first, then fit it.

    kind limits wraps limit_min and limit_max around the feature and turns
    limits on. Pass a and b for a box through those two spots, or x and y
    (or position, or at) for one spot. One spot uses the viewfinder scan: a
    box about as large as the distance from the eye, then tightened to the
    surface. padding defaults to 1.2. Then render_preview.
    """

    error = _choice_error(kind, "kind", ("radius", "limits"))
    if error:
        return f"Error: {error}"
    error = _number_error(padding, "padding", 0, 10)
    if error:
        return f"Error: {error}"
    if padding == 0:
        return "Error: padding must be greater than 0."
    params: dict = {"kind": kind, "padding": padding}
    if a is not None or b is not None:
        if a is None or b is None:
            return "Error: pass both a and b."
        first, error = _spot_params(a, "a")
        if error:
            return f"Error: {error}"
        second, error = _spot_params(b, "b")
        if error:
            return f"Error: {error}"
        params["a"] = first
        params["b"] = second
    if kind == "radius":
        error = _int_error(id, "id", 1, 100000)
        if error:
            return f"Error: {error}"
        if "a" not in params:
            return "Error: radius needs a and b."
        params["id"] = id
        if diameter:
            params["diameter"] = True
    else:
        if x is not None or y is not None:
            if x is None or y is None:
                return "Error: pass both x and y."
            error = _screen_error([x, y], "point")
            if error:
                return f"Error: {error}"
            params["x"] = x
            params["y"] = y
        if position is not None:
            error = _vec3_error(position, "position")
            if error:
                return f"Error: {error}"
            params["position"] = position
        if at:
            if not isinstance(at, str):
                return "Error: at must be a place name such as center or right."
            params["at"] = at
        if "a" not in params and "x" not in params and "position" not in params and "at" not in params:
            return "Error: limits need a and b, or x and y, or position, or at."
    return _call("fit_to_view", params)


@server.tool()
async def render_still(
    path: str = "",
    name: str = "",
    quality: str = "full",
    width: int = 0,
    height: int = 0,
    long_side: int = 0,
    wait: bool = True,
    ctx: Context = None,
) -> str:
    """Start a still render.

    name is the picture title and becomes the file name. Pass path as the
    folder. Do not name the file after the loaded settings file. settings.fract
    is the session, not the picture. A title such as mandelbulb-mandelbox is
    the name. name may not contain a folder. When name is omitted, path may be
    the full file path.

    quality full keeps the session's detail and effects, on a copy, so the open
    file's settings are not rewritten. quality draft caps detail at 0.5 and
    turns the expensive effects off. long_side above 0 rescales and is capped
    at 640 for a draft and 4096 for full. Each side is also capped at 4096, and
    a clamped job says so in note. antialiasing_size 2 is 4 samples per output
    pixel. That is the 4x supersample. Do not also ask for an image 4 times
    wider. detail_level is measured in output pixels, so a 4K frame with size 2
    costs far more than four screen previews.

    This host usually sends a progress token. wait true (the default) then
    blocks until the still finishes, and the percent and ETA in render_status
    are not visible until this call returns. For anything slower than a small
    test, pass wait false. message looks like
    "Done 8%, elapsed: ..., estimated to end: ...". get_image fetches the PNG
    when done is true. A wait that lasts 3 minutes returns the current status
    and leaves the job running. Call render_status with that id and wait true
    to hold until it finishes. That return is the completion notice. There is
    no notification after a tool call has already returned. stop_render sets
    the stop flag with no dialog.
    """

    error = _choice_error(quality, "quality", ("draft", "better", "full"))
    if error:
        return f"Error: {error}"
    for label, value in (("width", width), ("height", height), ("long_side", long_side)):
        if value:
            error = _int_error(value, label, 1, 100000)
            if error:
                return f"Error: {error}"
    if path and not isinstance(path, str):
        return "Error: path must be a file path."
    if name and not isinstance(name, str):
        return "Error: name must be a picture title."
    if isinstance(name, str) and ("/" in name or "\\" in name or name.strip() in {".", ".."}):
        return "Error: name is the picture title, not a folder. Put the folder in path."
    params: dict = {"quality": quality}
    if path:
        params["path"] = path
    if isinstance(name, str) and name.strip():
        params["name"] = name.strip()
    if width:
        params["width"] = width
    if height:
        params["height"] = height
    if long_side:
        params["long_side"] = long_side
    try:
        started = await asyncio.to_thread(bridge.call, "render_still", params, 60)
        finished = await _await_job(ctx, started, wait) if ctx is not None else started
        return _dump(finished)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return f"Error: {exc}"


@server.tool()
async def render_status(id: str = "", wait: bool = False, ctx: Context = None) -> str:
    """Progress of a still or animation job: status, frame, progress, image path, error.

    Omit id to ask about the job that is running, or the last one started.
    done is true when status is finished, stopped, failed, or cancelled.
    cancelled means the user pressed Stop in the Background renders window:
    cancelled_by_user is true and error is "The user cancelled this render."
    That stop was intentional. Do not start a replacement unless asked.
    wait false returns immediately. wait true holds this call open until the
    job finishes, which is how a render notifies you: the tool return is the
    completion notice. Pass the id from a still or animation that already
    returned because its 3 minute wait ended. There is no notification for a
    call that has already returned.
    """

    if wait:
        try:
            status = await _wait_for_completion(ctx, id)
            return _dump(status)
        except (RuntimeError, ConnectionError, OSError) as exc:
            return f"Error: {exc}"
    return _call("render_status", {"id": id} if id else None)


@server.tool()
def stop_render(id: str = "") -> str:
    """Stop one background render, or a render the open window started.

    Pass id to stop that background job. Omit it to stop the most recent one
    and a render the open window started. Sets the stop flag. There is no
    confirmation dialog. If the user already pressed Stop in the Background
    renders window, the result has cancelled_by_user true and error "The user
    cancelled this render." An agent stop stays status stopped.
    """

    return _call("stop_render", {"id": id} if id else None)


@server.tool()
def resume_render(source: str = "", resume: bool = True) -> str:
    """Continue an interrupted still or animation from its last finished scanline.

    The startup dialog asks the same question. This answers that dialog when it
    is open, and otherwise resumes or discards the checkpoint itself. source is
    background (an MCP still or animation) or window. Omit it when only one
    checkpoint exists, or while the dialog is open. resume false discards it.
    Poll render_status after a resume. Do not start a new render_still over a
    checkpoint that is still waiting.
    """

    if source and source not in ("background", "window"):
        return "Error: source must be background or window."
    params: dict = {"resume": bool(resume)}
    if source:
        params["source"] = source
    return _call("resume_render", params)


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

    error = _int_error(frames_per_keyframe, "frames_per_keyframe", 1, 10000) or _int_error(
        index, "index", -1, 100000
    )
    if error:
        return f"Error: {error}"
    params: dict = {"frames_per_keyframe": frames_per_keyframe}
    if index >= 0:
        params["index"] = index
    return _call("add_keyframe", params)


@server.tool()
def update_keyframe(index: int, frames_per_keyframe: int = 0) -> str:
    """Replace key `index` with the camera as it is right now. One undo step.

    Pass frames_per_keyframe to change how many pictures lead to the next key.
    """

    error = _int_error(index, "index", 0, 100000) or _int_error(
        frames_per_keyframe, "frames_per_keyframe", 0, 10000
    )
    if error:
        return f"Error: {error}"
    params: dict = {"index": index}
    if frames_per_keyframe:
        params["frames_per_keyframe"] = frames_per_keyframe
    return _call("update_keyframe", params)


@server.tool()
def delete_keyframe(index: int) -> str:
    """Remove one key. One undo step."""

    error = _int_error(index, "index", 0, 100000)
    if error:
        return f"Error: {error}"
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

    error = _choice_error(kind, "kind", ("orbit", "flythrough", "frames")) or _int_error(
        count, "count", 2, 360
    )
    if error:
        return f"Error: {error}"
    for label, value in (("radius", radius), ("height", height)):
        if value is not None:
            error = _number_error(value, label)
            if error:
                return f"Error: {error}"
    if fov is not None:
        error = _number_error(fov, "fov", 0, 720)
        if error:
            return f"Error: {error}"
    for label, value in (("target", target), ("start", start), ("end", end)):
        if value is not None:
            error = _vec3_error(value, label)
            if error:
                return f"Error: {error}"
    if frames is not None:
        if not isinstance(frames, list) or len(frames) < 2:
            return 'Error: frames needs at least two {camera, target} objects.'
        for frame in frames:
            if not isinstance(frame, dict):
                return 'Error: frames needs at least two {camera, target} objects.'
            if "camera" in frame:
                error = _vec3_error(frame["camera"], "camera")
                if error:
                    return f"Error: {error}"
            if "target" in frame:
                error = _vec3_error(frame["target"], "target")
                if error:
                    return f"Error: {error}"
            if "fov" in frame:
                error = _number_error(frame["fov"], "fov", 0, 720)
                if error:
                    return f"Error: {error}"
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
async def render_animation(
    mode: str = "keyframe",
    start: int = 0,
    end: int = 0,
    directory: str = "",
    quality: str = "draft",
    long_side: int = 160,
    max_frames: int = 24,
    width: int = 0,
    height: int = 0,
    contact_sheet: bool = False,
    frame_edge: int = 96,
    columns: int = 0,
    wait: bool = True,
    ctx: Context = None,
) -> str:
    """Start a keyframe or flight render.

    mode is keyframe or flight. end 0 means through the last frame. quality
    defaults to draft. long_side defaults to 160 so a sketch stays small; pass
    0 only when you also pass width and height. The range is refused when it is
    longer than max_frames (default 24, hard maximum 500). Frames are written
    as frame_NNNN.png. Without a progress token, or with wait false, this
    returns a job id immediately. Call render_status with wait true to hold
    until the job finishes; that return is the completion notice. With a
    progress token and wait true it reports progress and, after 3 minutes,
    returns the current status while the job keeps going. contact_sheet true
    writes one thumbnail PNG when the job ends. get_contact_sheet builds that
    strip on demand. get_image returns a single frame.
    """

    error = (
        _choice_error(mode, "mode", ("keyframe", "flight"))
        or _choice_error(quality, "quality", ("draft", "better", "full"))
        or _int_error(start, "start", 0, 100000)
        or _int_error(end, "end", 0, 100000)
        or _int_error(max_frames, "max_frames", 1, 500)
        or _int_error(frame_edge, "frame_edge", 16, 320)
        or _int_error(columns, "columns", 0, 100)
    )
    if error:
        return f"Error: {error}"
    if width or height:
        error = _int_error(width, "width", 1, 100000) or _int_error(height, "height", 1, 100000)
        if error:
            return f"Error: {error}"
    elif long_side:
        error = _int_error(long_side, "long_side", 1, 100000)
        if error:
            return f"Error: {error}"
    params: dict = {
        "mode": mode,
        "start": start,
        "quality": quality,
        "max_frames": max_frames,
        "contact_sheet": contact_sheet,
        "frame_edge": frame_edge,
    }
    if columns:
        params["columns"] = columns
    if end:
        params["end"] = end
    if directory:
        params["directory"] = directory
    if width and height:
        params["width"] = width
        params["height"] = height
    elif long_side:
        params["long_side"] = long_side
    try:
        started = await asyncio.to_thread(bridge.call, "render_animation", params, 60)
        finished = await _await_job(ctx, started, wait) if ctx is not None else started
        return _dump(finished)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return f"Error: {exc}"


@server.tool(structured_output=False)
def get_contact_sheet(id: str = "", frame_edge: int = 96, columns: int = 0) -> list:
    """One PNG that tiles an animation's frames so you can review them together.

    Pass the job id from render_animation, or omit it for the latest job that
    has frames. frame_edge is the thumbnail size, from 16 to 320. columns 0
    lays out up to 6 across. The real frames stay as frame_NNNN.png. Use this
    instead of opening every frame.
    """

    error = _int_error(frame_edge, "frame_edge", 16, 320) or _int_error(columns, "columns", 0, 100)
    if error:
        return [f"Error: {error}"]
    params: dict = {"frame_edge": frame_edge}
    if id:
        params["id"] = id
    if columns:
        params["columns"] = columns
    try:
        result = bridge.call("get_contact_sheet", params)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return [f"Error: {exc}"]
    data = open(result["path"], "rb").read()
    note = _dump({key: value for key, value in result.items() if key != "path"})
    return [note, Image(data=data, format="png")]


@server.tool()
def list_presets() -> str:
    """The preset cards on the toolbar.

    user is the person's own row. agent is the group this bridge saved, drawn
    after a separator so it stays out of their picks. Each entry has the
    formulas in that file. inspect_preset shows one thumbnail.
    """

    return _call("list_presets")


@server.tool(structured_output=False)
def inspect_preset(name: str) -> list:
    """One preset card: formulas, camera, and its toolbar thumbnail.

    name is the title from list_presets. The image is the thumbnail already on
    the strip. A user preset and an agent preset can both be inspected.
    """

    if not isinstance(name, str) or not name.strip():
        return ["Error: name is the preset title from list_presets."]
    try:
        result = bridge.call("inspect_preset", {"name": name}, timeout=90)
    except (RuntimeError, ConnectionError, OSError) as exc:
        return [f"Error: {exc}"]
    path = result.get("thumbnail_path")
    note = _dump({key: value for key, value in result.items() if key != "thumbnail_path"})
    if not path:
        return [note]
    data = open(path, "rb").read()
    return [note, Image(data=data, format="png")]


@server.tool()
def save_preset(name: str, overwrite: bool = False) -> str:
    """Add the live session as an agent preset card.

    It is stored apart from the user's presets and shown after a separator,
    with a mark on the card. The session itself is not changed. Say the name
    before you call this.
    """

    if not isinstance(name, str) or not name.strip():
        return "Error: name is a short title for the preset."
    if not isinstance(overwrite, bool):
        return "Error: overwrite must be true or false."
    return _call("save_preset", {"name": name, "overwrite": overwrite})


@server.tool()
def load_preset(name: str) -> str:
    """Load a user or agent preset into the live session. One undo step.

    Does not modify the preset file. If the name matches more than one card,
    the error lists them.
    """

    if not isinstance(name, str) or not name.strip():
        return "Error: name is the preset title from list_presets."
    return _call("load_preset", {"name": name})


@server.tool()
def delete_preset(name: str) -> str:
    """Remove one agent preset card.

    Refuses a preset the user added. Only presets saved by this bridge can be
    deleted. The live session is left as it is.
    """

    if not isinstance(name, str) or not name.strip():
        return "Error: name is an agent preset from list_presets."
    return _call("delete_preset", {"name": name})


@server.tool()
def load_settings(path: str) -> str:
    """Load a .fract file into the live session. One undo step. Does not modify the file."""

    if not isinstance(path, str) or not path.strip():
        return "Error: path must be a .fract file."
    return _call("load_settings", {"path": path})


@server.tool()
def load_example(name: str) -> str:
    """Load one shipped example by filename or relative path. One undo step.

    If the name matches more than one file, the error lists them. Ask the user
    which one. Loading does not write the example.
    """

    if not isinstance(name, str) or not name.strip():
        return "Error: name must be an example filename."
    return _call("load_example", {"name": name})


@server.tool()
def save_settings(path: str, overwrite: bool = False) -> str:
    """Write the live session to an explicit path.

    Refuses the file that is currently loaded unless overwrite is true, and
    refuses anything inside the shared examples directory. Say which path you
    are writing before you call this. save_preset is the way to add a preset
    card. This tool writes an arbitrary settings file.
    """

    if not isinstance(path, str) or not path.strip():
        return "Error: path must be a file path."
    if not isinstance(overwrite, bool):
        return "Error: overwrite must be true or false."
    return _call("save_settings", {"path": path, "overwrite": overwrite})


@server.tool()
def undo() -> str:
    """Undo the last edit made in this Mandelbulber process.

    Stops at the scene from when this process finished starting (after autosave
    recovery, if that dialog was answered). It does not walk undo files left by
    an older session. The window's Undo menu can still reach those. Each MCP
    edit is one step: parameters, formula, tree, look, camera, animation, or a
    load.
    """

    return _call("undo")


@server.tool()
def redo() -> str:
    """Redo an edit that was just undone."""

    return _call("redo")


def main() -> None:
    server.run(transport="stdio")


if __name__ == "__main__":
    main()
