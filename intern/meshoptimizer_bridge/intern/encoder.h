/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "common.h"

#include <stddef.h>

API(void)
encodeIndexVersion(int version);

API(void)
encodeVertexVersion(int version);

API(size_t)
encodeIndexBufferBound(size_t index_count, size_t vertex_count);

API(size_t)
encodeIndexBuffer(unsigned char* buffer, size_t buffer_size, const unsigned int* indices, size_t index_count);

API(size_t)
encodeVertexBufferBound(size_t vertex_count, size_t vertex_size);

API(size_t)
encodeVertexBuffer(unsigned char* buffer, size_t buffer_size, const void* vertices, size_t vertex_count, size_t vertex_size);

API(size_t)
encodeIndexSequence(unsigned char* buffer, size_t buffer_size, const unsigned int* indices, size_t index_count);