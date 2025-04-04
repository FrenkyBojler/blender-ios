# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import logging
import threading
import urllib.parse
from pathlib import Path

import pydantic

from _bpy_internal.http.downloader import CachingDownloader, RequestDescription, ThreadBridgingReporter, DownloadCancelled
from . import blender_asset_library_openapi as api_models

logger = logging.getLogger(__name__)

_urlpath_library_meta = "asset-library-meta.json"
_urlpath_asset_index = "v1/asset-index.json"
# TODO: unify this with the generator, maybe do not use leading zeroes:
_urlpath_asset_index_page = "v1/assets-{:05}.json"


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
        chunk_size=1,
    )
    main_thread_reporter = DownloadReporter(downloader=downloader)
    thread_bridge = ThreadBridgingReporter(main_thread_reporter)
    downloader.add_reporter(thread_bridge)

    # Download the metadata.
    metadata_local_path = base_path / _urlpath_library_meta
    metadata_remote_url = urllib.parse.urljoin(base_url, _urlpath_library_meta)

    metadata = _download_and_parse_metadata(
        downloader,
        main_thread_reporter,
        thread_bridge,
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


def _download_and_parse_metadata(
    downloader: CachingDownloader,
    main_thread_reporter: DownloadReporter,
    thread_bridge: ThreadBridgingReporter,
    metadata_remote_url: str,
    metadata_local_path: Path,
) -> api_models.AssetLibraryMeta:

    # This has to be set on the main thread,
    main_thread_reporter.add_pending_download()

    def _thread() -> None:
        try:
            downloader.download_to_file(
                metadata_remote_url,
                metadata_local_path,
            )
        except DownloadCancelled:
            logger.warning("download got cancelled")

    t = threading.Thread(name="download", target=_thread, daemon=True)
    t.start()

    # Normally this would happen in a timer on a modal operator.
    while not main_thread_reporter.all_downloads_done():
        thread_bridge.update()

    t.join()

    if main_thread_reporter.num_downloads_error:
        # The reporter class should have taken care of reporting to the UI already.
        # We just need to stop any further processing.
        raise RuntimeError("download failed, stopping everything")

    json_data = metadata_local_path.read_bytes()
    return api_models.AssetLibraryMeta.model_validate_json(json_data)


class DownloadReporter(pydantic.BaseModel):
    model_config = pydantic.ConfigDict(
        arbitrary_types_allowed=True,
    )

    downloader: CachingDownloader

    pending_downloads: int = 0
    num_downloads_ok: int = 0
    num_downloads_error: int = 0

    _logger: logging.Logger = logger.getChild("DownloadReporter")

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self._logger.info(f"Downloading {http_req_descr.http_method} {http_req_descr.url}")

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._logger.debug(f"Local file is fresh, no need to re-download: {local_file}")
        self._mark_download_done()
        self.num_downloads_ok += 1

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        self._logger.error(f"Error downloading (ex={error!r})")
        self._mark_download_done()
        self.num_downloads_error += 1

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        self._logger.debug(
            f"Download progress: {downloaded_bytes} of {content_length_bytes}: "
            f"{downloaded_bytes/content_length_bytes*100:.0f}%"
        )

        self._logger.warning("going to cancel the download, just for shits and giggles")
        self.downloader.cancel_download()

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._logger.info(f"Download finished, stored at {local_file}")
        self._mark_download_done()
        self.num_downloads_ok += 1

    def _mark_download_done(self) -> None:
        self.pending_downloads = max(0, self.pending_downloads - 1)

    def add_pending_download(self) -> None:
        self.pending_downloads += 1

    def all_downloads_done(self) -> bool:
        return self.pending_downloads == 0


def add_cli_parser(subparsers: argparse._SubParsersAction) -> None:
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
