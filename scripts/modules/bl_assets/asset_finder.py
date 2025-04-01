# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import logging
import os
import shutil
import urllib.parse
from pathlib import Path

import bpy
import pydantic

from . import blender_asset_library_openapi as api_models

log = logging.getLogger(__name__)


class BlendfileInfo(pydantic.BaseModel):
    # Asset info that's blendfile-specific:
    archive_url: str
    archive_hash: str
    archive_size_in_bytes: int

    # Last access/modification time:
    st_atime: float
    st_mtime: float


def list_assets(blendfile: Path, asset_library_root: Path) -> list[api_models.Asset]:
    # Start by erasing everything from memory.
    bpy.ops.wm.read_homefile(use_factory_startup=True, use_empty=True, load_ui=False)

    # Tell Blender to only load asset data-blocks.
    with bpy.data.libraries.load(str(blendfile), assets_only=True) as (
        data_from,
        data_to,
    ):
        for attr in dir(data_to):
            setattr(data_to, attr, getattr(data_from, attr))

    # Get the last modification timestamp of the blend file, to compare against
    # the thumbnails.
    blendfile_info = _blendfile_info(blendfile, asset_library_root)
    thumbnail_dir = blendfile.with_name(blendfile.stem + "_thumbnails")

    thumbnail_timestamper = thumbnail_dir / ".last_modified"
    if thumbnail_timestamper.exists():
        thumb_mtime = thumbnail_timestamper.stat().st_mtime
        blend_mtime = blendfile_info.st_mtime
        should_write_thumbnails = thumb_mtime != blend_mtime
    else:
        should_write_thumbnails = True

    if should_write_thumbnails:
        # Remove the entire thumbnail tree, so that thumbnails of deleted assets
        # are also deleted. All thumbnails are going to be re-written anyway.
        log.info("thumbnails will be (re-)exported to %s", thumbnail_dir)
        assert thumbnail_dir
        if thumbnail_dir.root == thumbnail_dir:
            raise RuntimeError(f"Refusing to remove a root directory: {thumbnail_dir}")
        if thumbnail_dir.exists():
            shutil.rmtree(thumbnail_dir)

    # Collect the asset data.
    assets: list[api_models.Asset] = []
    for attr in dir(data_to):
        datablocks = getattr(data_from, attr)
        datablocks_assets = _find_assets(
            datablocks, thumbnail_dir, should_write_thumbnails
        )
        assets.extend(datablocks_assets)

    # After processing is done, set the thumbnail dir mtime to that of the
    # blendfile. By tracking the mtime of the directory itself, not every
    # individual thumbnail needs to be time-checked.
    thumbnail_timestamper.touch(exist_ok=True)
    os.utime(thumbnail_timestamper, (blendfile_info.st_atime, blendfile_info.st_mtime))

    return assets


def _find_assets(
    datablocks: bpy.types.BlendData,
    thumbnail_dir: Path,
    should_write_thumbnails: bool,
) -> list[api_models.Asset]:

    assets = []
    for datablock in datablocks:
        asset_data: bpy.types.AssetData = datablock.asset_data
        if not asset_data:
            continue

        thumbnail_path = _thumbnail_path(datablock, thumbnail_dir)

        if thumbnail_path and should_write_thumbnails:
            _save_thumbnail(datablock, thumbnail_path)

        asset = api_models.Asset(
            name=datablock.name,
            id_type=datablock.id_type.lower(),
            blender_version_min=".".join(map(str, bpy.data.version)),
            thumbnail_url=str(thumbnail_path or ""),  # To be turned into a URL later.
            archive_url="",  # TODO
            archive_hash="",  # TODO
            archive_size_in_bytes=0,  # TODO
            meta=api_models.AssetMetadata(
                catalog=asset_data.catalog_simple_name,
                tags=[tag.name for tag in asset_data.tags] or None,
                author=asset_data.author,
                description=asset_data.description,
                license=asset_data.license,
                copyright=asset_data.copyright,
            ),
        )
        assets.append(asset)
    return assets


def _save_thumbnail(datablock: bpy.types.ID, thumbnail_path: Path) -> None:
    """Save the internal preview thumbnail as a WebP image."""

    # Get the preview image size.
    width: int = datablock.preview.image_size[0]
    height: int = datablock.preview.image_size[1]

    if not (width > 0 and height > 0):
        return

    thumbnail_path.parent.mkdir(exist_ok=True, parents=True)

    log.debug("Writing thumbnail: %s", thumbnail_path)
    try:
        # Create a new image in Blender to store the preview.
        image: bpy.types.Image = bpy.data.images.new(
            thumbnail_path.stem, width, height, alpha=True
        )

        # Assign the pixel data from the preview to the new image.
        # image.pixels = [p for p in datablock.preview.image_pixels_float]
        image.pixels[:] = datablock.preview.image_pixels_float

        # Save the image to disk.
        image.file_format = "WEBP"
        image.save(filepath=str(thumbnail_path), quality=80)

        # Remove the image from Blender data after saving to free memory.
        bpy.data.images.remove(image)
    except Exception as e:
        print(f"Failed to save thumbnail for {datablock.name}: {e}")


def _thumbnail_path(datablock: bpy.types.ID, thumbnail_dir: Path) -> Path | None:
    """Return the path for this datablock's thumbnail, or None if it has none."""

    if not datablock.preview:
        return None

    # Not all datablock names are valid in a file path, so better use URI encoding here.
    # safe="" avoids the default safe="/", as we also don't want slashes in the filename.
    datablock_safe = urllib.parse.quote(datablock.name, safe="")
    thumbnail_path: Path = (
        thumbnail_dir / datablock.id_type.title() / f"{datablock_safe}.webp"
    )

    return thumbnail_path


def _blendfile_info(filepath: Path, asset_library_root: Path) -> BlendfileInfo:
    stat = filepath.stat()

    return BlendfileInfo(
        archive_url=filepath.relative_to(asset_library_root).as_posix(),
        archive_hash=_sha256_file(filepath),
        archive_size_in_bytes=stat.st_size,
        st_atime=stat.st_atime,
        st_mtime=stat.st_mtime,
    )


def _sha256_file(filepath: Path) -> str:
    """Computes and returns the SHA256 hash of the file."""
    sha256_hash = hashlib.sha256()

    file_size_bytes = 0
    with open(filepath, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            file_size_bytes += len(byte_block)
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()
