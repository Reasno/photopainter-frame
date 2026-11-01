"""Battery, last update and current photo sensors."""

from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import PERCENTAGE
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import PhotoFrameEntity


async def async_setup_platform(hass: HomeAssistant, config, async_add_entities: AddEntitiesCallback, discovery_info=None) -> None:
    if discovery_info is None:
        return
    library = hass.data[DOMAIN]
    async_add_entities([BatterySensor(library), LastUpdateSensor(library), PhotoSensor(library)])


class BatterySensor(PhotoFrameEntity, SensorEntity):
    _attr_name = "Photo Frame Battery"
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, library) -> None:
        super().__init__(library, "battery")

    @property
    def native_value(self):
        return self._library.state.battery


class LastUpdateSensor(PhotoFrameEntity, SensorEntity):
    _attr_name = "Photo Frame Last Update"
    _attr_device_class = SensorDeviceClass.TIMESTAMP

    def __init__(self, library) -> None:
        super().__init__(library, "last_update")

    @property
    def native_value(self):
        return self._library.state.updated


class PhotoSensor(PhotoFrameEntity, SensorEntity):
    _attr_name = "Photo Frame Photo"
    _attr_icon = "mdi:image-frame"

    def __init__(self, library) -> None:
        super().__init__(library, "photo")

    @property
    def native_value(self):
        photo = self._library.state.photo
        return photo.file if photo else None

    @property
    def extra_state_attributes(self):
        photo = self._library.state.photo
        if not photo:
            return None
        return {
            "taken": photo.taken.isoformat() if photo.taken else None,
            "place": photo.place or None,
        }
