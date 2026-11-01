"""Serve random PhotoPainter pictures with a status bar rendered on Home Assistant."""

from __future__ import annotations

import logging
from pathlib import Path

import voluptuous as vol

from homeassistant.const import CONF_TOKEN, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.discovery import async_load_platform
from homeassistant.helpers.typing import ConfigType

from .api import PhotoFrameView
from .const import DOMAIN
from .library import PhotoLibrary

_LOGGER = logging.getLogger(__name__)

CONF_PHOTO_DIR = "photo_dir"
CONF_FONT = "font"
PLATFORMS = (Platform.SENSOR, Platform.BINARY_SENSOR, Platform.IMAGE)

CONFIG_SCHEMA = vol.Schema(
    {
        DOMAIN: vol.Schema(
            {
                vol.Required(CONF_TOKEN): cv.string,
                vol.Optional(CONF_PHOTO_DIR, default="/media/tfcard/photopainter"): cv.string,
                vol.Optional(CONF_FONT, default=""): cv.string,
            }
        )
    },
    extra=vol.ALLOW_EXTRA,
)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    conf = config.get(DOMAIN)
    if conf is None:
        return True
    font = conf[CONF_FONT] or str(Path(__file__).parent / "fonts" / "fusion-pixel-12px-proportional-zh_hans.ttf")
    library = PhotoLibrary(Path(conf[CONF_PHOTO_DIR]), Path(font), Path(hass.config.config_dir) / ".storage")
    await hass.async_add_executor_job(library.load)
    hass.data[DOMAIN] = library
    hass.http.register_view(PhotoFrameView(library, conf[CONF_TOKEN]))
    for platform in PLATFORMS:
        hass.async_create_task(async_load_platform(hass, platform, DOMAIN, {}, config))
    _LOGGER.info("Photo frame ready with %d pictures", library.count)
    return True
