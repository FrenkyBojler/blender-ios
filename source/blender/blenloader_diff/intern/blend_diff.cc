#include <fmt/format.h>
#include <fstream>
#include <iostream>

#include "BLI_filereader.h"
#include "BLI_index_range.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_resource_scope.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"
#include "BLO_core_blend_header.hh"
#include "BLO_core_file_reader.hh"
#include "DNA_genfile.h"
#include "DNA_sdna_types.h"

namespace blender::rich_sdna {

class Member;
class Struct;
class Type;
class RichSDNA;

class StructMember {
 public:
  StringRefNull identifier;
  /** This contains a bit more than just the name, e.g. for pointers it contains the `*`. */
  StringRefNull name_with_array;
  int64_t offset_in_struct;
  int64_t elem_size;
  int64_t elem_num;
  int64_t size_in_bytes;

  enum class Category {
    Struct,
    Primitive,
    Pointer,
  };
  Category category;
  const Type *type;
};

struct StructMemberIdentifierGetter {
  StringRef operator()(const StructMember *member) const
  {
    return member->name_with_array;
  }
};

class Struct {
 public:
  const Type *type;
  CustomIDVectorSet<const StructMember *, StructMemberIdentifierGetter> members;
};

class Type {
 public:
  StringRefNull name;
  int64_t size_in_bytes;
  const Struct *opt_struct = nullptr;

  void print(std::ostream &stream, const bool verbose = false) const
  {
    fmt::memory_buffer mem_buf;
    fmt::appender dst{mem_buf};
    fmt::format_to(dst, "Type: {}\n", this->name);
    fmt::format_to(dst, "  Size in bytes: {}\n", this->size_in_bytes);
    if (this->opt_struct) {
      fmt::format_to(dst, "  Members:\n");
      for (const int member_i : this->opt_struct->members.index_range()) {
        const StructMember &member = *this->opt_struct->members[member_i];
        if (verbose) {
          fmt::format_to(dst, "    {}\n", member.identifier);
          fmt::format_to(dst, "      Name with array: {}\n", member.name_with_array);
          fmt::format_to(dst, "      Offset in struct: {}\n", member.offset_in_struct);
          fmt::format_to(dst, "      Elem size: {}\n", member.elem_size);
          fmt::format_to(dst, "      Elem num: {}\n", member.elem_num);
          fmt::format_to(dst, "      Size in bytes: {}\n", member.size_in_bytes);
          fmt::format_to(dst, "      Category: {}\n", int(member.category));
          fmt::format_to(dst, "      Type: {}\n", member.type->name);
        }
        else {
          fmt::format_to(dst, "    {} {}\n", member.type->name, member.name_with_array);
        }
      }
    }
    stream << fmt::to_string(mem_buf);
  }
};

struct StructNameGetter {
  StringRef operator()(const Struct *strct) const
  {
    return strct->type->name;
  }
};

struct TypeNameGetter {
  StringRef operator()(const Type *type_) const
  {
    return type_->name;
  }
};

static bool name_is_pointer(const StringRefNull name)
{
  return name[0] == '*' || (name[0] == '(' && name[1] == '*');
}

class RichSDNA {
 public:
  ResourceScope scope_;
  CustomIDVectorSet<const Struct *, StructNameGetter> structs;
  CustomIDVectorSet<const Type *, TypeNameGetter> types;

