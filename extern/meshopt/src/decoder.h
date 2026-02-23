/* SPDX-FileCopyrightText: 2026 The Khronos Group
*
* SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "common.h"

API(uint32_t)
decodeVertexBuffer(float *out, size_t n, size_t stride, const void *data, size_t size);

API(uint32_t)
decodeIndexBuffer(uint32_t *out, size_t n, const void *data, size_t size);

API(uint32_t)
decodeIndexSequence(uint32_t *out, size_t n, const void *data, size_t size);

API(void)
decodeFilterOct(float *out, size_t n, size_t stride, const void *data, size_t size);

API(void)
decodeFilterQuat(float *out, size_t n, size_t stride, const void *data, size_t size);

API(void)
decodeFilterExp(float *out, size_t n, size_t stride, const void *data, size_t size);