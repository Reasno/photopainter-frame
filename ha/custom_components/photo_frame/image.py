"""Picture currently shown on the frame, including the status bar."""

from __future__ import annotations

from homeassistant.components.image import ImageEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import PhotoFrameEntity


async def async_setup_platform(hass: HomeAssistant, config, async_add_entities: AddEntitiesCallback, discovery_info=None) -> None:
    if discovery_info is None:
        return
    async_add_entities([CurrentPhotoImage(hass, hass.data[DOMAIN])])


class CurrentPhotoImage(PhotoFrameEntity, ImageEntity):
    _attr_name = "Photo Frame Current Photo"
    _attr_content_type = "image/png"

    def __init__(self, hass: HomeAssistant, library) -> None:
        PhotoFrameEntity.__init__(self, library, "current_photo")
        ImageEntity.__init__(self, hass)
        self._attr_image_last_updated = library.state.updated

    @callback
    def _handle_update(self) -> None:
        self._attr_image_last_updated = self._library.state.updated
        self.async_write_ha_state()

    async def async_image(self) -> bytes | None:
        return self._library.state.image
