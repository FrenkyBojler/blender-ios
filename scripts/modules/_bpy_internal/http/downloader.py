# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
import logging
import queue
import threading
from pathlib import Path
from typing import Protocol, TypeAlias, Any

import pydantic
import requests
import requests.adapters
import urllib3.util.retry

logger = logging.getLogger(__name__)


_http_retries = urllib3.util.retry.Retry(
    total=10,
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

    This is mostly used to avoid None checks in the CachingDownloader. The print
    statements here are ok-ish for debugging/demo purposes, but in the final
    code, they should be removed.
    """

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        print(f"Downloading {http_req_descr.http_method} {http_req_descr.url}")

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        print(f"Local file is fresh, no need to re-download: {local_file}")

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        print(f"Error downloading: {error}")

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        print(
            f"Download progress: {downloaded_bytes} of {content_length_bytes}: "
            f"{downloaded_bytes/content_length_bytes*100:.0f}%"
        )

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        print(f"Download finished, stored at {local_file}")


class ThreadBridgingReporter(CachingDownloadReporter):
    """DownloadReporter that can bridge threads.

    Bridging two threads T1 and T2 requires two reporters and the downloader itself:

    - Create a CachingDownloadReporter that should get called on T1.
    - Create this ThreadBridgingReporter, passing it the above reporter.
    - Create the CachingDownloader, and put in the thread-bridging reporter.
    - Start the CachingDownloader in T2.
    - Call ThreadBridgingReporter.update() from T1.
    """

    FunctionCall: TypeAlias = tuple[str, tuple[Any, ...]]
    """Tuple of the function name and the positional arguments."""

    _queue: queue.Queue[FunctionCall]
    """Queue of function calls."""

    _logger: logging.Logger

    def __init__(self, reporter: CachingDownloadReporter) -> None:
        self.reporter = reporter
        self._queue = queue.Queue()
        self._logger = logger.getChild(self.__class__.__name__)

    def update(self, *, limit_num_calls=100) -> bool:
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
            function = getattr(self.reporter, function_name)
            function(*function_arguments)

        return self._queue.empty()

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
            prepped.headers["If-Not-Match"] = meta.etag

        with self.http_session.send(prepped, stream=True) as stream:
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
        # TODO: catch validation errors and remove the file if that happens,
        # then act as if the file never existed in the first place.
        return HTTPMetadata.model_validate_json(meta_json)

    def _metadata_if_file_matches(
        self, http_req_descr: RequestDescription, local_path: Path
    ) -> HTTPMetadata | None:
        if not local_path.exists():
            return None

        meta = self._load_metadata(http_req_descr)
        if not meta:
            return None

        if local_path.stat().st_size != meta.content_length:
            return None

        return meta

    def _save_metadata(
        self, http_req_descr: RequestDescription, meta: HTTPMetadata
    ) -> None:
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


class HTTPMetadata(pydantic.BaseModel):
    """HTTP headers, stored so they can be used for conditional requests later."""

    etag: str = ""
    last_modified: str = ""
    content_length: int = 0


class RequestDescription(pydantic.BaseModel):
    """Simple descriptor for HTTP requests.

    This is used to simplify function parameters, as well as a key for hashing
    to find the HTTPMetadata file that stores data of previous calls to this
    HTTP requests.
    """

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
