# DNA / RNA Data System

Blender's data model is split into two layers: **DNA** (the serialized C
struct layout) and **RNA** (the introspection/reflection layer that exposes
DNA — and dynamic data — to Python and the UI). Understanding this split is
required before touching any persisted data structure. See also the
"`.blend` File Compatibility" section in [CLAUDE.md](../CLAUDE.md).

---

## 1. DNA — `source/blender/makesdna/`

DNA structs are plain C structs declared in headers such as
`source/blender/makesdna/DNA_object_types.h:466` (`struct Object { ... }`).
These define Blender's on-disk and in-memory data layout and follow strict
rules (no `#ifdef`, no function pointers used for data) because they must
serialize byte-for-byte into `.blend` files.

### The `makesdna` build-time tool

`source/blender/makesdna/intern/makesdna.cc` is a build-time program that
parses the `DNA_*_types.h` headers with its **own simplified lexer** (not a
full C parser — see the doc comment "About makesdna tool" near line 11). The
core parser is `make_structDNA()` (`makesdna.cc:863`), invoked from `main()`
(`makesdna.cc:952`; it runs the pass twice — once to build, once to verify,
see `makesdna.cc:1040`).

The output is a compact **SDNA table** (struct/field names + types) compiled
into `dna.c` and embedded in every `.blend` file. This is what lets Blender
read files written by older or newer versions: field offsets and sizes are
computed at load time from this metadata rather than hardcoded, so even if
the current C struct layout has changed, the file's own embedded SDNA tells
the loader how to interpret its bytes.

### DNA-level versioning

Raw struct-field patching (renames, reordering, defaults for new fields)
happens in `source/blender/blenloader/intern/versioning_dna.cc`, invoked as
`blo_do_versions_dna(fd->filesdna, fd->fileversion, subversion)` from
`source/blender/blenloader/intern/readfile.cc:897`.

---

## 2. RNA — `source/blender/makesrna/`

### Static definitions

RNA structs/properties are declared imperatively in `rna_*.c`/`.cc` files,
e.g. `source/blender/makesrna/intern/rna_object.cc`:

```c
RNA_def_struct(brna, "VertexGroup", nullptr);                       // rna_object.cc:2333
RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);             // rna_object.cc:2339
```

The DNA linkage is explicit:
- `RNA_def_property_sdna(prop, ...)` binds a property to a DNA struct
  member name/offset.
- `RNA_def_property_struct_type(prop, "Material")` (`rna_object.cc:2417`)
  links a pointer/collection property to another RNA struct.

### Core types (`source/blender/makesrna/intern/rna_internal_types.hh`)

- `PropertyRNA` (line 368) — identifier, flags, type, and (for DNA-backed
  properties) offset/struct-name info used to read/write the underlying DNA
  field via pointer arithmetic. For non-DNA (`IDProperty`-backed) properties
  it instead carries get/set callbacks.
- `StructRNA` (line 659) — the struct-level descriptor (name, properties,
  functions, base type for inheritance).

### The code generator

`makesrna.cc` (`main()` at `source/blender/makesrna/intern/makesrna.cc:4378`)
runs at **build time**: it calls every `rna_def_*` registration function to
build the in-memory RNA database, then emits `rna_*_gen.cc` files containing
generated getter/setter/iterator glue and the final `RNA_*` C API tables.
These generated files are what the rest of Blender — including the Python
layer — links against.

---

## 3. Python `bpy.types.*` Generation

`source/blender/python/intern/bpy_rna.cc` wraps each `StructRNA` into a
Python type object **dynamically at runtime** (not code-generated) —
`bpy.types.Object`, `bpy.types.Mesh`, etc. are built directly from the RNA
struct database.

