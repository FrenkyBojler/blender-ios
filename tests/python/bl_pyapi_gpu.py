# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# blender -b --factory-startup --python tests/python/bl_pyapi_gpu.py

import unittest

import gpu


class TestGpuInit(unittest.TestCase):
    def test_gpu_module_is_usable_after_init(self):
        gpu.init()

        # GPU module functions will raise SystemError if the GPU has not been initialized.
        gpu.platform.vendor_get()
        gpu.platform.renderer_get()
        gpu.platform.version_get()
        gpu.platform.device_type_get()
        gpu.types.GPUTexture(size=(1024, 1024), format="RGBA8")


class TestGpuFrameBuffer(unittest.TestCase):
    def test_read_color_bounds_check(self):
        gpu.init()

        # Setup a framebuffer.
        tex = gpu.types.GPUTexture(size=(16, 16), format="RGBA8")
        fb = gpu.types.GPUFrameBuffer(color_slots=[tex])

        with fb.bind():
            # Reading within bounds should succeed.
            buf = fb.read_color(0, 0, 1, 1, 4, 0, "UBYTE")

            # Reading outside bounds should raise ValueError.
            with self.assertRaises(ValueError):
                fb.read_color(-1, 0, 1, 1, 4, 0, "UBYTE")


class TestGpuStorageBuf(unittest.TestCase):
    def test_create_update_read(self):
        import struct

        gpu.init()

        data = struct.pack("4f", 1.0, 2.0, 3.0, 4.0)
        ssbo = gpu.types.GPUStorageBuf(data)

        self.assertEqual(struct.unpack("4f", bytes(ssbo.read())), (1.0, 2.0, 3.0, 4.0))

        new_data = struct.pack("4f", 10.0, 20.0, 30.0, 40.0)
        ssbo.update(new_data)
        self.assertEqual(struct.unpack("4f", bytes(ssbo.read())), (10.0, 20.0, 30.0, 40.0))

        ssbo.clear_to_zero()
        self.assertEqual(struct.unpack("4f", bytes(ssbo.read())), (0.0, 0.0, 0.0, 0.0))

        # Should not raise.
        ssbo.sync_to_host()

    def test_compute_shader_binding(self):
        import struct

        gpu.init()

        info = gpu.types.GPUShaderCreateInfo()
        info.storage_buf(0, {"READ", "WRITE"}, "float", "data[]")
        info.local_group_size(4)
        info.compute_source(
            """
            void main() {
              uint i = gl_GlobalInvocationID.x;
              data[i] = data[i] * 2.0;
            }
            """
        )

        shader = gpu.shader.create_from_info(info)
        ssbo = gpu.types.GPUStorageBuf(struct.pack("4f", 1.0, 2.0, 3.0, 4.0))

        shader.bind()
        shader.storage_block("data", ssbo)
        gpu.compute.dispatch(shader, 1, 1, 1)

        self.assertEqual(struct.unpack("4f", bytes(ssbo.read())), (2.0, 4.0, 6.0, 8.0))


if __name__ == "__main__":
    import sys

    sys.argv = [__file__] + (
        sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    )

    unittest.main()
