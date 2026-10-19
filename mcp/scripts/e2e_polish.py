#!/usr/bin/env python3
# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stop-before-execute, undo floor, framing, contact sheet, and progress."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import sys
import time
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from e2e_test import as_json, first_image, raw_call, session_file


def lit(path: Path) -> None:
    from PIL import Image

    image = Image.open(path)
    peak = max(channel[1] for channel in image.getextrema())
    if peak == 0:
        raise SystemExit(f"{path.name} is black {image.size}")
    print(path.name, image.size, image.getextrema(), path.stat().st_size)


def undo_dir() -> Path:
    home = Path.home()
    for folder in (home / ".mandelbulber" / "undo", home / "mandelbulber" / "undo"):
        if folder.is_dir():
            return folder
    folder = home / ".mandelbulber" / "undo"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def run(artifact_dir: Path, binary: str) -> None:
    planted = undo_dir()
    dummies = [planted / "undo_91.fract", planted / "undo_92.fract"]
    for path in dummies:
        path.write_text(
            "# planted by e2e_polish; MCP undo must not load this\n",
            encoding="utf-8",
        )
    print("planted", [str(path) for path in dummies])

    env = dict(os.environ)
    env["MANDELBULBER_BIN"] = binary
    env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "mandelbulber_mcp.server"],
        env=env,
    )
    try:
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                guidance = (await session.call_tool("get_guidance", {})).content[0].text
                for needle in (
                    "frame_scene",
                    "get_contact_sheet",
                    "progress token",
                    "older session",
                    "boolean_add",
                    "frames_per_keyframe",
                    "set_flight_path",
                    "iteration fog",
                ):
                    if needle not in guidance:
                        raise SystemExit(f"guidance missing {needle!r}")
                print("guidance needles ok")

                as_json(
                    await session.call_tool(
                        "set_parameters",
                        {"parameters": {"power": 4}, "fractal_slot": 1},
                    )
                )
                once = as_json(await session.call_tool("undo", {}))
                power = once["summary"]["formulas"][0]["power"]
                if abs(float(power) - 9.0) > 1e-6:
                    raise SystemExit(f"undo returned to power {power}, expected the startup default 9")
                twice = await session.call_tool("undo", {})
                text = twice.content[0].text
                if "Nothing to undo" not in text:
                    raise SystemExit(f"second undo walked past the baseline: {text[:400]}")
                print("undo floor ok", power)

                stopped = raw_call(
                    "render_preview",
                    {
                        "quality": "draft",
                        "long_side": 64,
                        "stop_before_execute": True,
                    },
                )
                status = (stopped.get("result") or {}).get("status")
                error = str(stopped.get("error") or "")
                if stopped.get("ok") or status != "stopped" or "stopped" not in error.lower():
                    raise SystemExit(f"stop-before-execute did not stop the job: {stopped}")
                print("stop-before-execute", status)

                as_json(await session.call_tool("set_formula", {"formula": "mandelbox", "slot": 1}))
                as_json(
                    await session.call_tool(
                        "set_parameters",
                        {"parameters": {"mandelbox_scale": 2}, "fractal_slot": 1},
                    )
                )
                framed = as_json(await session.call_tool("frame_scene", {}))
                print(
                    "framed",
                    "distance",
                    round(float(framed["distance"]), 3),
                    "radius",
                    round(float(framed["measured_radius"]), 3),
                )
                preview = await session.call_tool(
                    "render_preview",
                    {"quality": "draft", "long_side": 220},
                    read_timeout_seconds=300,
                )
                artifact_dir.mkdir(parents=True, exist_ok=True)
                framed_path = artifact_dir / "preview-framed-mandelbox.png"
                framed_path.write_bytes(first_image(preview))
                lit(framed_path)

                as_json(await session.call_tool("add_keyframe", {"frames_per_keyframe": 2}))
                camera = framed["camera"]
                nudged = [camera[0] + 0.4, camera[1], camera[2] + 0.2]
                as_json(await session.call_tool("set_camera", {"camera": nudged}))
                as_json(await session.call_tool("add_keyframe", {"frames_per_keyframe": 2}))
                directory = Path("/tmp/mandelbulber-mcp-sheet")
                if directory.exists():
                    shutil.rmtree(directory)
                started = as_json(
                    await session.call_tool(
                        "render_animation",
                        {
                            "mode": "keyframe",
                            "start": 0,
                            "end": 3,
                            "directory": str(directory),
                            "quality": "draft",
                            "long_side": 80,
                            "max_frames": 8,
                            "contact_sheet": True,
                            "frame_edge": 64,
                            "columns": 3,
                        },
                    )
                )
                job_id = started["id"]
                deadline = time.time() + 180
                final = started
                while time.time() < deadline:
                    final = as_json(await session.call_tool("render_status", {"id": job_id}))
                    if final.get("done"):
                        break
                    time.sleep(0.3)
                if final.get("status") != "finished":
                    raise SystemExit(f"animation did not finish: {final}")
                sheet_on_disk = directory / "contact_sheet.png"
                if not sheet_on_disk.is_file():
                    raise SystemExit(f"contact_sheet.png missing in {directory}")
                frames = sorted(directory.glob("frame_*.png"))
                if len(frames) < 3:
                    raise SystemExit(f"expected 3 frames, got {frames}")
                lit(frames[0])
                shutil.copy(frames[0], artifact_dir / "sheet-frame.png")

                sheet = await session.call_tool(
                    "get_contact_sheet",
                    {"id": job_id, "frame_edge": 64, "columns": 3},
                )
                info = as_json(sheet)
                png = first_image(sheet)
                sheet_path = artifact_dir / "contact-sheet.png"
                sheet_path.write_bytes(png)
                if int(info["width"]) <= 80:
                    raise SystemExit(f"contact sheet is not wider than one frame: {info}")
                if int(info["frames"]) < 3:
                    raise SystemExit(f"contact sheet dropped frames: {info}")
                lit(sheet_path)
                print("contact sheet", info["width"], info["height"], "frames", info["frames"])

                notes: list[tuple[float, float | None, str | None]] = []

                async def on_progress(progress: float, total: float | None, message: str | None) -> None:
                    notes.append((progress, total, message))

                progressed = await session.call_tool(
                    "render_still",
                    {"quality": "draft", "long_side": 96},
                    progress_callback=on_progress,
                    read_timeout_seconds=300,
                )
                body = as_json(progressed)
                if body.get("status") != "finished":
                    raise SystemExit(f"progress still did not wait for completion: {body}")
                if not notes:
                    raise SystemExit("render_still with a progress token sent no notifications")
                print("progress", len(notes), "last", notes[-1])
                print("e2e polish ok")
    finally:
        for path in dummies:
            path.unlink(missing_ok=True)
        # Session file is removed by a clean adapter exit. If a test left a
        # dead pid behind, the next script would refuse to start.
        try:
            meta = json.loads(session_file().read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, SystemExit):
            meta = None
        if isinstance(meta, dict):
            print("session still present for pid", meta.get("pid"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--artifacts", required=True)
    args = parser.parse_args()
    asyncio.run(run(Path(args.artifacts), args.binary))


if __name__ == "__main__":
    main()
