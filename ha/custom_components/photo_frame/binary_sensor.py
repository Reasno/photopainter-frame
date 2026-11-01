"""Charging state reported by the frame."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import PhotoFrameEntity


async def async_setup_platform(hass: HomeAssistant, config, async_add_entities: AddEntitiesCallback, discovery_info=None) -> None:
    if discovery_info is None:
        return
    async_add_entities([ChargingSensor(hass.data[DOMAIN])])


class ChargingSensor(PhotoFrameEntity, BinarySensorEntity):
    _attr_name = "Photo Frame Charging"
    _attr_device_class = BinarySensorDeviceClass.BATTERY_CHARGING

    def __init__(self, library) -> None:
        super().__init__(library, "charging")

    @property
    def is_on(self):
        return self._library.state.charging if self._library.state.updated else None
