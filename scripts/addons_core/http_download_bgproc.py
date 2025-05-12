import queue
import multiprocessing
import multiprocessing.connection

# To work around:
# mypy   : Variable "multiprocessing.Event" is not valid as a type
#          note: See https://mypy.readthedocs.io/en/stable/common_issues.html#variables-vs-type-aliases
# Pylance: Variable not allowed in type expression
from multiprocessing.synchronize import Event as EventClass


def background_task(
    conn: multiprocessing.connection.Connection,
        download_queue: multiprocessing.Queue,
        shutdown: EventClass,
) -> None:
    conn.send(f"Hello from {__file__}!")

    while not shutdown.is_set():
        try:
            to_download = download_queue.get(timeout=0.1)
        except queue.Empty:
            pass
        else:
            conn.send(f"Downloading: {to_download}")

    conn.send("Shutting down")
    conn.close()
