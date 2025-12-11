# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def new_hasher(hash_algorithm, storage_path):
    from _bpy_internal.disk_file_hash_service import disk_file_hash_service

    return disk_file_hash_service.DiskFileHashService(
        hash_algorithm, storage_path
    )
