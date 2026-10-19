#!/usr/bin/env python3
# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Objects tree, looks, a short keyframe animation, and a flight sketch."""

from __future__ import annotations

import argparse
import asyncio
import os
import shutil
import sys
import time
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from e2e_test import as_json, first_image


def lit(path: Path) -> None:
    from PIL import Image

    image = Image.open(path)
    if max(channel[1] for channel in image.getextrema()) == 0:
        raise SystemExit(f"{path.name} is black {image.size}")
    print(path.name, image.size, image.getextrema(), path.stat().st_size)


async def run(artifact_dir: Path, binary: str) -> None:
    env = dict(os.environ)
    env["MANDELBULBER_BIN"] = binary
    env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "mandelbulber_mcp.server"],
        env=env,
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            guidance = (await session.call_tool("get_guidance", {})).content[0].text
            for needle in ("boolean_add", "frames_per_keyframe", "set_flight_path", "iteration fog"):
                if needle not in guidance:
                    raise SystemExit(f"guidance missing {needle!r}")
            print("guidance recipes ok")

            detail = as_json(await session.call_tool("describe_parameter", {"name": "detail_level"}))
            description = (detail.get("description") or "") + (detail.get("ui_tooltip") or "")
            if "detail" not in description.lower():
                raise SystemExit(f"detail_level has no dock tooltip: {detail}")
            print("detail_level tooltip", len(description), "chars")

            tree = as_json(await session.call_tool("get_objects_tree", {}))
            if not tree["nodes"]:
                raise SystemExit("fresh tree is empty")
            for node in tree["nodes"]:
                if node["type"] == "fractal":
                    as_json(await session.call_tool("delete_object", {"id": node["id"]}))
            hybrid = as_json(await session.call_tool("add_object", {"type": "hybrid"}))
            hybrid_id = hybrid["added"]
            bulb = as_json(
                await session.call_tool(
                    "add_object",
                    {
                        "type": "fractal",
                        "formula": "mandelbulb",
                        "parent_id": hybrid_id,
                        "formula_iterations": 1,
                    },
                )
            )
            box = as_json(
                await session.call_tool(
                    "add_object",
                    {
                        "type": "fractal",
                        "formula": "mandelbox",
                        "parent_id": hybrid_id,
                        "formula_iterations": 1,
                    },
                )
            )
            slot = box["object_id"]
            as_json(
                await session.call_tool(
                    "set_parameters",
                    {"parameters": {"mandelbox_scale": -1.5}, "fractal_slot": slot},
                )
            )
            as_json(
                await session.call_tool(
                    "set_parameters",
                    {"parameters": {"power": 8}, "fractal_slot": bulb["object_id"]},
                )
            )
            print("hybrid", hybrid_id, "mandelbox slot", slot, "bulb node", bulb["added"])

            preview = await session.call_tool(
                "render_preview",
                {"quality": "draft", "long_side": 180},
                read_timeout_seconds=300,
            )
            artifact_dir.mkdir(parents=True, exist_ok=True)
            hybrid_path = artifact_dir / "preview-hybrid.png"
            hybrid_path.write_bytes(first_image(preview))
            lit(hybrid_path)

            as_json(
                await session.call_tool(
                    "set_material",
                    {"id": 1, "surface_color": "#c45a28", "name": "clay"},
                )
            )
            as_json(await session.call_tool("set_light", {"id": 1, "intensity": 1.4, "color": "#fff2e0"}))
            as_json(
                await session.call_tool(
                    "set_parameters",
                    {"parameters": {"iteration_fog_enable": True, "iteration_fog_color_1": [80, 140, 255]}},
                )
            )
            look = await session.call_tool(
                "render_preview",
                {"quality": "draft", "long_side": 180},
                read_timeout_seconds=300,
            )
            look_path = artifact_dir / "preview-look.png"
            look_path.write_bytes(first_image(look))
            lit(look_path)

            as_json(await session.call_tool("set_camera", {"camera": [3.2, -6.2, 2.1], "fov": 48}))
            first = as_json(await session.call_tool("add_keyframe", {"frames_per_keyframe": 2}))
            if first["keyframe_count"] != 1:
                raise SystemExit(f"expected 1 key, got {first}")
            as_json(await session.call_tool("set_camera", {"camera": [2.4, -5.2, 2.6]}))
            second = as_json(await session.call_tool("add_keyframe", {"frames_per_keyframe": 2}))
            total = second["interpolated_frames"]
            if total < 3:
                raise SystemExit(f"expected at least 3 interpolated frames, got {second}")
            end = min(3, total)
            print("keyframes", second["keyframe_count"], "interpolated", total, "rendering", end)

            job = as_json(
                await session.call_tool(
                    "render_animation",
                    {
                        "mode": "keyframe",
                        "start": 0,
                        "end": end,
                        "quality": "draft",
                        "width": 80,
                        "height": 60,
                        "max_frames": 8,
                    },
                )
            )
            job_id = job["id"]
            status = job
            deadline = time.time() + 180
            while not status.get("done") and time.time() < deadline:
                await asyncio.sleep(0.4)
                status = as_json(await session.call_tool("render_status", {"id": job_id}))
            print("animation", status.get("status"), status.get("frames"))
            if status.get("status") != "finished":
                raise SystemExit(f"animation did not finish: {status}")
            frames = [Path(path) for path in status["frames"]]
            if len(frames) < end:
                raise SystemExit(f"expected {end} frames, got {frames}")
            for frame in frames:
                if not frame.is_file():
                    raise SystemExit(f"missing {frame}")
                lit(frame)
                shutil.copy(frame, artifact_dir / frame.name)

            viewed = await session.call_tool("get_image", {"max_edge": 40})
            view_path = artifact_dir / "animation-view.png"
            view_path.write_bytes(first_image(viewed))
            view = as_json(viewed)
            if view.get("width", 99) > 40 and view.get("height", 99) > 40:
                raise SystemExit(f"max_edge did not downscale: {view}")
            print("downscaled", view.get("width"), view.get("height"))

            flight = as_json(
                await session.call_tool(
                    "set_flight_path",
                    {"kind": "orbit", "count": 4, "radius": 7, "height": 2, "target": [0, 0, 0]},
                )
            )
            if flight["flight_frames"] != 4:
                raise SystemExit(f"orbit did not write 4 frames: {flight}")
            orbit = as_json(
                await session.call_tool(
                    "render_animation",
                    {
                        "mode": "flight",
                        "start": 0,
                        "end": 2,
                        "quality": "draft",
                        "width": 80,
                        "height": 60,
                        "max_frames": 4,
                        "directory": "/tmp/mandelbulber-mcp-orbit",
                    },
                )
            )
            deadline = time.time() + 180
            status = orbit
            while not status.get("done") and time.time() < deadline:
                await asyncio.sleep(0.4)
                status = as_json(await session.call_tool("render_status", {"id": orbit["id"]}))
            if status.get("status") != "finished" or len(status.get("frames") or []) < 2:
                raise SystemExit(f"orbit render failed: {status}")
            orbit_frame = Path(status["frames"][0])
            lit(orbit_frame)
            shutil.copy(orbit_frame, artifact_dir / "orbit-frame.png")
            print("slice2 ok")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--artifacts", required=True)
    args = parser.parse_args()
    asyncio.run(run(Path(args.artifacts), args.binary))


if __name__ == "__main__":
    main()
