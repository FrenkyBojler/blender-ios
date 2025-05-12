# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "HTTP Download Demo",
    "author": "Sybren Stüvel",
    "version": (0, 0, 1),
    "blender": (4, 5, 0),
    "location": "Blender menu",
    "description": "Demo add-on for HTTP file downloader",
    "warning": "This is just a demo",
    "support": 'NONE',
    "category": "System",
}

from pathlib import Path
from typing import Generator
from contextlib import contextmanager
import multiprocessing

# To work around:
# mypy   : Variable "multiprocessing.Event" is not valid as a type
#          note: See https://mypy.readthedocs.io/en/stable/common_issues.html#variables-vs-type-aliases
# Pylance: Variable not allowed in type expression
from multiprocessing.synchronize import Event as EventClass

import bpy

from _bpy_internal.http.downloader import RequestDescription, ConditionalDownloader, BackgroundDownloader, DownloadCancelled

# Just a demo URL.
url = "https://projects.blender.org/blender/blender/raw/commit/a26ed85adffe6eb7cd609808553aa2026ce59bdf/README.md"

# Path to download to, and to store metadata at.
local_path = Path("/tmp")
download_to_path = local_path / 'README.md'


class HTTP_OT_demo_download_foreground(bpy.types.Operator):
    bl_idname = "http.demo_download_foreground"
    bl_label = "Example Foreground Download"

    @classmethod
    def poll(cls, context: bpy.types.Context) -> bool:
        if not context.preferences.system.use_online_access:
            cls.poll_message_set("Allow online access first")
            return False
        return True

    def execute(self, context: bpy.types.Context) -> set[str]:
        downloader = ConditionalDownloader(
            metadata_cache_location=local_path / "_local-meta-cache",
        )

        downloader.download_to_file(url, download_to_path)

        self.report({'INFO'}, "File downloaded to {}".format(download_to_path))
        return {'FINISHED'}

    # Below here: CachingDownloadReporter functions:

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self.report({'INFO'}, "Download starting: {}".format(http_req_descr.url))

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        print("Download unnecessary, file already downloaded: {}".format(http_req_descr.url))

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        self.report({'ERROR'}, "Error downloading {}: {}".format(http_req_descr.url, error))

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
        print("Download finished: {}".format(http_req_descr.url))


class HTTP_OT_demo_download_background(bpy.types.Operator):
    bl_idname = "http.demo_download_background"
    bl_label = "Example Background Download"

    _bg_downloader: BackgroundDownloader
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
        self._operator_context = None

        downloader = ConditionalDownloader(
            metadata_cache_location=local_path / "_local-meta-cache",
        )

        # Create the BackgroundDownloader, adding this operator as a reporter:
        self._bg_downloader = BackgroundDownloader(downloader)
        self._bg_downloader.add_reporter(self)
        self._bg_downloader.start()

        # Use a timer to regularly get events, in order to call self._bg_downloader.update().
        wm = context.window_manager
        self._timer = wm.event_timer_add(0.01, window=context.window)
        wm.modal_handler_add(self)

        # Kickstart the download process.
        self._bg_downloader.queue_download(url, download_to_path, self.on_done)

        return {'RUNNING_MODAL'}

    def modal(self, context: bpy.types.Context, event: bpy.types.Event) -> set[str]:
        if event.type == 'ESC':
            self.cancel(context)
            return {'CANCELLED'}

        if self._bg_downloader.is_shutdown_complete:
            self.cancel(context)
            return {'FINISHED'}

        with self._context(context):
            # self._context(context) just ensures that self._operator_context is set.
            # This is safe to do, as it's cleared when the `with` statement ends.
            #
            # Its purpose is to be able to use the context in the reporter functions,
            # without having to make the downloader/reporter classes aware of context
            # (or anything Blender-specific, even).
            self._bg_downloader.update()

        return {'PASS_THROUGH'}

    def cancel(self, context: bpy.types.Context) -> None:
        import threading

        assert threading.current_thread() == threading.main_thread()

        if self._timer:
            wm = context.window_manager
            wm.event_timer_remove(self._timer)
            self._timer = None

        with self._context(context):
            self._bg_downloader.shutdown()

    def on_done(self, http_req_descr: RequestDescription, local_file: Path) -> None:
        self.report({'INFO'}, "File downloaded to {}".format(local_file))

        # Since this is a demo of a single-file download, things can shut down
        # from here as the demo is done. In more complex use cases, this function
        # can inspect the downloaded file (for example by parsing a JSON file),
        # then proceed to download more.

        self.cancel(self._operator_context)

    # Below here: CachingDownloadReporter functions:

    def download_starts(self, http_req_descr: RequestDescription) -> None:
        self.report({'INFO'}, "Download starting: {}".format(http_req_descr.url))

    def already_downloaded(
        self,
        http_req_descr: RequestDescription,
        local_file: Path,
    ) -> None:
        print("Download unnecessary, file already downloaded: {}".format(http_req_descr.url))

    def download_error(
        self,
        http_req_descr: RequestDescription,
        error: Exception,
    ) -> None:
        if isinstance(error, DownloadCancelled):
            print("Download cancelled: {}".format(http_req_descr))
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


