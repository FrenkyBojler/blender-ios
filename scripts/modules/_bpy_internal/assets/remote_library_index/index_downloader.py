# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import logging
import urllib.parse
from pathlib import Path

import pydantic

from _bpy_internal.http.downloader import CachingDownloader, BackgroundDownloader
from . import blender_asset_library_openapi as api_models
from . import index_common

logger = logging.getLogger(__name__)


class CLIArguments(pydantic.BaseModel):
    """Parsed commandline arguments."""

    url: str


def cli_main(arguments_raw: argparse.Namespace) -> None:
    """Generate the index for the passed-on-the-CLI asset library path."""

    # Parse CLI arguments.
    arguments = _parse_cli_args(arguments_raw)

    base_url = arguments.url
    base_path = Path(".").resolve() / "_asset_download_location"  # TODO: be sensible.

    downloader = CachingDownloader(
        metadata_cache_location=base_path / "_local-meta-cache",
        chunk_size=10,
    )

    bg_downloader = BackgroundDownloader(downloader)
    bg_downloader.start()

    try:
        # Download the metadata.
        metadata_local_path = base_path / index_common.ASSET_TOP_METADATA_FILENAME
        metadata_remote_url = urllib.parse.urljoin(base_url, index_common.ASSET_TOP_METADATA_FILENAME)

        metadata = _download_and_parse_metadata(
            bg_downloader,
            metadata_remote_url,
            metadata_local_path)

        # Show what we downloaded.
        logger.info("    API version       : %d", metadata.api_version)
        logger.info("    Asset Library Name: %s", metadata.name)
        if metadata.contact:
            logger.info(
                "    Contact           : %s | %s | %s",
                metadata.contact.name,
                metadata.contact.url,
                metadata.contact.email,
            )
    finally:
        bg_downloader.shutdown()


def _download_and_parse_metadata(
    downloader: BackgroundDownloader,
    metadata_remote_url: str,
    metadata_local_path: Path,
) -> api_models.AssetLibraryMeta:

    # This has to be set on the main thread,
    downloader.clear_download_counts()
    downloader.queue_download(metadata_remote_url, metadata_local_path)

    # Normally this would happen in a timer on a modal operator.
    while not downloader.all_downloads_done():
        downloader.update()

    if downloader.num_downloads_error:
        # The reporter class should have taken care of reporting to the UI already.
        # We just need to stop any further processing.
        raise RuntimeError("download failed, stopping everything")

    json_data = metadata_local_path.read_bytes()
    return api_models.AssetLibraryMeta.model_validate_json(json_data)


# Ignore the type of the `subparsers` argument, because there doesn't seem
# to be a way to make both static mypy and the runtime Python happy at the
# same time.
def add_cli_parser(subparsers: argparse._SubParsersAction) -> None:  # type: ignore[type-arg]
    """Add argparser for this subcommand."""

    parser = subparsers.add_parser("download", help="Download and parse a remote asset library index")
    parser.set_defaults(func=cli_main)

    parser.add_argument(
        "url",
        type=str,
        help="""URL of the remote asset library""",
    )


def _parse_cli_args(arguments_raw: argparse.Namespace) -> CLIArguments:
    """Make sure the passed arguments are valid."""

    try:
        urllib.parse.urlparse(arguments_raw.url)
    except ValueError as ex:
        logger.error("invalid URL specified: {}".format(ex))

    arguments = CLIArguments(
        url=arguments_raw.url,
    )

    return arguments
