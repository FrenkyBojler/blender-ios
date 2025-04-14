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
from enum import Enum
from pathlib import Path
from typing import Callable, TypeAlias

import bpy

from _bpy_internal.assets.remote_library_index import index_common
from _bpy_internal.assets.remote_library_index import blender_asset_library_openapi as api_models
from _bpy_internal.http.downloader import RequestDescription, CachingDownloader, BackgroundDownloader, DownloadCancelled

logger = logging.getLogger(__name__)


class AssetDownloadState(Enum):
    STARTING = 0
    DOWNLOADING = 1
    DONE = 2


class ASSETS_OT_dummy_download(bpy.types.Operator):
    bl_idname = "assets.dummy_download"
    bl_label = "Dummy Download"

    url: bpy.props.StringProperty(default="http://localhost:8000/")  # type: ignore

    _local_path: Path
    _bg_downloader: BackgroundDownloader
    _timer: bpy.types.Timer | None
    _state: AssetDownloadState

    # Keep track of which callback to call on the completion of which HTTP request.
    # This assumes that RequestDescriptions are unique, and not queued up
    # multiple times simultaneously.
    DownloadDoneCallback: TypeAlias = Callable[[RequestDescription, Path], None]
    _on_done_callbacks: dict[RequestDescription, DownloadDoneCallback]

    _num_asset_pages_pending: int

    @classmethod
    def poll(cls, context: bpy.types.Context) -> bool:
        if not context.preferences.system.use_online_access:
            cls.poll_message_set("Allow online access first")
            return False
        return True

    def execute(self, context: bpy.types.Context) -> set[str]:
        self._local_path = Path(bpy.app.tempdir) / "dummy_asset_library"
        self._state = AssetDownloadState.STARTING
        self._on_done_callbacks = {}
        self._num_asset_pages_pending = 0

        downloader = CachingDownloader(
            metadata_cache_location=self._local_path / "_local-meta-cache",
            chunk_size=10,
        )

        self._bg_downloader = BackgroundDownloader(downloader)
        self._bg_downloader.add_reporter(self)
        self._bg_downloader.start()

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.01, window=context.window)
        wm.modal_handler_add(self)

        return {'RUNNING_MODAL'}

    def cancel(self, context: bpy.types.Context) -> None:
        num_pending = self._bg_downloader.num_pending_downloads

        logger.info("Cancel: Shutting down background downloader, %d downloads pending", num_pending)
        if num_pending:
            # The shutdown call below will block this thread, so by the time the
            # report is visible, it's already cancelled.
            self.report({'WARNING'}, "Cancelled {} pending download".format(num_pending))

        self._bg_downloader.shutdown()

        wm = context.window_manager
        wm.event_timer_remove(self._timer)

    def modal(self, context: bpy.types.Context, event: bpy.types.Event) -> set[str]:
        if event.type in {'RIGHTMOUSE', 'ESC'}:
            self.cancel(context)
            return {'CANCELLED'}

        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        do_continue = self.on_timer(context)
        if not do_continue:
            self.cancel(context)
            return {'FINISHED'}

        return {'PASS_THROUGH'}

    def on_timer(self, context: bpy.types.Context) -> bool:
        """Returns whether the code needs to continue looping or not."""

        match self._state:
            case AssetDownloadState.STARTING:
                self.on_start(context)
            case AssetDownloadState.DOWNLOADING:
                pass
            case AssetDownloadState.DONE:
                logger.info("downloader done")
                self.report({'INFO'}, "DummyDownloader Done")
                return False

        # logger.info("operator state: %s", self._state)
        self._bg_downloader.update()
        return True

    def on_start(self, context: bpy.types.Context) -> None:
        self._state = AssetDownloadState.DOWNLOADING
        self._queue_download(
            index_common.ASSET_TOP_METADATA_FILENAME,
            index_common.ASSET_TOP_METADATA_FILENAME,
            self.parse_asset_lib_metadata,
        )

    def parse_asset_lib_metadata(self,
                                 http_req_descr: RequestDescription,
                                 local_file: Path,
                                 ) -> None:
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
        for page_index, page_url in enumerate(page_urls):
            # These URLs may be absolute or they may be relative. In any case,
            # do not assume that they can be used direclty as local filesystem path.
            local_path = index_common.api_versioned(f"assets-{page_index:05}.json")
            self._queue_download(page_url, local_path, self.on_asset_page_downloaded)

    def on_asset_page_downloaded(self,
                                 http_req_descr: RequestDescription,
                                 local_file: Path,
                                 ) -> None:
        self._num_asset_pages_pending -= 1
        assert self._num_asset_pages_pending >= 0

        logger.info("Asset index page downloaded: %s", local_file)

        if self._num_asset_pages_pending > 0:
            # Wait until all files have downloaded.
            return

        self.report({'INFO'}, "Asset library index downloaded")

    def _queue_download(self, relative_url: str, relative_path: Path | str,
                        on_done: Callable[[RequestDescription, Path], None]) -> None:
        remote_url = urllib.parse.urljoin(self.url, relative_url)
        local_path = self._local_path / relative_path

        http_req_descr = RequestDescription(http_method='GET', url=remote_url)
        assert http_req_descr not in self._on_done_callbacks
        self._on_done_callbacks[http_req_descr] = on_done

        self._bg_downloader.queue_download(remote_url, local_path)

    def _on_download_finished(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        callback = self._on_done_callbacks.pop(http_req_descr)
        logger.info("download done, calling %s", callback.__name__)
        callback(http_req_descr, local_file)

    # Below here: CachingDownloadReporter functions:

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self.report({'INFO'}, "Download starting: {}".format(http_req_descr.url))

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        self._on_download_finished(http_req_descr, local_file)

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        if isinstance(error, DownloadCancelled):
            # Don't report here, because the 'cancel' function itself already reports a warning.
            pass
        else:
            self.report({'ERROR'}, "Error downloading {}: {}".format(http_req_descr.url, error))

        # TODO: pass the context into bg_downloader.update() so that it can be passed to here.
        self.cancel(bpy.context)

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
        self._on_download_finished(http_req_descr, local_file)


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
