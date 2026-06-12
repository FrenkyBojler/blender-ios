"""WebDAV virtual file system backend."""

import email.utils
import xml.etree.ElementTree as ET

import requests

_NS = {"d": "DAV:"}


class VFSWebDAV:
    def __init__(self, address: str):
        self.address = address

    def list_directory(self, path: str) -> list[tuple[str, bool, float, float, bool]]:
        """Return [(name, is_dir, size_bytes, mtime_epoch, is_hidden), ...] for the given virtual path.

        ``is_hidden`` is ``True`` when *name* starts with ``'.'``.

        :param path: Virtual directory path, e.g. ``/productions/my-project``.
        """
        url = f"http://{self.address}{path}"
        propfind = (
            '<?xml version="1.0" encoding="utf-8"?>'
            '<propfind xmlns="DAV:">'
            "   <prop>"
            "     <getcontentlength/>"
            "     <getlastmodified/>"
            "   </prop>"
            "</propfind>"
        )

        print(f"VFS WebDAV: PROPFIND {url}", flush=True)
        try:
            resp = requests.request(
                "PROPFIND",
                url,
                headers={"Depth": "1"},
                data=propfind.encode("utf-8"),
                timeout=30,
            )
            resp.raise_for_status()
        except BaseException:
            print(f"VFS WebDAV: request failed", flush=True)
            raise

        print(f"VFS WebDAV: status={resp.status_code}, body={resp.text[:500]}", flush=True)

        root = ET.fromstring(resp.text)
        entries: list[tuple[str, bool, float, float]] = []

        clean_path = path.rstrip("/")
        skipped_self = False

        for response in root.findall("d:response", _NS):
            href_el = response.find("d:href", _NS)
            if href_el is None:
                continue
            href = href_el.text.rstrip("/")

            # Extract the path portion (handle both "/path" and "http://host/path").
            if "://" in href:
                sep = href.index("://") + 3
                ps = href.find("/", sep)
                href_path = href[ps:] if ps >= 0 else "/"
            else:
                href_path = href

            # Skip only the first entry whose exact path matches the listed
            # directory (PROPFIND Depth:1 always includes the collection itself).
            if not skipped_self and clean_path and href_path == clean_path:
                skipped_self = True
                continue

            name = href.rsplit("/", 1)[-1]
            if not name:
                continue

            propstat = response.find("d:propstat", _NS)
            is_dir = True
            size = 0.0
            mtime = 0.0

            if propstat is not None:
                prop = propstat.find("d:prop", _NS)
                if prop is not None:
                    content_length_el = prop.find("d:getcontentlength", _NS)
                    if content_length_el is not None and content_length_el.text:
                        size = float(content_length_el.text)
                        is_dir = False

                    last_modified_el = prop.find("d:getlastmodified", _NS)
                    if last_modified_el is not None and last_modified_el.text:
                        parsed = email.utils.parsedate_to_datetime(
                            last_modified_el.text)
                        if parsed is not None:
                            mtime = parsed.timestamp()

            is_hidden = name.startswith(".")
            entries.append((name, is_dir, size, mtime, is_hidden))

        return entries

    def create_directory(self, path: str) -> bool:
        """Create a directory at the given virtual path.

        :param path: Virtual directory path to create, e.g. ``/productions/my-project``.
        :returns: True if successful, False otherwise.
        """
        url = f"http://{self.address}{path}"
        
        print(f"VFS WebDAV: MKCOL {url}", flush=True)
        try:
            resp = requests.request(
                "MKCOL",
                url,
                timeout=30,
            )
            resp.raise_for_status()
        except BaseException:
            print(f"VFS WebDAV: MKCOL request failed", flush=True)
            raise

        print(f"VFS WebDAV: MKCOL status={resp.status_code}", flush=True)
        return True

    def rename_item(self, src: str, dst: str) -> bool:
        """Rename an item from src to dst. Both must share the same parent VFSPath.
        
        :param src: Source virtual path.
        :param dst: Destination virtual path.
        :returns: True if successful, False otherwise.
        """
        src_url = f"http://{self.address}{src}"
        dst_url = f"http://{self.address}{dst}"
        
        print(f"VFS WebDAV: MOVE {src_url} -> {dst_url}", flush=True)
        try:
            resp = requests.request(
                "MOVE",
                src_url,
                headers={"Destination": dst_url},
                timeout=30,
            )
            resp.raise_for_status()
        except BaseException:
            print(f"VFS WebDAV: MOVE request failed", flush=True)
            raise

        print(f"VFS WebDAV: MOVE status={resp.status_code}", flush=True)
        return True
