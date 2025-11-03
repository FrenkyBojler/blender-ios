# SPDX-FileCopyrightText: 2025 Blender Authors & Khronos Group contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later
import pathlib
import sys
import unittest

import bpy
from os.path import join, basename, dirname, isfile, normpath
from os import remove, listdir
import struct
import json
import numpy as np
import base64
from urllib.parse import unquote
from enum import IntEnum

sys.path.append(str(pathlib.Path(__file__).parent.absolute()))

args = None


def gltf_generate_descr(output_datafile: pathlib.Path) -> str:
    gltf = glTFDataExtractor(str(output_datafile))
    gltf.load()

    text = ""
    text += str(gltf.magic) + "\n"
    text += str(gltf.version) + "\n"
    text += str(gltf.file_size) + "\n"
    text += json.dumps(gltf.json, indent=2, ensure_ascii=False)
    for accessor in gltf.accessors_data:
        text += accessor + "\n"
    return text


def do_gltf_roundtrip(filepath, params_import, params_export):
    bpy.ops.import_scene.gltf(filepath=filepath, **params_import)
    bpy.ops.export_scene.gltf(
        filepath=join(
            join(
                dirname(filepath),
                "out"),
            basename(filepath)),
        **params_export)


class GLTFImportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir
        cls.output_dir = args.outdir

    def test_roundtrip_gltf(self):
        input_files = sorted(pathlib.Path(self.testdir).glob("*.gltf"))
        self.passed_tests = []
        self.failed_tests = []
        self.updated_tests = []

        from modules import io_report
        report = io_report.Report(
            "glTF Roundtrip",
            self.output_dir,
            self.testdir,
            self.testdir.joinpath("reference"),
            gltf_generate_descr)

        for input_file in input_files:
            with self.subTest(pathlib.Path(input_file).stem):
                bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "../../empty.blend"))
                ok = report.generate_and_check(
                    input_file,
                    lambda filepath,
                    params_import,
                    params_export: do_gltf_roundtrip(
                        filepath,
                        params_import,
                        params_export),
                    roundtrip=True)
                if not ok:
                    self.fail(f"{input_file.stem} import result does not match expectations")

        # Now that all tests have been run
        # Let's empty the output directory of the generated files
        # so that we can make sure that the next run will generate them all again.
        for f in listdir(join(self.testdir, "out")):
            if isfile(join(join(self.testdir, "out"), f)) and f != "README.md":  # keep the README
                remove(join(join(self.testdir, "out"), f))

        report.finish("io_gltf_roundtrip")

# This is a simple glTF loader to extract the JSON content and buffer from a GLB file.
# Based on  KhronosGroup/glTf-Blender-IO


