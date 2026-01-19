/*
 * Minimal GrEagerVertexAllocator for Blender's GrTriangulator integration.
 * This provides CPU-only vertex allocation without GPU dependencies.
 */

#ifndef GrEagerVertexAllocator_DEFINED
#define GrEagerVertexAllocator_DEFINED

#include "src/gpu/BufferWriter.h"
#include <cstddef>
#include <cstdlib>

// Interface for allocating vertex data
class GrEagerVertexAllocator {
public:
    virtual void* lock(size_t stride, int eagerCount) = 0;
    virtual void unlock(int actualCount) = 0;
    virtual ~GrEagerVertexAllocator() {}

    skgpu::VertexWriter lockWriter(size_t stride, int eagerCount) {
        void* p = this->lock(stride, eagerCount);
        return p ? skgpu::VertexWriter{p, stride * eagerCount} : skgpu::VertexWriter{};
    }
};

// Simple CPU allocator using malloc
class GrCpuVertexAllocator : public GrEagerVertexAllocator {
public:
    GrCpuVertexAllocator() = default;
    ~GrCpuVertexAllocator() override {
        if (fVertices) {
            free(fVertices);
        }
    }

    void* lock(size_t stride, int eagerCount) override {
        fLockStride = stride;
        fVertices = malloc(stride * eagerCount);
        return fVertices;
    }

    void unlock(int actualCount) override {
        fActualCount = actualCount;
        // Optionally resize - for simplicity we keep the original allocation
    }

    void* vertices() const { return fVertices; }
    int vertexCount() const { return fActualCount; }
    size_t stride() const { return fLockStride; }

private:
    void* fVertices = nullptr;
    size_t fLockStride = 0;
    int fActualCount = 0;
};

#endif  // GrEagerVertexAllocator_DEFINED