For user-defined subclasses (an add-on's `Operator`/`Panel`/`PropertyGroup`),
`pyrna_register_class()` (exposed as `bpy.utils.register_class`,
`bpy_rna.cc:10367`) creates a **new dynamic `StructRNA`** extending the
Python class's RNA base and installs Python-callback-based function
pointers. Validation of the class's overridden methods/properties happens
recursively in `bpy_class_validate_recursive()` (`bpy_rna.cc:9544`), invoked
via `bpy_class_validate()` (`bpy_rna.cc:9776`), which is wired in as the RNA
`validate` callback during registration (~`bpy_rna.cc:10491`).

See [python-bpy-integration.md](./python-bpy-integration.md) for the full
registration flow (add-ons, startup scripts, operator/tool registration).

---

## 4. `.blend` Save/Load and Versioning

`BLENDER_FILE_VERSION` is `#define`d as `BLENDER_VERSION` in
`source/blender/blenkernel/BKE_blender_version.h:32`.

On load (`source/blender/blenloader/intern/readfile.cc`):

1. **Raw DNA-struct patching** — `blo_do_versions_dna()` (`readfile.cc:897`)
   applies field-level fixups using the file's own embedded SDNA table.
2. **Higher-level data migration** — `do_versions_after_linking()` and the
   numbered `versioning_XXX.cc` files (e.g.
   `source/blender/blenloader/intern/versioning_400.cc`) run semantic
   conversions, gated by checks like
   `MAIN_VERSION_FILE_ATLEAST(bmain, 400, 9)` (`versioning_400.cc:323`).
   This is where deprecated DNA gets converted into current data structures.

This two-level scheme (raw SDNA remap, then semantic `do_versions`) is what
lets old files open correctly even after structs change — see the
`.blend` compatibility rules in [CLAUDE.md](../CLAUDE.md) before modifying
any DNA struct.

---

## 5. `IDProperty` — Dynamic / Custom Properties

Data that isn't backed by a compiled-in DNA field (Python custom properties,
add-on data) is stored as `IDProperty` trees attached to an ID. RNA bridges
to these dynamically:

- `rna_idproperty_find()` and `rna_idproperty_check()`
  (`source/blender/makesrna/intern/rna_access.cc:310` and `:616`) look up an
  `IDProperty` matching a given `PropertyRNA` identifier when no static
  DNA-backed property exists.

This lets `RNA_property_*` API calls transparently read/write either
compiled-in DNA fields or ad-hoc `IDProperty` values through the same
interface — the caller doesn't need to know which backing store a given
property uses.

---

## Key File:Line Reference Table

| Topic | File | Line |
|---|---|---|
| Example DNA struct (`Object`) | `source/blender/makesdna/DNA_object_types.h` | 466 |
| `make_structDNA()` | `source/blender/makesdna/intern/makesdna.cc` | 863 |
| makesdna `main()` | `source/blender/makesdna/intern/makesdna.cc` | 952 |
| `blo_do_versions_dna()` call site | `source/blender/blenloader/intern/readfile.cc` | 897 |
| Example RNA struct def | `source/blender/makesrna/intern/rna_object.cc` | 2333 |
| Example RNA property def | `source/blender/makesrna/intern/rna_object.cc` | 2339 |
| `RNA_def_property_struct_type` example | `source/blender/makesrna/intern/rna_object.cc` | 2417 |
| `PropertyRNA` struct | `source/blender/makesrna/intern/rna_internal_types.hh` | 368 |
| `StructRNA` struct | `source/blender/makesrna/intern/rna_internal_types.hh` | 659 |
| makesrna `main()` | `source/blender/makesrna/intern/makesrna.cc` | 4378 |
| `pyrna_register_class` | `source/blender/python/intern/bpy_rna.cc` | 10367 |
| `bpy_class_validate_recursive` | `source/blender/python/intern/bpy_rna.cc` | 9544 |
| `BLENDER_FILE_VERSION` | `source/blender/blenkernel/BKE_blender_version.h` | 32 |
| Example versioning file | `source/blender/blenloader/intern/versioning_400.cc` | 323 |
| `rna_idproperty_find` | `source/blender/makesrna/intern/rna_access.cc` | 310 |
| `rna_idproperty_check` | `source/blender/makesrna/intern/rna_access.cc` | 616 |
