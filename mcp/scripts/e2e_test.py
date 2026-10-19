#!/usr/bin/env python3
# Copyright (C) 2026 Mandelbulber Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive the real MCP server, then a raw socket, and save preview PNGs."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import socket
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def session_file() -> Path:
    home = Path.home()
    for path in (
        home / ".mandelbulber" / "mcp-session.json",
        home / "mandelbulber" / "mcp-session.json",
    ):
        if path.is_file():
            return path
    raise SystemExit("no session file")


def raw_call(method: str, params: dict) -> dict:
    session = json.loads(session_file().read_text(encoding="utf-8"))
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(60)
    sock.connect(session["socket"])
    sock.sendall(json.dumps({"id": 1, "method": method, "params": params}).encode() + b"\n")
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    sock.close()
    return json.loads(buf.split(b"\n", 1)[0].decode())


def as_json(result) -> dict:
    if getattr(result, "is_error", False):
        text = " ".join(getattr(block, "text", "") for block in result.content)
        raise RuntimeError(text or "tool failed")
    for block in result.content:
        if getattr(block, "type", None) == "text":
            parsed = json.loads(block.text)
            if isinstance(parsed, str):
                parsed = json.loads(parsed)
            if isinstance(parsed, dict):
                return parsed
    raise RuntimeError(f"no JSON in tool result: {result}")


def first_image(result) -> bytes:
    for block in result.content:
        if getattr(block, "type", None) == "image":
            import base64

            return base64.b64decode(block.data)
    raise RuntimeError("tool returned no image")


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
            guidance = await session.call_tool("get_guidance", {})
            text = guidance.content[0].text
            if "Mandelbulb" not in text or "detail_level" not in text:
                raise SystemExit("get_guidance is missing the starter recipes")
            print("guidance ok", len(text), "chars")

            state = as_json(await session.call_tool("get_state", {"mode": "summary"}))
            print("state", state.get("summary", state).get("formulas"))

            described = as_json(
                await session.call_tool("describe_formula", {"name": "mandelbulb", "verbose": True})
            )
            brief = as_json(await session.call_tool("describe_formula", {"name": "mandelbulb"}))
            if not brief.get("start_here"):
                raise SystemExit(f"describe_formula start_here missing: {list(brief)[:12]}")
            summary = described.get("summary", "")
            if "Mandelbulb" not in summary and "mandelbulb" not in summary.lower():
                raise SystemExit(f"describe_formula summary was: {summary[:200]!r}")
            power = next(item for item in described["parameters"] if item["name"] == "power")
            if "Power" not in (power.get("label") or ""):
                raise SystemExit(f"power label missing: {power}")
            print("formula", described["internal_name"], "params", len(described["parameters"]))

            listed = as_json(await session.call_tool("list_parameters", {"area": "quality", "query": "detail"}))
            names = [item["name"] for item in listed["parameters"]]
            if "detail_level" not in names:
                raise SystemExit(f"detail_level not in {names}")

            as_json(await session.call_tool("set_formula", {"formula": "Mandelbulb", "slot": 1}))
            set_power = as_json(
                await session.call_tool(
                    "set_parameters",
                    {"parameters": {"power": 8, "fov": 45}, "fractal_slot": 1},
                )
            )
            print("set", set_power["applied"])

            comma = raw_call("set_parameters", {"parameters": {"power": "8,5"}, "fractal_slot": 1})
            if not comma.get("ok"):
                raise SystemExit(f"decimal comma was rejected: {comma}")
            applied = comma["result"]["applied"]
            value = applied[0]["value"]
            if abs(float(value) - 8.5) > 1e-9:
                raise SystemExit(f"decimal comma parsed as {value}, expected 8.5")
            print("decimal comma ->", value)

            both = raw_call("set_parameters", {"parameters": {"power": "8,5.0"}, "fractal_slot": 1})
            if both.get("ok"):
                raise SystemExit(f"mixed separators were accepted: {both}")
            print("mixed separators rejected")

            undone = as_json(await session.call_tool("undo", {}))
            formulas = undone["summary"]["formulas"]
            power_now = formulas[0]["power"]
            if abs(float(power_now) - 8.0) > 1e-6:
                raise SystemExit(f"undo restored {power_now}, expected 8 (the edit before the comma)")
            print("undo restored", power_now)

            preview = await session.call_tool(
                "render_preview",
                {"quality": "draft", "long_side": 220},
                read_timeout_seconds=300,
            )
            png = first_image(preview)
            artifact_dir.mkdir(parents=True, exist_ok=True)
            bulb = artifact_dir / "preview-mandelbulb.png"
            bulb.write_bytes(png)
            from PIL import Image

            bulb_image = Image.open(bulb)
            if max(channel[1] for channel in bulb_image.getextrema()) == 0:
                raise SystemExit(f"mandelbulb preview is black {bulb_image.size}")
            print("preview", bulb, bulb_image.size, bulb_image.getextrema())

            as_json(await session.call_tool("set_formula", {"formula": "mandelbox", "slot": 1}))
            # Stored default scale 2 is an empty frame from the default camera.
            # The classic Amazing Box is a negative scale.
            as_json(
                await session.call_tool(
                    "set_parameters",
                    {"parameters": {"mandelbox_scale": -1.5}, "fractal_slot": 1},
                )
            )
            box = await session.call_tool(
                "render_preview",
                {"quality": "draft", "long_side": 220},
                read_timeout_seconds=300,
            )
            box_path = artifact_dir / "preview-mandelbox.png"
            box_path.write_bytes(first_image(box))
            box_image = Image.open(box_path)
            if max(channel[1] for channel in box_image.getextrema()) == 0:
                raise SystemExit(f"mandelbox preview is black {box_image.size}")
            print("preview", box_path, box_image.size, box_image.getextrema())

            examples = as_json(await session.call_tool("list_examples", {"query": "mandelbulb001", "limit": 5}))
            if not examples["examples"]:
                raise SystemExit("list_examples found nothing")
            loaded = as_json(await session.call_tool("load_example", {"name": "mandelbulb001.fract"}))
            print("loaded", loaded["loaded"])

            target = Path("/tmp/mandelbulber-mcp-save-test.fract")
            saved = as_json(await session.call_tool("save_settings", {"path": str(target)}))
            if not target.is_file():
                raise SystemExit("save_settings did not write the file")
            print("saved", saved["saved"])
            refused = await session.call_tool("save_settings", {"path": str(target), "overwrite": False})
            refused_text = refused.content[0].text
            if "overwrite" not in refused_text.lower() and "loaded" not in refused_text.lower():
                raise SystemExit(f"saving over the loaded file should have been refused, got {refused_text}")
            print("overwrite refused")

            print("e2e ok")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--artifacts", required=True)
    args = parser.parse_args()
    asyncio.run(run(Path(args.artifacts), args.binary))


if __name__ == "__main__":
    main()
