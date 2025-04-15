# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
import logging
import queue
import threading
from pathlib import Path
from typing import Protocol, TypeAlias, Any, Callable

import pydantic
import requests
import requests.adapters
import urllib3.util.retry

logger = logging.getLogger(__name__)


_http_retries = urllib3.util.retry.Retry(
    total=8,  # Times,
    backoff_factor=0.05,
)
_http_adapter = requests.adapters.HTTPAdapter(max_retries=_http_retries)
_http_session = requests.session()
_http_session.mount("https://", _http_adapter)
_http_session.mount("http://", _http_adapter)


class CachingDownloadReporter(Protocol):
    """This protocol can be used to receive reporting from CachingDownloader."""

    def download_starts(self, http_req_descr: RequestDescription) -> None: ...

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        """The previous download to this file is still fresh."""

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None: ...

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None: ...

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        """The URL was downloaded to the given file."""


class _DummyReporter(CachingDownloadReporter):
    """Dummy CachingDownloadReporter.

    Does not do anything. This is mostly used to avoid None checks in the
    CachingDownloader.
    """

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        pass

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        pass

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        pass

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        pass

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        pass


class ThreadBridgingReporter(CachingDownloadReporter):
    """DownloadReporter that can bridge threads.

    Bridging two threads T1 and T2 requires two reporters and the downloader itself:

    - Create a CachingDownloadReporter that should get called on T1.
    - Create this ThreadBridgingReporter, passing it the above reporter.
    - Create the CachingDownloader, and put in the thread-bridging reporter.
    - Start the CachingDownloader in T2.
    - Call ThreadBridgingReporter.update() from T1.

    See `BackgroundDownloader` for a concrete use.
    """

    FunctionCall: TypeAlias = tuple[str, tuple[Any, ...]]
    """Tuple of the function name and the positional arguments."""

    _queue: queue.Queue[FunctionCall]
    """Queue of function calls."""

    _reporters: list[CachingDownloadReporter]

    _logger: logging.Logger

    def __init__(self) -> None:
        self._reporters = []
        self._queue = queue.Queue()
        self._logger = logger.getChild(self.__class__.__name__)

    def add_reporter(self, reporter: CachingDownloadReporter) -> None:
        self._reporters.append(reporter)

    def update(self, *, limit_num_calls: int = 100) -> bool:
        """Handle queued function calls on the thread that calls this function.

        Only a finite number of queued calls is processed, to avoid blocking the
        calling thread completely.

        Returns whether there are still function calls left to process.
        """

        for _ in range(limit_num_calls):
            try:
                # Wait 1ms for any calls to arrive. This slows down this thread
                # a little bit, to give other threads a chance to run.
                queued_call = self._queue.get(block=True, timeout=0.001)
            except queue.Empty:
                # Not having anything to do is fine.
                return False

            function_name, function_arguments = queued_call
            for reporter in self._reporters:
                function = getattr(reporter, function_name)
                function(*function_arguments)

        return not self._queue.empty()

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self._queue_call('download_starts', http_req_descr)

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._queue_call('already_downloaded', http_req_descr, local_file)

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        self._queue_call('download_error', http_req_descr, error)

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        self._queue_call('download_progress', http_req_descr, content_length_bytes, downloaded_bytes)

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._queue_call('download_finished', http_req_descr, local_file)

    def _queue_call(self, function_name: str, *function_args: Any) -> None:
        """Put a function call in the queue."""
        self._logger.debug(f"{function_name}{function_args}")
        self._queue.put((function_name, function_args))


