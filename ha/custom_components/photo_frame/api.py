"""HTTP endpoint polled by the photo frame."""

from __future__ import annotations

import hmac
import logging
import time

from aiohttp import web

from homeassistant.components.http import KEY_HASS, HomeAssistantView
from homeassistant.helpers.dispatcher import async_dispatcher_send

from .const import SIGNAL_UPDATED
from .library import PhotoLibrary

_LOGGER = logging.getLogger(__name__)


class PhotoFrameView(HomeAssistantView):
    """GET /api/photo_frame/next?token=…&direction=next returns a 480x800 24-bit BMP.

    Direction is next (default) or previous; battery and charging describe the frame.
    The X-Frame-Time response header carries the current Unix time so the frame can set its clock.
    """

    url = "/api/photo_frame/next"
    name = "api:photo_frame:next"
    requires_auth = False

    def __init__(self, library: PhotoLibrary, token: str) -> None:
        self._library = library
        self._token = token

    async def get(self, request: web.Request) -> web.Response:
        supplied = request.query.get("token", "")
        if not hmac.compare_digest(supplied, self._token):
            return web.Response(status=401)
        direction = request.query.get("direction", "next")
        if direction not in ("next", "previous"):
            return web.Response(status=400, text="direction must be next or previous")
        battery = _parse_battery(request.query.get("battery"))
        charging = request.query.get("charging") == "1"
        hass = request.app[KEY_HASS]
        body, photo = await hass.async_add_executor_job(
            self._library.render, battery, charging, -1 if direction == "previous" else 1
        )
        async_dispatcher_send(hass, SIGNAL_UPDATED)
        _LOGGER.info("Frame picture %s (direction=%s battery=%s charging=%s)", photo.file, direction, battery, charging)
        return web.Response(
            body=body,
            content_type="image/bmp",
            headers={"X-Frame-Time": str(int(time.time()))},
        )


def _parse_battery(value: str | None) -> int | None:
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return number if 0 <= number <= 100 else None
