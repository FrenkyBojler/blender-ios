/* SPDX-FileCopyrightText: 2026 The Khronos Group
*
* SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "meshoptimizer/meshoptimizer.h"

#define LOG_PREFIX "meshoptimizer | "

int decodeVertexBuffer(float *out, size_t n, size_t stride, const unsigned char *data, size_t size){

    return meshopt_decodeVertexBuffer(out, n, stride, data, size);

}

int decodeIndexBuffer(unsigned int *out, size_t n, const unsigned char *data, size_t size){

    return meshopt_decodeIndexBuffer(out, n, data, size);

}

int decodeIndexSequence(unsigned int *out, size_t n, const unsigned char *data, size_t size){

    return meshopt_decodeIndexSequence(out, n, data, size);

}

void decodeFilterOct(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterOct(buffer, count, stride);

}

void decodeFilterQuat(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterQuat(buffer, count, stride);

}

void decodeFilterExp(void* buffer, size_t count, size_t stride){

    meshopt_decodeFilterExp(buffer, count, stride);

}