  RichSDNA(const SDNA &raw_sdna)
  {
    LinearAllocator<> &allocator = scope_.allocator();
    for (const int type_i : IndexRange(raw_sdna.types_num)) {
      const StringRefNull type_name = allocator.copy_string(raw_sdna.types[type_i]);
      Type &sdna_type = scope_.construct<Type>();
      sdna_type.name = type_name;
      sdna_type.size_in_bytes = raw_sdna.types_size[type_i];
      this->types.add(&sdna_type);
    }
    for (const int struct_i : IndexRange(raw_sdna.structs_num)) {
      const SDNA_Struct &raw_struct = *raw_sdna.structs[struct_i];
      Struct &sdna_struct = scope_.construct<Struct>();
      sdna_struct.type = this->types[raw_struct.type_index];
      const_cast<Type *>(sdna_struct.type)->opt_struct = &sdna_struct;

      for (const int member_i : IndexRange(raw_struct.members_num)) {
        const SDNA_StructMember &raw_member = raw_struct.members[member_i];
        StructMember &sdna_member = scope_.construct<StructMember>();
        sdna_member.elem_num = raw_sdna.members_array_num[raw_member.member_index];
        sdna_member.type = this->types[raw_member.type_index];
        sdna_member.name_with_array = allocator.copy_string(
            raw_sdna.members[raw_member.member_index]);
        const int array_start = sdna_member.name_with_array.find_first_of('[');
        if (array_start == StringRef::not_found) {
          sdna_member.identifier = sdna_member.name_with_array;
        }
        else {
          sdna_member.identifier = allocator.copy_string(
              sdna_member.name_with_array.substr(0, array_start));
        }
        sdna_struct.members.add(&sdna_member);
      }
    }
    for (const Struct *sdna_struct_const : this->structs) {
      Struct &sdna_struct = const_cast<Struct &>(*sdna_struct_const);
      int64_t offset = 0;
      for (const StructMember *sdna_member_const : sdna_struct.members) {
        StructMember &sdna_member = const_cast<StructMember &>(*sdna_member_const);
        sdna_member.offset_in_struct = offset;
        if (name_is_pointer(sdna_member.name_with_array)) {
          sdna_member.category = StructMember::Category::Pointer;
          sdna_member.elem_size = raw_sdna.pointer_size;
        }
        else if (sdna_member.type->opt_struct) {
          sdna_member.category = StructMember::Category::Struct;
          sdna_member.elem_size = sdna_member.type->size_in_bytes;
        }
        else {
          sdna_member.category = StructMember::Category::Primitive;
          sdna_member.elem_size = sdna_member.type->size_in_bytes;
        }
        sdna_member.size_in_bytes = sdna_member.elem_size * sdna_member.elem_num;
        offset += sdna_member.size_in_bytes;
      }
    }
  }

  void print(std::ostream &stream, const bool verbose = false) const
  {
    for (const int type_i : this->types.index_range()) {
      const Type &type = *this->types[type_i];
      type.print(stream, verbose);
    }
  }
};

}  // namespace blender::rich_sdna

namespace blender::blend_diff {

class DiffWriter {
 private:
  fmt::memory_buffer mem_buf_;
  fmt::appender dst_{mem_buf_};

 public:
  DiffWriter(StringRef path)
  {
    if (path.startswith("/")) {
      path = path.drop_prefix(1);
    }
    fmt::format_to(dst_, "diff --git a/{} b/{}\n", path, path);
    fmt::format_to(dst_, "--- a/{}\n", path);
    fmt::format_to(dst_, "+++ b/{}\n", path);
    fmt::format_to(dst_, "@@ -1 +100000 @@\n");
  }

  void writeln_unchanged(const StringRef line)
  {
    fmt::format_to(dst_, " {}\n", line);
  }

  void writeln_changed(const StringRef old_line, const StringRef new_line)
  {
    this->writeln_removed(old_line);
    this->writeln_added(new_line);
  }

  void writeln_added(const StringRef line)
  {
    fmt::format_to(dst_, "+{}\n", line);
  }

  void writeln_removed(const StringRef line)
  {
    fmt::format_to(dst_, "-{}\n", line);
  }

