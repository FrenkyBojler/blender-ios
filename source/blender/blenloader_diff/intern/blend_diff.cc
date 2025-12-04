#include <fmt/format.h>
#include <fstream>

#include "BLI_index_range.hh"

namespace blender::blend_diff {

}  // namespace blender::blend_diff

int main(int argc, char *argv[])
{
  if (argc <= 7) {
    return 1;
  }
  const char *file_a = argv[1];
  const char *file_b = argv[5];

  std::fstream myfile("/home/jacques/Downloads/test.txt", std::ios::out);
  for (const int i : blender::IndexRange(argc)) {
    myfile << argv[i] << '\n';
    // fmt::println("Arg {}: {}", i, argv[i]);
  }

  fmt::println(
      R"(diff --git a/{} b/{}
--- a/{}
+++ b/{}
@@ -1 +100000 @@
-Hello
-sdfsa
+Hella
+sdfsd)",
      file_a,
      file_a,
      file_a,
      file_a);
  return 0;
}
