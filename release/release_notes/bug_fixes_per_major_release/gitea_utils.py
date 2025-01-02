# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import time
import urllib.error
import urllib.request

# Conform to Blenders crawl delay request
# https://projects.blender.org/robots.txt
crawl_delay = 2
last_checked_time = None

def url_json_get(url):
    global last_checked_time

    if last_checked_time is not None:
        time.sleep(max(crawl_delay - (time.time() - last_checked_time), 0))
    last_checked_time = time.time()

    try:
        # Make the HTTP request and store the response in a 'response' object
        response = urllib.request.urlopen(url)
    except urllib.error.URLError as ex:
        print(url)
        print(f"Error making HTTP request: {ex}")
        return None

    # Convert the response content to a JSON object containing the user information.
    result = json.loads(response.read())
    assert result is None or isinstance(result, (dict, list))
    return result
