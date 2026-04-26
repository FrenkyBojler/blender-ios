/* SPDX-FileCopyrightText: 2026 The Khronos Group
*
* SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "decoder.h"
#include "meshoptimizer/meshoptimizer.h"

#define LOG_PREFIX "meshoptimizer | "

API(uint32_t)
decodeVertexBuffer(float *out, size_t index_count, size_t index_size, const unsigned char *data, size_t size){

    return meshopt_decodeVertexBuffer(out, index_count, index_size, data, size);

}

API(uint32_t)
decodeIndexBuffer(unsigned int *out, size_t index_count, size_t index_size, const unsigned char *data, size_t size){

    return meshopt_decodeIndexBuffer(out, index_count, index_size, data, size);

}

API(uint32_t)
decodeIndexSequence(unsigned int *out, size_t index_count, size_t index_size, const unsigned char *data, size_t size){

    return meshopt_decodeIndexSequence(out, index_count, index_size, data, size);

}

API(void)
decodeFilterOct(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterOct(buffer, count, stride);

}

API(void)
decodeFilterQuat(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterQuat(buffer, count, stride);

}

API(void)
decodeFilterExp(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterExp(buffer, count, stride);

}