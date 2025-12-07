#include <fcntl.h>
#include <fmt/format.h>
#include <iostream>

#include "BLI_endian_defines.h"
#include "BLI_fileops.h"
#include "BLI_filereader.h"
#include "BLI_linear_allocator.hh"
#include "BLI_memory_utils.hh"

#include "BLO_core_blend_header.hh"
#include "DNA_genfile.h"
#include "DNA_print.hh"
#include "DNA_sdna_types.h"

namespace blender::blend_textconv {

struct BHeadWithData {
  BHead bhead;
  Span<char> data;
};

class BlenderTextConv {
 private:
  int argc_;
  char **argv_;
  std::string blend_path_;
  LinearAllocator<> allocator_;
  SDNA *sdna_ = nullptr;

 public:
  BlenderTextConv(int argc, char *argv[]) : argc_(argc), argv_(argv) {}

  int run()
  {
    if (argc_ != 2) {
      fmt::println(stderr, "Usage: blend_textconv <filename>");
      return 1;
    }
    blend_path_ = argv_[1];

    const int file_descriptor = BLI_open(blend_path_.c_str(), O_BINARY | O_RDONLY, 0);
    FileReader *rawfile = BLI_filereader_new_file(file_descriptor);
    BLI_SCOPED_DEFER([&]() {
      if (rawfile) {
        rawfile->close(rawfile);
      }
    })

    char first_bytes[7];
    if (rawfile->read(rawfile, first_bytes, sizeof(first_bytes)) != sizeof(first_bytes)) {
      fmt::println(stderr, "Unable to read blender magic bytes");
      return 1;
    }

    /* Rewind to the start of the file. */
    rawfile->seek(rawfile, 0, SEEK_SET);

    FileReader *file = nullptr;
    BLI_SCOPED_DEFER([&]() {
      if (file) {
        file->close(file);
      }
    });

    if (memcmp(first_bytes, "BLENDER", sizeof(first_bytes)) == 0) {
      /* Try opening with memory-mapped IO. */
      file = BLI_filereader_new_mmap(file_descriptor);
      if (!file) {
        file = rawfile;
        rawfile = nullptr;
      }
    }
    else if (BLI_file_magic_is_gzip(first_bytes)) {
      file = BLI_filereader_new_gzip(rawfile);
      if (file) {
        rawfile = nullptr;
      }
    }
    else if (BLI_file_magic_is_zstd(first_bytes)) {
      file = BLI_filereader_new_zstd(rawfile);
      if (file) {
        rawfile = nullptr;
      }
    }
    if (!file) {
      fmt::println(stderr, "Unrecognized file format");
      return 1;
    }

    BlenderHeaderVariant header_variant = BLO_readfile_blender_header_decode(file);
    if (std::holds_alternative<BlenderHeaderInvalid>(header_variant)) {
      fmt::println(stderr, "Invalid blend file header");
      return 1;
    }
    if (std::holds_alternative<BlenderHeaderUnknown>(header_variant)) {
      fmt::println(stderr, "Unknown blend file header");
      return 1;
    }
    const BlenderHeader &header = std::get<BlenderHeader>(header_variant);
    fmt::println("File Version: {}", header.file_version);
    fmt::println("File Format Version: {}", header.file_format_version);

    if (header.endian != L_ENDIAN) {
      fmt::println(stderr, "File is not little endian");
      return 1;
    }

    Vector<BHeadWithData> blocks;
    std::optional<int> dna_block_i;
    Vector<int> non_data_blocks_indices;

    int block_index = 0;
    while (true) {
      BLI_SCOPED_DEFER([&]() { block_index++; });
      const std::optional<BHead> bhead = BLO_readfile_read_bhead(file, header.bhead_type());
      if (!bhead.has_value()) {
        break;
      }
      if (bhead->len < 0) {
        fmt::println(stderr, "Invalid block length");
        return 1;
      }

      void *data = allocator_.allocate(bhead->len, 16);
      if (file->read(file, data, bhead->len) != bhead->len) {
        fmt::println(stderr, "Failed to read block data");
        return 1;
      }
      const int bhead_index = blocks.append_and_get_index(
          {*bhead, {static_cast<const char *>(data), bhead->len}});
      if (bhead->code == BLO_CODE_DNA1) {
        if (dna_block_i.has_value()) {
          fmt::println(stderr, "More than one DNA block");
          return 1;
        }
        dna_block_i = bhead_index;
      }
      if (bhead->code != BLO_CODE_DATA) {
        non_data_blocks_indices.append(bhead_index);
      }
    }

    if (!dna_block_i) {
      fmt::println(stderr, "No DNA block found");
      return 1;
    }
    {
      const BHeadWithData &sdna_block = blocks[*dna_block_i];
      const char *error_message = nullptr;
      // TODO: Make sure #DNA_sdna_from_data is safe against maliscious data.
      sdna_ = DNA_sdna_from_data(
          sdna_block.data.data(), sdna_block.data.size(), false, true, &error_message);
      if (!sdna_) {
        fmt::println(stderr, "Failed to parse DNA block: {}", error_message);
        return 1;
      }
    }

    for (const int bhead_i : blocks.index_range()) {
      const BHeadWithData &block = blocks[bhead_i];
      switch (block.bhead.code) {
        case BLO_CODE_DATA: {
          if (block.bhead.SDNAnr < 0 || block.bhead.SDNAnr >= sdna_->types_num) {
            fmt::println("BLO_CODE_DATA: SDNAnr out of range");
            return 1;
          }

          dna::print_structs_at_address(*sdna_,
                                        block.bhead.SDNAnr,
                                        block.data.data(),
                                        block.bhead.old,
                                        block.bhead.nr,
                                        std::cout);
          break;
        }
      }
    }

    return 0;
  }
};

}  // namespace blender::blend_textconv

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fmt::println("Usage: blend_textconv <filename>");
    return 1;
  }
  blender::blend_textconv::BlenderTextConv blend_textconv(argc, argv);
  return blend_textconv.run();
}
