# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Remote Python REPL over a TCP socket, for driving Blender during development.

Local dev scaffolding for the SculptCore project -- not part of the upstream PR.

Why this exists: `bpy` is not thread-safe and must only be touched from
Blender's main thread. A socket server therefore cannot exec incoming code on
its own accept/receive threads. This module accepts connections on a background
thread, hands each command to a `bpy.app.timers` callback that runs on the main
thread, and ships the captured output back to the client.

Usage -- as a startup script (simplest):

    blender.exe --python claudeMemory/scripts/remote_repl.py

Usage -- as an addon: point Blender's script directory at `claudeMemory` (or
symlink this file into an addons directory) and enable "Development: Remote
Python REPL" in Preferences.

Then talk to it (persistent connection, commands are NUL-terminated):

    python claudeMemory/scripts/remote_repl.py --client
    # or from any tool:  send "<code>\\x00", read reply until "\\x00".

Configuration via environment variables (read at register time):
    BLENDER_REPL_HOST     bind address           (default 127.0.0.1)
    BLENDER_REPL_PORT     bind port              (default 4444)
    BLENDER_DEBUGPY_PORT  if set, start debugpy.listen() on this port so a
                          Python debugger (e.g. VS Code) can attach.

Caveat: main-thread dispatch runs from the timer, which fires while Blender's
event loop spins. In the GUI this is continuous; in pure `-b` batch mode with a
blocking script the loop may not spin, so commands would not drain.
"""

bl_info = {
    "name": "Remote Python REPL",
    "author": "SculptCore",
    "version": (1, 0, 0),
    "blender": (5, 3, 0),
    "location": "N/A (background TCP server)",
    "description": "Drive Blender's Python API over a TCP socket, main-thread safe.",
    "category": "Development",
}

import os
import queue
import socket
import threading

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4444
FRAME_DELIMITER = b"\x00"
DRAIN_INTERVAL = 0.05

# Jobs pushed by connection threads, drained on the main thread by the timer.
# Each item is (code_str, response_queue); the worker puts the output string
# on `response_queue` once it has run the code.
_job_queue: "queue.Queue" = queue.Queue()

# Namespace shared across all remote commands, so state persists between them.
_repl_globals: dict = {}

_server_socket = None
_server_thread = None
_running = threading.Event()


def _run_code(code):
    """Execute `code` on the main thread and return captured stdout/stderr.

    A single expression is evaluated and its repr printed; anything else is
    executed as statements. Exceptions are formatted into the returned text
    rather than raised, so the client always gets a reply.
    """
    import contextlib
    import io
    import traceback

    buffer = io.StringIO()
    try:
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(buffer):
            try:
                compiled = compile(code, "<remote>", "eval")
            except SyntaxError:
                exec(compile(code, "<remote>", "exec"), _repl_globals)
            else:
                result = eval(compiled, _repl_globals)
                if result is not None:
                    print(repr(result))
    except Exception:
        buffer.write(traceback.format_exc())
    return buffer.getvalue()


def _drain_jobs():
    """Timer callback: run every queued command on the main thread.

    Returns the re-arm interval so Blender keeps calling it.
    """
    while True:
        try:
            code, response_queue = _job_queue.get_nowait()
        except queue.Empty:
            break
        response_queue.put(_run_code(code))
    return DRAIN_INTERVAL


def _handle_connection(connection, address):
    """Read NUL-delimited commands from one client until it disconnects."""
    connection.settimeout(1.0)
    pending = b""
    with connection:
        while _running.is_set():
            try:
                chunk = connection.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            pending += chunk
            while FRAME_DELIMITER in pending:
                raw, pending = pending.split(FRAME_DELIMITER, 1)
                code = raw.decode("utf-8", "replace")
                response_queue: "queue.Queue" = queue.Queue(maxsize=1)
                _job_queue.put((code, response_queue))
                output = response_queue.get()  # Blocks until the main thread runs it.
                try:
                    connection.sendall(output.encode("utf-8") + FRAME_DELIMITER)
                except OSError:
                    return


def _serve(host, port):
    """Accept loop: one handler thread per client. Runs off the main thread."""
    global _server_socket
    _server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    _server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    _server_socket.bind((host, port))
    _server_socket.listen(4)
    _server_socket.settimeout(1.0)
    print("[remote_repl] listening on {:s}:{:d}".format(host, port))
    while _running.is_set():
        try:
            connection, address = _server_socket.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        threading.Thread(
            target=_handle_connection, args=(connection, address), daemon=True
        ).start()


def _maybe_start_debugpy():
    """Start a debugpy listener if BLENDER_DEBUGPY_PORT is set."""
    port_text = os.environ.get("BLENDER_DEBUGPY_PORT")
    if not port_text:
        return
    try:
        import debugpy
    except ImportError:
        print("[remote_repl] BLENDER_DEBUGPY_PORT set but debugpy is not installed.")
        return
    debugpy.listen(("127.0.0.1", int(port_text)))
    print("[remote_repl] debugpy listening on 127.0.0.1:{:s}".format(port_text))


def register():
    import bpy

    if _running.is_set():
        return
    _maybe_start_debugpy()
    host = os.environ.get("BLENDER_REPL_HOST", DEFAULT_HOST)
    port = int(os.environ.get("BLENDER_REPL_PORT", DEFAULT_PORT))

    global _server_thread
    _running.set()
    _server_thread = threading.Thread(target=_serve, args=(host, port), daemon=True)
    _server_thread.start()
    bpy.app.timers.register(_drain_jobs, persistent=True)


def unregister():
    import bpy

    _running.clear()
    if bpy.app.timers.is_registered(_drain_jobs):
        bpy.app.timers.unregister(_drain_jobs)
    global _server_socket
    if _server_socket is not None:
        try:
            _server_socket.close()
        except OSError:
            pass
        _server_socket = None
    print("[remote_repl] stopped")


def _run_client():
    """Minimal interactive client, so this file doubles as a test tool."""
    import sys

    host = os.environ.get("BLENDER_REPL_HOST", DEFAULT_HOST)
    port = int(os.environ.get("BLENDER_REPL_PORT", DEFAULT_PORT))
    connection = socket.create_connection((host, port))
    print("Connected to {:s}:{:d}. Type code; blank line sends. Ctrl-C to quit.".format(host, port))
    try:
        while True:
            lines = []
            while True:
                line = input("... " if lines else ">>> ")
                if line == "":
                    break
                lines.append(line)
            if not lines:
                continue
            connection.sendall("\n".join(lines).encode("utf-8") + FRAME_DELIMITER)
            reply = b""
            while FRAME_DELIMITER not in reply:
                reply += connection.recv(4096)
            text = reply.split(FRAME_DELIMITER, 1)[0].decode("utf-8", "replace")
            if text:
                sys.stdout.write(text)
                if not text.endswith("\n"):
                    sys.stdout.write("\n")
    except (KeyboardInterrupt, EOFError):
        connection.close()
        print("\nbye")


if __name__ == "__main__":
    import sys

    if "--client" in sys.argv:
        _run_client()
    else:
        register()