class glTFDataExtractor:
    def __init__(self, filepath):
        self.filepath = filepath
        self.buffers = []
        self.json = None
        self.magic = None
        self.version = None
        self.file_size = None
        self.accessors_data = []

    def load(self):
        if not isfile(self.filepath):
            raise FileNotFoundError(f"File not found: {self.filepath}")

        with open(self.filepath, 'rb') as f:
            content = memoryview(f.read())

        if content[:4] == b'glTF':
            # glb
            self.load_glb(content)
        else:
            # glTF + bin + textures
            self.json = glTFDataExtractor.load_json(content)

            # Get buffers
            for buffer in self.json.get('buffers', []):
                uri = buffer.get('uri', '')
                sep = ';base64,'
                if uri.startswith('data:'):
                    idx = uri.find(sep)
                    if idx != -1:
                        data = uri[idx + len(sep):]
                        self.buffers.append(memoryview(base64.b64decode(data)))
                else:
                    # External .bin file
                    bin_path = join(dirname(self.filepath), uri_to_path(uri))
                    with open(bin_path, 'rb') as bf:
                        self.buffers.append(memoryview(bf.read()))

        # Loop on accessors to extract data
        for accessor in self.json.get('accessors', []):
            buffer_view_index = accessor.get('bufferView')
            if buffer_view_index is not None:
                buffer_view = self.json['bufferViews'][buffer_view_index]
                buffer_index = buffer_view['buffer']
                buffer_data = self.buffers[buffer_index]

                byte_offset = buffer_view.get('byteOffset', 0) + accessor.get('byteOffset', 0)

                data = buffer_data[byte_offset: byte_offset + buffer_view['byteLength']]

                # MAT2/3 have special alignment requirements that aren't handled. But it
                # doesn't matter because nothing uses them.
                assert accessor.get('type') not in ['MAT2', 'MAT3']

                dtype = ComponentType.to_numpy_dtype(accessor.get('componentType'))
                component_nb = DataType.num_elements(accessor.get('type'))

                bytes_per_elem = dtype(1).nbytes
                default_stride = bytes_per_elem * component_nb
                stride = buffer_view.get('byteStride', default_stride)
                if stride == default_stride:
                    array = np.frombuffer(
                        data,
                        dtype=np.dtype(dtype).newbyteorder('<'),
                        count=accessor.get('count') * component_nb,
                    )
                    array = array.reshape(accessor.get('count'), component_nb)
                else:
                    # The data looks like
                    #   XXXppXXXppXXXppXXX
                    # where X are the components and p are padding.
                    # One XXXpp group is one stride's worth of data.
                    assert stride % bytes_per_elem == 0
                    elems_per_stride = stride // bytes_per_elem
                    num_elems = (accessor.get(count) - 1) * elems_per_stride + component_nb
                    array = np.frombuffer(
                        buffer_data,
                        dtype=np.dtype(dtype).newbyteorder('<'),
                        count=num_elems,
                    )
                    assert array.strides[0] == bytes_per_elem
                    array = np.lib.stride_tricks.as_strided(
                        array,
                        shape=(accessor.count, component_nb),
                        strides=(stride, bytes_per_elem),
                    )

                    # TODO manage sparse accessors
                    # (currently not used in Blender roudntrip tests)
            else:
                # Need to init data with zeros
                dtype = ComponentType.to_numpy_dtype(accessor.get('componentType'))
                component_nb = DataType.num_elements(accessor.get('type'))
                array = np.zeros((accessor.get('count'), component_nb), dtype=dtype)

                # TODO manage sparse accessors
                # (currently not used in Blender roudntrip tests)

            # Normalization
            if accessor.get('normalized'):
                if accessor.get('componentType') == 5120:       # int8
                    array = np.maximum(-1.0, array / 127.0)
                elif accessor.get('componentType') == 5121:     # uint8
                    array = array / 255.0
                elif accessor.get('componentType') == 5122:     # int16
                    array = np.maximum(-1.0, array / 32767.0)
                elif accessor.get('componentType') == 5123:     # uint16
                    array = array / 65535.0

                array = array.astype(np.float32, copy=False)

            self.accessors_data.append(np.array2string(array, formatter={'float_kind': lambda x: convert_float(x)}))

    def load_glb(self, content):
        self.magic = content[:4]
        self.version, self.file_size = struct.unpack_from('<II', content, offset=4)
        if self.version != 2:
            raise ImportError("GLB version must be 2; got %d" % version)
        if self.file_size != len(content):
            raise ImportError("Bad GLB: file size doesn't match")

        offset = 12  # header size = 12
        type_, len_, json_bytes, offset = self.load_chunk(content, offset)
        if type_ != b"JSON":
            raise ImportError("Bad GLB: first chunk not JSON")
        if len_ != len(json_bytes):
            raise ImportError("Bad GLB: length of json chunk doesn't match")
        self.json = glTFDataExtractor.load_json(json_bytes)

        # BIN chunk(s) is second (if exists)
        if offset < len(content):
            type_, len_, bin_bytes, offset = self.load_chunk(content, offset)
            if type_ != b"BIN\x00":
                raise ImportError("Bad GLB: second chunk not BIN")
            if len_ != len(bin_bytes):
                raise ImportError("Bad GLB: length of bin chunk doesn't match")
            self.buffers.append(bin_bytes)

    def load_chunk(self, content, offset):
        chunk_header = struct.unpack_from('<I4s', content, offset)
        data_length = chunk_header[0]
        data_type = chunk_header[1]
        data = content[offset + 8: offset + 8 + data_length]

        return data_type, data_length, data, offset + 8 + data_length

    @staticmethod
    def load_json(content):
        def bad_constant(val):
            raise ImportError('Bad glTF: json contained %s' % val)
        try:
            text = str(content, encoding='utf-8')
            return json.loads(text, parse_constant=bad_constant)
        except ValueError as e:
            raise ImportError('Bad glTF: json error: %s' % e.args[0])


class ComponentType(IntEnum):
    Byte = 5120
    UnsignedByte = 5121
    Short = 5122
    UnsignedShort = 5123
    UnsignedInt = 5125
    Float = 5126

    @classmethod
    def to_numpy_dtype(cls, component_type):
        import numpy as np
        return {
            ComponentType.Byte: np.int8,
            ComponentType.UnsignedByte: np.uint8,
            ComponentType.Short: np.int16,
            ComponentType.UnsignedShort: np.uint16,
            ComponentType.UnsignedInt: np.uint32,
            ComponentType.Float: np.float32,
        }[component_type]


class DataType:
    Scalar = "SCALAR"
    Vec2 = "VEC2"
    Vec3 = "VEC3"
    Vec4 = "VEC4"
    Mat2 = "MAT2"
    Mat3 = "MAT3"
    Mat4 = "MAT4"

    def __new__(cls, *args, **kwargs):
        raise RuntimeError("{} should not be instantiated".format(cls.__name__))

    @classmethod
    def num_elements(cls, data_type):
        return {
            DataType.Scalar: 1,
            DataType.Vec2: 2,
            DataType.Vec3: 3,
            DataType.Vec4: 4,
            DataType.Mat2: 4,
            DataType.Mat3: 9,
            DataType.Mat4: 16
        }[data_type]


def uri_to_path(uri):
    uri = uri.replace('\\', '/')  # Some files come with \\ as dir separator
    uri = unquote(uri)
    return normpath(uri)


def convert_float(x):
    if abs(x) < 0.0005:
        return "0.000"
    return f"{x:.3f}"


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    parser.add_argument('--outdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
