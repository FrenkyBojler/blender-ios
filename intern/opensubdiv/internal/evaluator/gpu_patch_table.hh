#pragma once

#include "GPU_storage_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include <opensubdiv/version.h>

#include <opensubdiv/osd/nonCopyable.h>
#include <opensubdiv/osd/types.h>

using OpenSubdiv::Far::PatchTable;
using OpenSubdiv::Osd::NonCopyable;
using OpenSubdiv::Osd::PatchArrayVector;

namespace blender::opensubdiv {

// TODO: use Blenlib NonCopyable.
class GPUPatchTable : private NonCopyable<GPUPatchTable> {
 public:
  ~GPUPatchTable();

  static GPUPatchTable *Create(PatchTable const *farPatchTable, void *deviceContext = NULL);

  /// Returns the patch arrays for vertex index buffer data
  PatchArrayVector const &GetPatchArrays() const
  {
    return _patchArrays;
  }

  /// Returns the GL index buffer containing the patch control vertices
  gpu::VertBuf *GetPatchIndexBuffer() const
  {
    return _patchIndexBuffer;
  }

  /// Returns the GL index buffer containing the patch parameter
  gpu::VertBuf *GetPatchParamBuffer() const
  {
    return _patchParamBuffer;
  }

  /// Returns the GL texture buffer containing the patch control vertices
  gpu::VertBuf *GetPatchIndexTextureBuffer() const
  {
    return _patchIndexTexture;
  }

  /// Returns the GL texture buffer containing the patch parameter
  gpu::VertBuf *GetPatchParamTextureBuffer() const
  {
    return _patchParamTexture;
  }

  /// Returns the patch arrays for varying index buffer data
  PatchArrayVector const &GetVaryingPatchArrays() const
  {
    return _varyingPatchArrays;
  }

  /// Returns the GL index buffer containing the varying control vertices
  gpu::VertBuf *GetVaryingPatchIndexBuffer() const
  {
    return _varyingIndexBuffer;
  }

  /// Returns the GL texture buffer containing the varying control vertices
  gpu::VertBuf *GetVaryingPatchIndexTextureBuffer() const
  {
    return _varyingIndexTexture;
  }

  /// Returns the number of face-varying channel buffers
  int GetNumFVarChannels() const
  {
    return (int)_fvarPatchArrays.size();
  }

  /// Returns the patch arrays for face-varying index buffer data
  PatchArrayVector const &GetFVarPatchArrays(int fvarChannel = 0) const
  {
    return _fvarPatchArrays[fvarChannel];
  }

  /// Returns the GL index buffer containing face-varying control vertices
  gpu::VertBuf *GetFVarPatchIndexBuffer(int fvarChannel = 0) const
  {
    return _fvarIndexBuffers[fvarChannel];
  }

  /// Returns the GL texture buffer containing face-varying control vertices
  gpu::VertBuf *GetFVarPatchIndexTextureBuffer(int fvarChannel = 0) const
  {
    return _fvarIndexTextures[fvarChannel];
  }

  /// Returns the GL index buffer containing face-varying patch params
  gpu::VertBuf *GetFVarPatchParamBuffer(int fvarChannel = 0) const
  {
    return _fvarParamBuffers[fvarChannel];
  }

  /// Returns the GL texture buffer containing face-varying patch params
  gpu::VertBuf *GetFVarPatchParamTextureBuffer(int fvarChannel = 0) const
  {
    return _fvarParamTextures[fvarChannel];
  }

 protected:
  GPUPatchTable();

  // allocate buffers from patchTable
  bool allocate(PatchTable const *farPatchTable);

  PatchArrayVector _patchArrays;

  gpu::VertBuf *_patchIndexBuffer = nullptr;
  gpu::VertBuf *_patchParamBuffer = nullptr;

  gpu::VertBuf *_patchIndexTexture = nullptr;
  gpu::VertBuf *_patchParamTexture = nullptr;

  PatchArrayVector _varyingPatchArrays;
  gpu::VertBuf *_varyingIndexBuffer = nullptr;
  gpu::VertBuf *_varyingIndexTexture = nullptr;

  std::vector<PatchArrayVector> _fvarPatchArrays;
  std::vector<gpu::VertBuf *> _fvarIndexBuffers;
  std::vector<gpu::VertBuf *> _fvarIndexTextures;

  std::vector<gpu::VertBuf *> _fvarParamBuffers;
  std::vector<gpu::VertBuf *> _fvarParamTextures;
};

}  // namespace blender::opensubdiv
