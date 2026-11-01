"""Shared base for photo_frame entities."""

from __future__ import annotations

from homeassistant.core import callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import Entity

from .const import DOMAIN, SIGNAL_UPDATED
from .library import PhotoLibrary


class PhotoFrameEntity(Entity):
    _attr_should_poll = False

    def __init__(self, library: PhotoLibrary, key: str) -> None:
        self._library = library
        self._attr_unique_id = f"{DOMAIN}_{key}"

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(async_dispatcher_connect(self.hass, SIGNAL_UPDATED, self._handle_update))

    @callback
    def _handle_update(self) -> None:
        self.async_write_ha_state()