class CachingDownloader:
    """Caching file downloader.

    Request an URL and stream the body of the response to a file on disk.
    Metadata is saved in a caller-determined location on disk, in a file per
    requested URL.

    Caching is performed via the HTTP headers 'ETag'/'If-None-Match' and
    'Last-Modified'/'If-Modified-Since'.

    See `BackgroundDownloader` to download things in a background thread.
    """

    # TODO: make a metadata cache class, instead of always using a path on disk.
    metadata_cache_location: Path
    """Directory where request metadata is stored."""

    http_session: requests.Session
    """Requests session, for control over retry behavior, TCP connection pooling, etc."""

    chunk_size: int = 8192
    """Download this many bytes before saving to disk and reporting progress."""

    _reporter: CachingDownloadReporter = _DummyReporter()

    _cancel_download_event: threading.Event

    def __init__(
            self,
            metadata_cache_location: Path,
            *,
            http_session: requests.Session = _http_session,
            chunk_size: int = 8192,
    ) -> None:
        self.metadata_cache_location = metadata_cache_location
        self.http_session = http_session
        self.chunk_size = chunk_size
        self._cancel_download_event = threading.Event()

    def download_to_file(
        self, url: str, local_path: Path, *, http_method: str = "GET"
    ) -> None:
        """Download the URL to a file on disk.

        The download is streamed to 'local_path + "~"' first. When succesful, it
        is renamed to the given path, overwriting any pre-existing file.

        Raises a HTTPRequestDownloadError for specific HTTP errors.
        """

        http_req_descr = RequestDescription(http_method=http_method, url=url)

        self._reporter.download_starts(http_req_descr)

        http_meta = self._metadata_if_file_matches(http_req_descr, local_path)

        # Download to a temporary file first.
        temp_path = local_path.with_suffix(local_path.suffix + "~")
        temp_path.parent.mkdir(exist_ok=True, parents=True)

        try:
            http_meta = self._stream_to_file(http_req_descr, temp_path, http_meta)
        except Exception as ex:
            # Clean up the partially downloaded file.
            temp_path.unlink(missing_ok=True)
            self._reporter.download_error(http_req_descr, ex)
            raise
        finally:
            # One way or the other, the download is no longer running, so any
            # pending cancellation can be cleared.
            self._cancel_download_event.clear()

        if http_meta is None:
            # Local file is already fresh, no need to re-download.
            assert temp_path.exists() == False
            self._reporter.already_downloaded(http_req_descr, local_path)
            return

        # Move the downloaded file to the final filename.
        # TODO: AFAIK this is necessary on Windows, while on other platforms the
        # rename is atomic. See if we can get this atomic everywhere.
        local_path.unlink(missing_ok=True)
        temp_path.rename(local_path)

        self._save_metadata(http_req_descr, http_meta)

        self._reporter.download_finished(http_req_descr, local_path)

    def _stream_to_file(
        self,
        http_req_descr: RequestDescription,
        local_path: Path,
        meta: HTTPMetadata | None,
    ) -> HTTPMetadata | None:
        """Stream the remote URL to a local file.

        :return: the metadata of the downloaded data, or None if the passed-in
            metadata matches the URL (a "304 Not Modified" was returned).
        """

        # Don't bother doing anything when the download was cancelled already.
        if self._cancel_download_event.is_set():
            raise DownloadCancelled(http_req_descr)

        req = requests.Request(http_req_descr.http_method, http_req_descr.url)
        prepped: requests.PreparedRequest = self.http_session.prepare_request(req)
        if meta:
            prepped.headers["If-Modified-Since"] = meta.last_modified
            prepped.headers["If-None-Match"] = meta.etag

        with self.http_session.send(prepped, stream=True) as stream:
            logger.debug(
                "HTTP %s %s (headers %s) -> %d",
                http_req_descr.http_method,
                http_req_descr.url,
                prepped.headers,
                stream.status_code,
            )

            stream.raise_for_status()

            if stream.status_code == 304:  # 304 Not Modified
                # The remote file matches what we have locally. Don't bother streaming.
                return None

            # Avoid reporting any progress when the download was cancelled.
            if self._cancel_download_event.is_set():
                raise DownloadCancelled(http_req_descr)

            # Determine how many bytes are expected.
            content_length_str: str = stream.headers.get("Content-Length") or ""
            try:
                content_length = int(content_length_str, base=10)
            except ValueError:
                raise ContentLengthUnknownError(http_req_descr) from None
            self._reporter.download_progress(http_req_descr, content_length, 0)

            # Stream the response to a file.
            num_downloaded_bytes = 0
            with local_path.open("wb") as file:
                for chunk in stream.iter_content(chunk_size=self.chunk_size):

                    if self._cancel_download_event.is_set():
                        raise DownloadCancelled(http_req_descr)

                    file.write(chunk)
                    num_downloaded_bytes += len(chunk)

                    self._reporter.download_progress(
                        http_req_descr, content_length, num_downloaded_bytes
                    )

                    if num_downloaded_bytes > content_length:
                        raise ResponseTooLargeError(http_req_descr)

            # File was downloaded succesfully, store the metadata.
            meta = HTTPMetadata(
                request=http_req_descr,
                etag=stream.headers.get("ETag") or "",
                last_modified=stream.headers.get("Last-Modified") or "",
                content_length=num_downloaded_bytes,
            )

        return meta

    def _cache_key(self, http_req_descr: RequestDescription) -> str:
        method = http_req_descr.http_method
        url = http_req_descr.url
        return hashlib.sha256(f"{method}:{url}".encode()).hexdigest()

    def _metadata_path(self, http_req_descr: RequestDescription) -> Path:
        # TODO: maybe use part of the cache key to bucket into subdirectories?
        return self.metadata_cache_location / self._cache_key(http_req_descr)

    def _load_metadata(self, http_req_descr: RequestDescription) -> HTTPMetadata | None:
        meta_path = self._metadata_path(http_req_descr)
        if not meta_path.exists():
            return None
        meta_json = meta_path.read_bytes()

        try:
            return HTTPMetadata.model_validate_json(meta_json)
        except pydantic.ValidationError:
            # File was an old format, got corrupted, or is otherwise unusable.
            # Just act as if it never existed in the first place.
            meta_path.unlink()
            return None

    def _metadata_if_file_matches(
        self, http_req_descr: RequestDescription, local_path: Path
    ) -> HTTPMetadata | None:
        if not local_path.exists():
            return None

        meta = self._load_metadata(http_req_descr)
        if not meta:
            return None

        assert meta.request == http_req_descr, f"req: {http_req_descr}, meta.req: {meta.request}"
        if meta.request != http_req_descr:
            # Somehow the metadata was loaded, but didn't match this request. Weird.
            return None

        local_file_size = local_path.stat().st_size
        if local_file_size == 0:
            # This is an optimization for downloading bigger files. There is no
            # need to do a conditional download of a zero-bytes file. It is more
            # likely that something went wrong and a file got truncated.
            #
            # And even if the file is of the correct size, non-conditinally
            # doing the same request for the empty file will require less data
            # than including the headers necessary for a conditional download.
            return None

        if local_file_size != meta.content_length:
            return None

        return meta

    def _save_metadata(
        self, http_req_descr: RequestDescription, meta: HTTPMetadata
    ) -> None:
        meta.request = http_req_descr

        meta_json = meta.model_dump_json()
        meta_path = self._metadata_path(http_req_descr)

        meta_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        meta_path.write_bytes(meta_json.encode())

    def add_reporter(self, reporter: CachingDownloadReporter) -> None:
        """Add a reporter to receive download progress information.

        The reporter's functions are called from the same thread as the calls to
        this CachingDownloader.
        """
        if self.has_reporter():
            raise ValueError(
                f"Only one reporter is supported, I already have {self._reporter}"
            )
        self._reporter = reporter

    def has_reporter(self) -> bool:
        return not isinstance(self._reporter, _DummyReporter)

    def cancel_download(self) -> None:
        """Cancel any running download.

        Thread-safe, can be called from a different thread than the download
        call itself.

        If there is no active download when this function is called, the next
        download will be cancelled.
        """
        self._cancel_download_event.set()


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

    # Here and below, 'RequestDescription' is quoted because Pylance (used by
    # VSCode) doesn't fully grasp the `from __future__ import annotations` yet.
    # Or at least so it seems - it shows these lines in error, while both mypy
    # is fine with it and at runtime it works.
    QueuedDownload: TypeAlias = tuple['RequestDescription', Path]
    """Tuple of URL to download, and path to download it to."""
    _queue: queue.Queue[QueuedDownload]

    # Keep track of which callback to call on the completion of which HTTP request.
    # This assumes that RequestDescriptions are unique, and not queued up
    # multiple times simultaneously.
    DownloadDoneCallback: TypeAlias = Callable[['RequestDescription', Path], None]
    _on_downloaded_callbacks: dict[RequestDescription, DownloadDoneCallback]

    def __init__(self, downloader: CachingDownloader) -> None:
        self.num_downloads_ok = 0
        self.num_downloads_error = 0
        self._num_pending_downloads = 0
        self._on_downloaded_callbacks = {}

        # Set up a thread bridge, so that updates are received on the main thread.
        self._thread_bridge = ThreadBridgingReporter()
        self._thread_bridge.add_reporter(self)

        self._queue = queue.Queue()

        self._shutdown_event = threading.Event()
        """Set this to trigger a shutdown."""
        self._shutdown_complete_event = threading.Event()
        """Gets set when shutdown is complete."""

        # Set up the downloader in a background thread.
        self._downloader = downloader
        self._downloader.add_reporter(self._thread_bridge)
        self._downloader_thread = threading.Thread(
            name="BackgroundDownloader",
            target=self._download_queued_items,
            daemon=True,
        )

    def add_reporter(self, reporter: CachingDownloadReporter) -> None:
        """Add a reporter to receive updates when .update() is called."""
        self._thread_bridge.add_reporter(reporter)

    def queue_download(self, remote_url: str, local_path: Path,
                       on_download_done: DownloadDoneCallback | None = None) -> None:
        """Queue up a download of some URL to a location on disk."""

        if self._shutdown_event.is_set():
            raise RuntimeError("BackgroundDownloader is shutting down, cannot queue new downloads")

        self._num_pending_downloads += 1

        http_req_descr = RequestDescription(http_method='GET', url=remote_url)
        if on_download_done:
            self._on_downloaded_callbacks[http_req_descr] = on_download_done
        self._queue.put((http_req_descr, local_path))

    def all_downloads_done(self) -> bool:
        return self._num_pending_downloads == 0

    @property
    def num_pending_downloads(self) -> int:
        return self._num_pending_downloads

    def clear_download_counts(self) -> None:
        """Resets the number of ok/error downloads."""

        self.num_downloads_ok = 0
        self.num_downloads_error = 0

    def start(self) -> None:
        """Start the downloaded thread.

        This MUST be called before calling .update().
        """
        if self._shutdown_event.is_set():
            raise ValueError("BackgroundDownloader was shut down, cannot start again")
        self._downloader_thread.start()

    @property
    def is_shutdown_requested(self) -> bool:
        return self._shutdown_event.is_set()

    @property
    def is_shutdown_complete(self) -> bool:
        return self._shutdown_complete_event.is_set()

    def shutdown(self) -> None:
        """Cancel any pending downloads and shut down the background thread.

        Blocks until the background thread has stopped and all queued updates
        have been processed.

        NOTE: call this from the same thread as used to call .update().
        """
        if self._shutdown_complete_event.is_set() and not self._downloader_thread.is_alive():
            self._logger.debug("shutdown already completed")
            return

        self._logger.debug("shutting down")
        self._shutdown_event.set()

        self._logger.debug("cancelling any running download")
        self._downloader.cancel_download()

        self._logger.debug("waiting for download thread to stop")
        self._downloader_thread.join()

        self._logger.debug("processing any pending updates")
        while self._thread_bridge.update():
            pass

        self._logger.debug("download thread stopped")
        self._shutdown_complete_event.set()

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

            if self._shutdown_event.is_set():
                break

            http_req_descr, local_path = queued_download

            # Try and download it.
            try:
                self._downloader.download_to_file(
                    http_req_descr.url,
                    local_path,
                    http_method=http_req_descr.http_method,)
            except DownloadCancelled:
                # Can be logged at a lower level, because the caller did the
                # cancelling, and can log/report things more loudly if
                # necessary.
                logger.debug("download got cancelled: {}".format(http_req_descr))
            except Exception as ex:
                logger.exception("could not download {}: {}".format(http_req_descr, ex))

        self._logger.debug("download thread shutting down")

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        """CachingDownloadReporter interface function."""

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        """CachingDownloadReporter interface function.

        Keeps track of internal bookkeeping.
        """
        self._logger.debug(f"Local file is fresh, no need to re-download: {local_file}")
        self._mark_download_done()
        self.num_downloads_ok += 1
        self._call_on_downloaded_callback(http_req_descr, local_file)

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        """CachingDownloadReporter interface function.

        Keeps track of internal bookkeeping.
        """
        self._logger.error(f"Error downloading (ex={error!r})")
        self._mark_download_done()
        self.num_downloads_error += 1

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        """CachingDownloadReporter interface function.

        Keeps track of internal bookkeeping.
        """
        self._logger.debug(
            f"Download progress: {downloaded_bytes} of {content_length_bytes}: "
            f"{downloaded_bytes/content_length_bytes*100:.0f}%"
        )

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        """CachingDownloadReporter interface function.

        Keeps track of internal bookkeeping.
        """
        self._logger.debug(f"Download finished, stored at %s", local_file)
        self._mark_download_done()
        self.num_downloads_ok += 1
        self._call_on_downloaded_callback(http_req_descr, local_file)

    def _mark_download_done(self) -> None:
        """Reduce the number of pending downloads."""
        self._num_pending_downloads -= 1
        assert self._num_pending_downloads >= 0, "downloaded more files than were queued"

    def _call_on_downloaded_callback(self, http_req_descr: RequestDescription, local_file: Path) -> None:
        """Call the 'on-download-done' callback for this request."""

        if self._shutdown_event.is_set():
            # Do not call any callbacks any more, as the downloader is trying to shut down.
            return

        try:
            callback = self._on_downloaded_callbacks.pop(http_req_descr)
        except KeyError:
            # Not having a callback is fine.
            return

        logger.debug("download done, calling %s", callback.__name__)
        callback(http_req_descr, local_file)


