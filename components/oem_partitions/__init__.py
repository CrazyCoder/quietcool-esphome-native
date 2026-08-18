"""Installs the OEM-compatible flash layout as the build's partition table.

`esp32: partitions:` resolves its path against the user's configuration
directory. That is fine when the YAML sits next to its files, but an adopted
Device Builder install consumes this configuration as a remote package, so a
config-relative path points at the user's `/config/esphome`, where the CSV does
not exist. Registering the file from a component instead resolves it relative to
the component's own directory, which is correct for both a local checkout and a
`type: git` external-components clone.

`esp32.copy_files()` emits its generated table only when `partitions.csv` is not
already registered, so this replaces the default layout rather than adding to it.
"""

from pathlib import Path

from esphome.components.esp32 import add_extra_build_file
import esphome.config_validation as cv

CODEOWNERS = ["@CrazyCoder"]
DEPENDENCIES = ["esp32"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    add_extra_build_file("partitions.csv", Path(__file__).parent / "partitions.csv")
