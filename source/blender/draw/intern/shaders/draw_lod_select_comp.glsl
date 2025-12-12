/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute visibility of each LOD based on distance.
 */

// !!!: WIP

layout(local_size_x = 64) in;

struct LodGPUInfo {
    float distances[MAX_LOD];
    int targets[MAX_LOD];
    int count;
};

layout(std430, binding = 0) readonly buffer lod_data {
    LodGPUInfo lod_info[];
};

layout(std430, binding = 1) writeonly buffer lod_output {
    int active_target[];
};

void main() {
    uint idx = gl_GlobalInvocationID.x;
    LodGPUInfo info = lod_info[idx];

    float dist = distance(camera_pos, object_centers[idx]);

    int selected = info.targets[info.count - 1];
    for (int i = 0; i < info.count; ++i) {
        if (dist < info.distances[i]) {
            selected = info.targets[i];
            break;
        }
    }

    active_target[idx] = selected;
}
