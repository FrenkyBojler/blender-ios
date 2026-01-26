# SPDX-FileCopyrightText: 2021-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

if "bpy" in locals():
    import importlib
    importlib.reload(action_registry)
else:
    from . import action_registry

from .action_profile import (
    VRDefaultActionmaps,
)


def vr_ensure_default_actionmaps(session_state):
    return action_registry.registry.ensure_actionmaps(session_state)
