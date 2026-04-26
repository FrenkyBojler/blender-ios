/* SPDX-FileCopyrightText: 2026 The Khronos Group
*
* SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "common.h"

API(uint32_t)
encodeVertexBuffer(void *out, size_t n, const void* vertices, size_t vertex_count, size_t vertex_size);

API(uint32_t)
encodeIndexBuffer(void *out, size_t n, const unsigned int* indices, size_t size);

API(uint32_t)
encodeIndexSequence(void *out, size_t n, const unsigned int* indices, size_t size);