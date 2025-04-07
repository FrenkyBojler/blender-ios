# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import logging
import queue
import threading
import urllib.parse
from pathlib import Path
from typing import TypeAlias

import pydantic

from _bpy_internal.http.downloader import CachingDownloader, CachingDownloadReporter, RequestDescription, ThreadBridgingReporter, DownloadCancelled
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
        chunk_size=10,
    )

    bg_downloader = BackgroundDownloader(downloader)
    bg_downloader.start()

    try:
        # Download the metadata.
        metadata_local_path = base_path / _urlpath_library_meta
        metadata_remote_url = urllib.parse.urljoin(base_url, _urlpath_library_meta)

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


class BackgroundDownloader:
    """Wrapper for a CachingDownloader + reporter.

    The downloader will run in a separate thread, and the reporter will receive
    updates on the main thread (or whatever thread runs
    BackgroundDownloader.update()).
    """

    num_downloads_ok: int
    num_downloads_error: int
    _num_pending_downloads: int

    _logger: logging.Logger = logger.getChild("BackgroundDownloader")

    QueuedDownload: TypeAlias = tuple[str, Path]
    """Tuple of URL to download, and path to download it to."""
    _queue: queue.Queue[QueuedDownload]

    def __init__(self, downloader: CachingDownloader, reporter: CachingDownloadReporter | None = None) -> None:
        self.num_downloads_ok = 0
        self.num_downloads_error = 0
        self._num_pending_downloads = 0

        # Set up a thread bridge, so that updates are received on the main thread.
        self._thread_bridge = ThreadBridgingReporter()
        self._thread_bridge.add_reporter(self)

        if reporter is not None:
            # TODO: remove this parameter, add an 'add_reporter()' method.
            self._thread_bridge.add_reporter(reporter)

        self._queue = queue.Queue()
        self._shutdown_event = threading.Event()

        # Set up the downloader in a background thread.
        self._downloader = downloader
        self._downloader.add_reporter(self._thread_bridge)
        self._downloader_thread = threading.Thread(
            name="BackgroundDownloader",
            target=self._download_queued_items,
            daemon=True,
        )

    def queue_download(self, remote_url: str, local_path: Path) -> None:
        self._num_pending_downloads += 1
        self._queue.put((remote_url, local_path))

    def all_downloads_done(self) -> bool:
        return self._num_pending_downloads == 0

    def clear_download_counts(self) -> None:
        """Resets the number of ok/error downloads."""
        self.num_downloads_ok = 0
        self.num_downloads_error = 0

    def start(self) -> None:
        self._downloader_thread.start()

    def shutdown(self) -> None:
        self._shutdown_event.set()
        self._downloader.cancel_download()
        self._logger.debug("waiting for download thread to stop")
        self._downloader_thread.join()
        self._logger.debug("download thread stopped")

    def update(self) -> None:
        """Call frequently to ensure the download progress is reported.

        The reports will be sent to self.reporter, in the same thread that calls this method.
        """
        if not self._downloader_thread.is_alive():
            raise RuntimeError("start the download thread first")
        self._thread_bridge.update()

    def _download_queued_items(self) -> None:
        """Runs in a daemon thread to download stuff."""

        while not self._shutdown_event.is_set():
            # Pop an item off the queue.
            try:
                queued_download = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue

            remote_url, local_path = queued_download

            # Try and download it.
            try:
                self._downloader.download_to_file(remote_url, local_path)
            except DownloadCancelled:
                logger.warning("download got cancelled: {}".format(remote_url))
            except Exception as ex:
                logger.exception("could not download {}: {}".format(remote_url, ex))

        self._logger.debug("download thread shutting down")

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

        # self._logger.warning("going to cancel the download, just for shits and giggles")
        # self._downloader.cancel_download()

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._logger.info(f"Download finished, stored at {local_file}")
        self._mark_download_done()
        self.num_downloads_ok += 1

    def _mark_download_done(self) -> None:
        self._num_pending_downloads = max(0, self._num_pending_downloads - 1)


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
