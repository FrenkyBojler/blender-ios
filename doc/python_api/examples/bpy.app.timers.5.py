"""
Use a Timer to react to events in another thread
------------------------------------------------

You should never modify Blender data at arbitrary points in time in separate threads.
However you can use a queue to collect all the actions that should be executed when Blender is in the right state again.

`bpy.app.timers.register()` is thread-safe so it can be used to trigger a callback from the main thread.
"""
import bpy
import threading


def on_main_thread():
    bpy.context.object.show_name = True
    return None


def background_thread():
    # We can't access (or modify) blender data from this background thread.
    # So instead, we register a timer (which only gets invoked once) to
    # arrange for some code to be executed on the main thread (at a later point,
    # where it _is_ safe to access scene data).
    bpy.app.timers.register(on_main_thread)


threading.Thread(target=background_thread).start()
