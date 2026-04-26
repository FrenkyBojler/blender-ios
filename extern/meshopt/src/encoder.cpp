/* SPDX-FileCopyrightText: 2026 The Khronos Group
*
* SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "meshoptimizer/meshoptimizer.h"

#define LOG_PREFIX "meshoptimizer | "

int encodeVertexBuffer(unsigned char *out, size_t n, const void* vertices, size_t vertex_count, size_t vertex_size){

    return meshopt_encodeVertexBuffer(out, n, vertices, vertex_count, vertex_size);

}

int encodeIndexBuffer(unsigned char *out, size_t n, const unsigned int* indices, size_t size){

    return meshopt_encodeIndexBuffer(out, n, indices, size);

}

int encodeIndexSequence(unsigned char *out, size_t n, const unsigned int *indices, size_t size){

    return meshopt_encodeIndexSequence(out, n, indices, size);

}

