/*
 * Minimal BufferWriter stub for Blender's GrTriangulator integration.
 * This provides just enough functionality to support CPU triangulation.
 */

#ifndef skgpu_BufferWriter_DEFINED
#define skgpu_BufferWriter_DEFINED

#include "include/core/SkPoint.h"
#include "include/private/base/SkAssert.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace skgpu {

struct BufferWriter {
public:
    struct Mark {
        Mark() : fMark(0) {}
        Mark(void* ptr, size_t offset = 0)
            : fMark(reinterpret_cast<uintptr_t>(ptr) + offset) {}

        bool operator<(const Mark& o) const { return fMark < o.fMark; }
        bool operator<=(const Mark& o) const { return fMark <= o.fMark; }
        bool operator==(const Mark& o) const { return fMark == o.fMark; }
        bool operator!=(const Mark& o) const { return fMark != o.fMark; }
        ptrdiff_t operator-(const Mark& o) const { return fMark - o.fMark; }
        explicit operator bool() const { return *this != Mark(); }
    private:
        uintptr_t fMark;
    };

    BufferWriter() = default;
    BufferWriter(BufferWriter&& w) { *this = std::move(w); }
    BufferWriter(void* ptr, size_t size) : fPtr(ptr), fEnd(Mark(ptr, ptr ? size : 0)) {}
    BufferWriter(void* ptr, Mark end) : fPtr(ptr), fEnd(end) {}

    BufferWriter& operator=(const BufferWriter&) = delete;
    BufferWriter& operator=(BufferWriter&& that) {
        fPtr = that.fPtr;
        that.fPtr = nullptr;
        fEnd = that.fEnd;
        that.fEnd = Mark();
        return *this;
    }

    explicit operator bool() const { return fPtr != nullptr; }

    Mark mark(size_t offset = 0) const {
        return Mark(fPtr, offset);
    }

protected:
    template<typename W>
    W makeOffset(size_t offsetInBytes) const {
        void* p = static_cast<char*>(fPtr) + offsetInBytes;
        Mark end = fEnd;
        return W{p, end};
    }

    void* fPtr = nullptr;
    Mark fEnd = {};
};

// VertexWriter for writing vertex data
struct VertexWriter : public BufferWriter {
    VertexWriter() = default;
    VertexWriter(void* ptr, size_t size) : BufferWriter(ptr, size) {}
    VertexWriter(void* ptr, Mark end) : BufferWriter(ptr, end) {}

    VertexWriter makeOffset(size_t offsetInBytes) const {
        return BufferWriter::makeOffset<VertexWriter>(offsetInBytes);
    }

    template <typename T>
    VertexWriter& operator<<(const T& val) {
        memcpy(fPtr, &val, sizeof(T));
        fPtr = static_cast<char*>(fPtr) + sizeof(T);
        return *this;
    }

    // For conditional coverage emission
    VertexWriter& operator<<(const std::tuple<uint8_t, bool>& coverageAndEmit) {
        if (std::get<1>(coverageAndEmit)) {
            uint8_t coverage = std::get<0>(coverageAndEmit);
            memcpy(fPtr, &coverage, sizeof(uint8_t));
            fPtr = static_cast<char*>(fPtr) + sizeof(uint8_t);
        }
        return *this;
    }
};

}  // namespace skgpu

#endif  // skgpu_BufferWriter_DEFINED