  std::string to_string() const
  {
    return fmt::to_string(mem_buf_);
  }
};

struct BlendBlock {
  BHead bhead;
  void *data = nullptr;
};

struct BlendData {
  std::unique_ptr<LinearAllocator<>> allocator;
  BlenderHeader header;
  Vector<BlendBlock> blocks;
  int64_t sdna_block_index = -1;
  int64_t global_block_index = -1;
};

static SDNA *parse_raw_sdna(const BlendBlock &block)
{
  BLI_assert(block.bhead.code == BLO_CODE_DNA1);
  return DNA_sdna_from_data(block.data, block.bhead.len, false, true, nullptr);
}

static std::optional<BlendData> read_blend_file_data(FileReader &file)
{
  const BlenderHeaderVariant header_variant = BLO_readfile_blender_header_decode(&file);
  const BlenderHeader *header = std::get_if<BlenderHeader>(&header_variant);
  if (!header) {
    return std::nullopt;
  }
  const BHeadType bhead_type = header->bhead_type();
  BlendData blend_file_data;
  blend_file_data.allocator = std::make_unique<LinearAllocator<>>();
  blend_file_data.header = *header;
  while (const std::optional<BHead> bhead = BLO_readfile_read_bhead(&file, bhead_type)) {
    if (bhead->len < 0) {
      return std::nullopt;
    }
    void *data = blend_file_data.allocator->allocate(bhead->len, 16);
    const int64_t read_size = file.read(&file, data, bhead->len);
    if (read_size != bhead->len) {
      return std::nullopt;
    }
    const int index = blend_file_data.blocks.append_and_get_index(BlendBlock{*bhead, data});
    switch (bhead->code) {
      case BLO_CODE_DNA1: {
        blend_file_data.sdna_block_index = index;
        break;
      }
      case BLO_CODE_GLOB: {
        blend_file_data.global_block_index = index;
        break;
      }
    }
  }
  if (blend_file_data.sdna_block_index == -1) {
    return std::nullopt;
  }
  if (blend_file_data.global_block_index == -1) {
    return std::nullopt;
  }
  return blend_file_data;
}

static int main_do(const int argc, char *argv[])
{
  if (argc < 3) {
    fmt::println(stderr, "Usage: blend_diff <file_old> <file_new>");
    return 1;
  }
  const StringRefNull relative_path = argv[1];
  const StringRefNull file_old = argv[2];
  const StringRefNull file_new = argv[5];

  FileReader *file_reader_old = BLO_file_reader_uncompressed_from_path(file_old.c_str());
  FileReader *file_reader_new = BLO_file_reader_uncompressed_from_path(file_new.c_str());
  if (!file_reader_old) {
    fmt::println(stderr, "Unable to open .blend file: {}", file_old);
    return 1;
  }
  if (!file_reader_new) {
    fmt::println(stderr, "Unable to open .blend file: {}", file_new);
    return 1;
  }

  const std::optional<BlendData> blend_data_old = read_blend_file_data(*file_reader_old);
  const std::optional<BlendData> blend_data_new = read_blend_file_data(*file_reader_new);
  file_reader_old->close(file_reader_old);
  file_reader_new->close(file_reader_new);
  if (!blend_data_old) {
    fmt::println(stderr, "Unable to read .blend file: {}", file_old);
    return 1;
  }
  if (!blend_data_new) {
    fmt::println(stderr, "Unable to read .blend file: {}", file_new);
    return 1;
  }

  SDNA *raw_sdna_old = parse_raw_sdna(blend_data_old->blocks[blend_data_old->sdna_block_index]);
  SDNA *raw_sdna_new = parse_raw_sdna(blend_data_new->blocks[blend_data_new->sdna_block_index]);
  if (!raw_sdna_old) {
    fmt::println(stderr, "Unable to parse SDNA: {}", file_old);
    return 1;
  }
  if (!raw_sdna_new) {
    fmt::println(stderr, "Unable to parse SDNA: {}", file_new);
    return 1;
  }
  BLI_SCOPED_DEFER([&]() { DNA_sdna_free(raw_sdna_old); });
  BLI_SCOPED_DEFER([&]() { DNA_sdna_free(raw_sdna_new); });

  using namespace rich_sdna;
  const RichSDNA sdna_old{*raw_sdna_old};
  const RichSDNA sdna_new{*raw_sdna_new};

  // sdna_old.types.lookup_key_as("ArrayModifierData")->print(std::cout);
  // sdna_new.print(std::cout);

  DiffWriter writer(relative_path);
  writer.writeln_removed("Hello");
  writer.writeln_added("Hella");
  writer.writeln_unchanged("ID: Hello");
  writer.writeln_removed("sdfsa");
  for (const int i : IndexRange(1000)) {
    writer.writeln_added(fmt::format("Hello {}", i));
  }

  std::fstream myfile("/home/jacques/Downloads/test.txt", std::ios::out);
  // for (const int i : blender::IndexRange(argc)) {
  // myfile << argv[i] << '\n';
  // fmt::println("Arg {}: {}", i, argv[i]);
  // }
  myfile << writer.to_string();

  std::cout << writer.to_string();
  return 0;

  const std::string dummy_patch = fmt::format(
      R"(diff --git a/{} b/{}
--- a/{}
+++ b/{}
@@ -1 +100000 @@
-Hello
+Hella
)",
      relative_path,
      relative_path,
      relative_path,
      relative_path);

  // myfile << dummy_patch;
  // std::cout << dummy_patch;
  return 0;
}

}  // namespace blender::blend_diff

int main(int argc, char *argv[])
{
  return blender::blend_diff::main_do(argc, argv);
}