class HTTP_OT_demo_multiprocessing(bpy.types.Operator):
    bl_idname = "http.demo_multiprocessing"
    bl_label = "Example Multiprocessing"

    def execute(self, context: bpy.types.Context) -> set[str]:
        # On Linux, 'fork' is the default. However the Python docs state "Note
        # that safely forking a multithreaded process is problematic.", and then
        # mention:
        #
        # The default start method will change away from fork in Python 3.14.
        # Code that requires fork should explicitly specify that via
        # get_context() or set_start_method().
        #
        # So I (Sybren) figure it's better to test with the 'spawn' method,
        # which is also the current default on Windows and macOS.
        mp_context = multiprocessing.get_context(method='spawn')

        parent_conn, child_conn = mp_context.Pipe()
        self._pipe = parent_conn
        self._queue = mp_context.Queue()
        self._shutdown: EventClass = mp_context.Event()

        import http_download_bgproc
        self._process = mp_context.Process(
            target=http_download_bgproc.background_task,
            args=(child_conn, self._queue, self._shutdown),
        )
        self._process.start()

        # Use a timer to regularly get events, in order to call self._bg_downloader.update().
        wm = context.window_manager
        self._timer = wm.event_timer_add(0.01, window=context.window)
        wm.modal_handler_add(self)

        return {'RUNNING_MODAL'}

    def modal(self, context: bpy.types.Context, event: bpy.types.Event) -> set[str]:
        if event.type == 'ESC':
            self.cancel(context)
            return {'CANCELLED'}

        if event.type == 'D':
            self.report({'INFO'}, "Queueing download")
            self._queue.put('https://${SERVER}/${PATH}')

        has_data = self._pipe.poll()
        if has_data:
            try:
                msg = self._pipe.recv()
            except EOFError:
                self.report({'WARNING'}, "Pipe closed unexpectedly")
            else:
                print(f"Received message: {msg}")

        return {'PASS_THROUGH'}

    def cancel(self, context: bpy.types.Context) -> None:
        if self._timer:
            wm = context.window_manager
            wm.event_timer_remove(self._timer)
            self._timer = None

        self._shutdown.set()

        try:
            while True:
                msg = self._pipe.recv()
                print(f"Received message: {msg}")
        except EOFError:
            self.report({'INFO'}, "Subprocess downloader closed")

        self._process.join()


def topbar_blender_menu_draw(self: bpy.types.TOPBAR_MT_blender, context: bpy.types.Context) -> None:
    self.layout.operator("http.demo_download_foreground")
    self.layout.operator("http.demo_download_background")
    self.layout.operator("http.demo_multiprocessing")


classes = (
    HTTP_OT_demo_download_foreground,
    HTTP_OT_demo_download_background,
    HTTP_OT_demo_multiprocessing,
)
_register, _unregister = bpy.utils.register_classes_factory(classes)


def register():
    _register()
    bpy.types.TOPBAR_MT_blender.append(topbar_blender_menu_draw)


def unregister():
    bpy.types.TOPBAR_MT_blender.remove(topbar_blender_menu_draw)
    _unregister()
