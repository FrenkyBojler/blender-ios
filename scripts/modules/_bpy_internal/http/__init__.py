# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import requests


def session() -> requests.Session:
    import urllib3.util.retry

    """Construct a requests.Session for HTTP requests."""
    http_retries = urllib3.util.retry.Retry(
        total=8,  # Times,
        backoff_factor=0.05,
    )
    http_adapter = requests.adapters.HTTPAdapter(max_retries=http_retries)
    http_session = requests.session()
    http_session.mount("https://", http_adapter)
    http_session.mount("http://", http_adapter)

    return http_session
