"""The car page's server: the rendered car body and the live record, from the QNX guest or from the demo car.

FE_GUEST=1 python -m uvicorn app.server:app --port 8092   live: fe-bridge in the guest at 127.0.0.1:2242
python -m uvicorn app.server:app --port 8092              demo: no QNX, the gate core on this host (app/core.py)
"""

from __future__ import annotations

import asyncio
import json
import os
import threading
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any, Literal

from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import __version__
from .core import DemoCar, load_cases

WEB_DIR = Path(os.environ.get("APP_WEB_DIR") or Path(__file__).resolve().parents[1] / "web")
LIVE = os.environ.get("FE_GUEST") == "1"

app = FastAPI(title="Four Eyes", version=__version__)


class Hub:
    """Fans events out to every open page."""

    def __init__(self) -> None:
        self.queues: list[asyncio.Queue[str]] = []
        self.loop: asyncio.AbstractEventLoop | None = None
        self.mu = threading.Lock()

    def publish(self, kind: str, payload: Any) -> None:
        msg = f"event: {kind}\ndata: {json.dumps(payload, ensure_ascii=True)}\n\n"
        with self.mu:
            targets = list(self.queues)
        for q in targets:
            if self.loop is not None:
                self.loop.call_soon_threadsafe(q.put_nowait, msg)


hub = Hub()
CASES = {c["id"]: c for c in load_cases()}
# Live mode takes every verdict from the guest's gate, so it never builds the host's demo car (or compiles its core).
car: DemoCar | None = None if LIVE else DemoCar(emit=hub.publish)
bridge: Any = None
if LIVE:  # pragma: no cover - needs the guest
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    import guest  # type: ignore[import-not-found]

    from .bridge import GuestBridge

    bridge = GuestBridge([*guest.ssh_args(), "root@127.0.0.1"], on_event=hub.publish)


class CarRequest(BaseModel):
    case: str | None = Field(default=None, max_length=16)
    origin: Literal["driver", "message"] = "driver"
    text: str = Field(default="", max_length=1000)


class DriveIn(BaseModel):
    speed_kmh: float = Field(ge=0, le=250)
    gear: Literal["P", "R", "N", "D"]
    parking_brake: bool


@app.get("/api/health")
def health() -> dict[str, object]:
    return {"status": "ok", "version": __version__, "mode": "live" if LIVE else "demo"}


@app.get("/api/cases")
def cases() -> list[dict[str, Any]]:
    return [{k: c[k] for k in ("id", "kind", "origin", "text", "vehicle")} for c in CASES.values()]


def demo_car() -> DemoCar:
    assert car is not None, "the demo car exists only in demo mode"
    return car


@app.get("/api/record")
def record() -> list[dict[str, Any]]:
    return list(bridge.record if LIVE else demo_car().record)


@app.get("/api/state")
def state() -> dict[str, Any]:
    return dict(bridge.state) if LIVE else demo_car().state()


@app.post("/api/request")
def request(body: CarRequest) -> dict[str, Any]:
    if LIVE:  # pragma: no cover - needs the guest
        c = CASES.get(body.case or "")
        if c:
            v = c["vehicle"]
            for cmd in (
                "reset",
                f"brake {int(v['parking_brake'])}",
                f"gear {v['gear']}",
                f"speed {round(v['speed_kmh'])} now",
            ):
                bridge.drive(cmd)
            return bridge.submit(c["origin"], c["text"])  # type: ignore[no-any-return]
        return bridge.submit(body.origin, body.text)  # type: ignore[no-any-return]
    if not body.case or body.case not in CASES:
        raise HTTPException(422, "the demo has no model: choose one of the preset requests (/api/cases)")
    return {"entries": demo_car().request(body.case)}


@app.post("/api/drive")
def drive(body: DriveIn) -> dict[str, Any]:
    if LIVE:  # pragma: no cover - needs the guest
        bridge.drive(f"brake {int(body.parking_brake)}")
        bridge.drive(f"gear {body.gear}")
        bridge.drive(f"speed {round(body.speed_kmh)}")
        return {"ok": True}
    return demo_car().drive(body.speed_kmh, body.gear, body.parking_brake)


@app.get("/api/events")
async def events() -> StreamingResponse:
    hub.loop = asyncio.get_running_loop()
    q: asyncio.Queue[str] = asyncio.Queue()
    with hub.mu:
        hub.queues.append(q)

    async def stream() -> AsyncIterator[str]:
        try:
            yield f"event: state\ndata: {json.dumps(state())}\n\n"
            while True:
                try:
                    yield await asyncio.wait_for(q.get(), timeout=15)
                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
        finally:
            with hub.mu:
                hub.queues.remove(q)

    return StreamingResponse(stream(), media_type="text/event-stream")


# Mounted last, so /api/* wins.
if WEB_DIR.is_dir():
    app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")
