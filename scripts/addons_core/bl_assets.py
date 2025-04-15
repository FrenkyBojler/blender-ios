# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "Remote Assets",
    "author": "Sybren Stüvel",
    "version": (0, 0, 1),
    "blender": (4, 4, 0),
    "location": "Nobody Knows",
    "description": "Glue Add-on for remote asset libraries",
    "warning": "",
    # "doc_url": "",
    "support": 'OFFICIAL',
    "category": "System",
}

import logging
import urllib.parse
from pathlib import Path
from typing import Callable, Generator
from contextlib import contextmanager

import bpy

from _bpy_internal.assets.remote_library_index import index_common
from _bpy_internal.assets.remote_library_index import blender_asset_library_openapi as api_models
from _bpy_internal.http.downloader import RequestDescription, CachingDownloader, BackgroundDownloader, DownloadCancelled

logger = logging.getLogger(__name__)


class ASSETS_OT_dummy_download(bpy.types.Operator):
    bl_idname = "assets.dummy_download"
    bl_label = "Dummy Download"

    url: bpy.props.StringProperty(default="http://localhost:8000/")  # type: ignore

    _local_path: Path
    _bg_downloader: BackgroundDownloader
    _num_asset_pages_pending: int
    _timer: bpy.types.Timer | None

    # BackgroundDownloader is independent of `bpy`, and I (Sybren) quite like
    # that. So instead of passing the context to its update() function, so that
    # it can pass those back to this class, just store the context here for the
    # duration of the update() call.
    _operator_context: bpy.types.Context | None

    @classmethod
    def poll(cls, context: bpy.types.Context) -> bool:
        if not context.preferences.system.use_online_access:
            cls.poll_message_set("Allow online access first")
            return False
        return True

    def execute(self, context: bpy.types.Context) -> set[str]:
        # self._local_path = Path(bpy.app.tempdir) / "dummy_asset_library"
        self._local_path = Path("/tmp/dummy_asset_library")
        self._num_asset_pages_pending = 0
        self._operator_context = None

        downloader = CachingDownloader(
            metadata_cache_location=self._local_path / "_local-meta-cache",
            chunk_size=1024 * 16,
        )

        self._bg_downloader = BackgroundDownloader(downloader)
        self._bg_downloader.add_reporter(self)
        self._bg_downloader.start()

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.01, window=context.window)
        wm.modal_handler_add(self)

        # Kickstart the download process.
        self.on_start(context)

        return {'RUNNING_MODAL'}

    def cancel(self, context: bpy.types.Context) -> None:
        wm = context.window_manager
        wm.event_timer_remove(self._timer)

        # It may be tempting to call self.report(...) here, and report on the
        # cancellation. However, this should be done by the caller, when they know
        # of the reason of the cancellation and thus can provide more info.
        num_pending = self._bg_downloader.num_pending_downloads
        if num_pending:
            logger.info("Cancel: Shutting down background downloader, %d downloads pending", num_pending)
        else:
            logger.info("Cancel: Shutting down background downloader")

        with self._context(context):
            self._bg_downloader.shutdown()

    def modal(self, context: bpy.types.Context, event: bpy.types.Event) -> set[str]:
        if event.type == 'ESC':
            num_pending = self._bg_downloader.num_pending_downloads
            self.cancel(context)

            if num_pending:
                msg = "DummyDownloader cancelled with {} downloads pending".format(num_pending)
            else:
                msg = "DummyDownloader cancelled"
            self.report({'WARNING'}, msg)

            return {'CANCELLED'}

        if self._bg_downloader.is_shutdown:
            logger.info("downloader done")
            self.report({'INFO'}, "DummyDownloader Done")
            self.cancel(context)
            return {'FINISHED'}

        with self._context(context):
            self._bg_downloader.update()

        return {'PASS_THROUGH'}

    def on_start(self, context: bpy.types.Context) -> None:
        self._queue_download(
            index_common.ASSET_TOP_METADATA_FILENAME,
            index_common.ASSET_TOP_METADATA_FILENAME,
            self.parse_asset_lib_metadata,
        )

    def parse_asset_lib_metadata(self,
                                 http_req_descr: RequestDescription,
                                 local_file: Path,
                                 ) -> None:
        logger.info("Parsing %s", local_file)
        json_data = local_file.read_bytes()
        metadata = api_models.AssetLibraryMeta.model_validate_json(json_data)

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

        # Download the asset index.
        relative_path = index_common.api_versioned(index_common.ASSET_INDEX_JSON_FILENAME)
        self._queue_download(
            relative_path.as_posix(),
            relative_path,
            self.parse_asset_lib_index,
        )

    def parse_asset_lib_index(self,
                              http_req_descr: RequestDescription,
                              local_file: Path,
                              ) -> None:
        json_data = local_file.read_bytes()
        asset_index = api_models.AssetLibraryIndex.model_validate_json(json_data)

        page_urls = asset_index.page_urls or []

        logger.info("    Schema version    : %s", asset_index.schema_version)
        logger.info("    Asset count       : %d", asset_index.asset_count)
        logger.info("    Pages             : %d", len(page_urls))

        # Download the asset pages.
        self._num_asset_pages_pending = len(page_urls)
        referenced_local_files: list[Path] = []
        for page_index, page_url in enumerate(page_urls):
            # These URLs may be absolute or they may be relative. In any case,
            # do not assume that they can be used direclty as local filesystem path.
            local_path = index_common.api_versioned(f"assets-{page_index:05}.json")
            download_to = self._queue_download(page_url, local_path, self.on_asset_page_downloaded)

            referenced_local_files.append(download_to)

        # Remove any dangling pages of assets (downloaded before, no longer referenced).
        asset_page_dir = self._local_path / index_common.API_VERSIONED_SUBDIR
        # TODO: when upgrading to Python 3.12+, add `case_sensitive=False` to the glob() call.
        for asset_page_file in asset_page_dir.glob("assets-*.json"):
            abs_path = asset_page_dir / asset_page_file
            if abs_path in referenced_local_files:
                continue
            abs_path.unlink()

    def on_asset_page_downloaded(self,
                                 http_req_descr: RequestDescription,
                                 local_file: Path,
                                 ) -> None:
        self._num_asset_pages_pending -= 1
        assert self._num_asset_pages_pending >= 0

        logger.info("Asset index page downloaded: %s", local_file)

        if self._num_asset_pages_pending > 0:
            # Wait until all files have downloaded.
            self.report(
                {'INFO'},
                "Asset library index page downloaded; needs %d more".format(
                    self._num_asset_pages_pending))
            return

        self.report({'INFO'}, "Asset library index downloaded")
        self.cancel(self._operator_context)

    def _queue_download(self, relative_url: str, relative_path: Path | str,
                        on_done: Callable[[RequestDescription, Path], None]) -> Path:
        """Queue up this download, returning the path to which it will be downloaded."""
        remote_url = urllib.parse.urljoin(self.url, relative_url)
        download_to_path = self._local_path / relative_path

        self._bg_downloader.queue_download(remote_url, download_to_path, on_done)

        return download_to_path

    # Below here: CachingDownloadReporter functions:

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self.report({'INFO'}, "Download starting: {}".format(http_req_descr.url))

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self.report({'INFO'}, "Download unnecessary, file already downloaded: {}".format(http_req_descr.url))

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        if isinstance(error, DownloadCancelled):
            if self._num_asset_pages_pending:
                self.report({'WARNING'}, "Cancelled {} pending download".format(self._num_asset_pages_pending))
        else:
            self.report({'ERROR'}, "Error downloading {}: {}".format(http_req_descr.url, error))

        assert self._operator_context is not None
        self.cancel(self._operator_context)

    def download_progress(
        self,
        http_req_descr: RequestDescription,
        content_length_bytes: int,
        downloaded_bytes: int,
    ) -> None:
        percentage = downloaded_bytes / content_length_bytes * 100
        self.report({'INFO'}, "File download progress: {:.0f}%".format(percentage))

    def download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self.report({'INFO'}, "Download finished: {}".format(http_req_descr.url))

    @contextmanager
    def _context(self, context: bpy.types.Context) -> Generator:
        """For the duration of the context manager, set self._operator_context."""
        try:
            self._operator_context = context
            yield
        finally:
            self._operator_context = None


def topbar_blender_menu_draw(self: bpy.types.TOPBAR_MT_blender, context: bpy.types.Context) -> None:
    self.layout.operator("assets.dummy_download")


classes = (ASSETS_OT_dummy_download,)
_register, _unregister = bpy.utils.register_classes_factory(classes)


def register():
    import sys

    do_reload = 'bl_assets' in sys.modules

    from _bpy_internal.assets import remote_library_index

    if do_reload:
        import importlib

        remote_library_index = importlib.reload(remote_library_index)

    remote_library_index.register()
    _register()

    bpy.types.TOPBAR_MT_blender.append(topbar_blender_menu_draw)


def unregister():
    from _bpy_internal.assets import remote_library_index

    bpy.types.TOPBAR_MT_blender.remove(topbar_blender_menu_draw)
    _unregister()
    remote_library_index.unregister()
