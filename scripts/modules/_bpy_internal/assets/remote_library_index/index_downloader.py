# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import logging
import urllib.parse
from typing import Type, TypeVar
from pathlib import Path

import pydantic

from _bpy_internal.http.downloader import Downloader, BackgroundDownloader
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

    downloader = Downloader(
        metadata_cache_location=base_path / "_local-meta-cache",
        chunk_size=8192,
    )
    downloader.http_session.headers.update({'Accept': 'application/json'})

    bg_downloader = BackgroundDownloader(downloader)
    bg_downloader.start()

    try:
        # Download the metadata.
        metadata_local_path = base_path / index_common.ASSET_TOP_METADATA_FILENAME
        metadata_remote_url = urllib.parse.urljoin(base_url, index_common.ASSET_TOP_METADATA_FILENAME)

        metadata = _download_and_parse(
            bg_downloader,
            metadata_remote_url,
            metadata_local_path,
            api_models.AssetLibraryMeta,
        )

        # Show what we downloaded.
        logger.info("    API versions      : %s", metadata.api_versions)
        logger.info("    Asset Library Name: %s", metadata.name)
        if metadata.contact:
            logger.info(
                "    Contact           : %s | %s | %s",
                metadata.contact.name,
                metadata.contact.url,
                metadata.contact.email,
            )

        # Download the main index.
        main_index_relpath = index_common.api_versioned(index_common.ASSET_INDEX_JSON_FILENAME)
        index_local_path = base_path / main_index_relpath
        index_remote_url = urllib.parse.urljoin(base_url, main_index_relpath.as_posix())

        asset_index = _download_and_parse(
            bg_downloader,
            index_remote_url,
            index_local_path,
            api_models.AssetLibraryIndexV1,
        )
        if asset_index.page_urls is None:
            asset_index.page_urls = []

        logger.info("    Schema version    : %s", asset_index.schema_version)
        logger.info("    Asset count       : %d", asset_index.asset_count)
        logger.info("    Pages             : %d", len(asset_index.page_urls))

        # Download the index pages.
        for page_index, page_url in enumerate(asset_index.page_urls):
            # These URLs may be absolute or they may be relative. In any case,
            # do not assume that they can be used direclty as local filesystem path.
            local_path = base_path / index_common.api_versioned(f"assets-{page_index:05}.json")
            remote_url = urllib.parse.urljoin(base_url, page_url)

            page = _download_and_parse(
                bg_downloader,
                remote_url,
                local_path,
                api_models.AssetLibraryIndexPageV1,
            )

            logger.info("    Page              : #%d", page_index)
            if page.asset_count != len(page.assets):
                logger.info("    Asset count       : %d (declared) / %d (actual)", page.asset_count, len(page.assets))
            else:
                logger.info("    Asset count       : %d", page.asset_count)

    finally:
        bg_downloader.shutdown()


M = TypeVar('M', bound=pydantic.BaseModel)


def _download_and_parse(
    downloader: BackgroundDownloader,
    remote_url: str,
    local_path: Path,
    model_class: Type[M],
) -> M:
    downloader.clear_download_counts()
    downloader.queue_download(remote_url, local_path)

    # Normally this would happen in a timer on a modal operator.
    while not downloader.all_downloads_done():
        downloader.update()

    if downloader.num_downloads_error:
        # The reporter class should have taken care of reporting to the UI already.
        # We just need to stop any further processing.
        raise RuntimeError("download failed, stopping everything")

    json_data = local_path.read_bytes()
    model = model_class.model_validate_json(json_data)

    return model


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
