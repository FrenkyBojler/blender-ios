# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
blender -b --factory-startup -P tests/python/bl_http_downloader.py  -- output-dir /tmp/should-not-exist --verbose
"""


__all__ = (
    "main",
)

import unittest
from pathlib import Path
import shutil


class BasicImportTest(unittest.TestCase):
    """Just do a basic import and instantiation of classes.

    This doesn't test the functionality, but does ensure that dependencies like
    third-party libraries are available.
    """
    output_dir: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.output_dir = args.output_dir
        # exists_ok=False to prevent a rmtree() of existing directories on teardown.
        cls.output_dir.mkdir(exist_ok=False, parents=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.output_dir)

    def test_downloader(self) -> None:
        from _bpy_internal.http import downloader as http_dl

        metadata_provider = http_dl.MetadataProviderFilesystem(
            cache_location=self.output_dir / "http_metadata"
        )
        downloader = http_dl.ConditionalDownloader(metadata_provider=metadata_provider)
        self.assertIsNotNone(downloader)

    def test_background_downloader(self) -> None:
        from _bpy_internal.http import downloader as http_dl

        metadata_provider = http_dl.MetadataProviderFilesystem(
            cache_location=self.output_dir / "http_metadata"
        )
        options = http_dl.DownloaderOptions(
            metadata_provider=metadata_provider,
            timeout=1,
            http_headers={'X-Unit-Test': self.__class__.__name__}
        )

        def on_error(req_desc: http_dl.RequestDescription, local_path: Path, ex: Exception) -> None:
            self.fail(f"unexpected call to on_error({req_desc}, {local_path}, {ex})")

        downloader = http_dl.BackgroundDownloader(
            options=options,
            on_callback_error=on_error,
        )

        # Test some trivial properties that don't require anything running.
        self.assertTrue(downloader.all_downloads_done)
        self.assertEqual(0, downloader.num_pending_downloads)
        self.assertFalse(downloader.is_shutdown_requested)
        self.assertFalse(downloader.is_shutdown_complete)
        self.assertFalse(downloader.is_subprocess_alive)


class BackgroundDownloaderProcessTest(unittest.TestCase):
    """Start & stop the background process for the BackgroundDownloader.

    This doesn't test any HTTP requests, but does start & stop the background
    process to check that this is at least possible.
    """
    output_dir: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.output_dir = args.output_dir
        # exists_ok=False to prevent a rmtree() of existing directories on teardown.
        cls.output_dir.mkdir(exist_ok=False, parents=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.output_dir)

    def test_start_stop(self) -> None:
        from _bpy_internal.http import downloader as http_dl

        metadata_provider = http_dl.MetadataProviderFilesystem(
            cache_location=self.output_dir / "http_metadata"
        )
        options = http_dl.DownloaderOptions(
            metadata_provider=metadata_provider,
            timeout=1,
            http_headers={'X-Unit-Test': self.__class__.__name__}
        )

        def on_error(req_desc: http_dl.RequestDescription, local_path: Path, ex: Exception) -> None:
            self.fail(f"unexpected call to on_error({req_desc}, {local_path}, {ex})")

        downloader = http_dl.BackgroundDownloader(
            options=options,
            on_callback_error=on_error,
        )

        # Queueing a download before the downloader has started should be rejected.
        with self.assertRaises(RuntimeError):
            downloader.queue_download("https://example.com/", self.output_dir / "download.tmp")

        downloader.start()

        try:
            self.assertFalse(downloader.is_shutdown_requested)
            self.assertFalse(downloader.is_shutdown_complete)
            self.assertTrue(downloader.is_subprocess_alive)

            # For good measure, call the update function a few times to ensure that
            # any messages are sent. There shouldn't be any, but this should also
            # not be a problem.
            downloader.update()
            downloader.update()
            downloader.update()
        finally:
            # In case any of the pre-shutdown assertions fail, the downloader
            # should still be shut down.
            if downloader.is_subprocess_alive:
                downloader.shutdown()

        downloader.shutdown()

        self.assertTrue(downloader.is_shutdown_requested)
        self.assertTrue(downloader.is_shutdown_complete)
        self.assertFalse(downloader.is_subprocess_alive)


def main() -> None:
    global args
    import argparse
    import sys

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', required=True, type=Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