class HTTPMetadata(pydantic.BaseModel):
    """HTTP headers, stored so they can be used for conditional requests later."""

    request: RequestDescription
    etag: str = ""
    last_modified: str = ""
    content_length: int = 0


class RequestDescription(pydantic.BaseModel):
    """Simple descriptor for HTTP requests.

    This is used to simplify function parameters, as well as a key for hashing
    to find the HTTPMetadata file that stores data of previous calls to this
    HTTP requests.
    """
    model_config = pydantic.ConfigDict(frozen=True)

    http_method: str
    url: str


class HTTPRequestDownloadError(RuntimeError):
    """Base class for HTTP download errors.

    Note that errors thrown by the Requests library, or thrown when writing
    downloaded data to disk, are NOT wrapped in this class, and are raised
    as-is.
    """

    http_req_desc: RequestDescription

    def __init__(self, http_req_desc: RequestDescription) -> None:
        super().__init__()
        self.http_req_desc = http_req_desc

    def __repr__(self) -> str:
        return f"{self.__class__.__name__}({self.http_req_desc})"

    def __str__(self) -> str:
        return repr(self)


class ContentLengthUnknownError(HTTPRequestDownloadError):
    """Raised when a HTTP response does not have a Content-Length header.

    Also raised when the header exists, but cannot be parsed as integer.
    """


class ResponseTooLargeError(HTTPRequestDownloadError):
    """Raised when a HTTP response body is larger than its Content-Length header indicates."""


class DownloadCancelled(HTTPRequestDownloadError):
    """Raised when the CachingDownloader.cancel_download() function was called.

    This exception is raised in the thread that called
    CachingDownloader.download_to_file(), and not from the thread doing the
    cancellation.
    """
