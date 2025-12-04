#include <fmt/format.h>

#include "BLI_index_range.hh"

int main(int argc, char *argv[])
{
  std::string data =
      R"(diff --git a/source/blender/blenloader_diff/CMakeLists.txt b/source/blender/blenloader_diff/CMakeLists.txt
index 949e6da4d5b..c654b3bb650 100644
--- a/source/blender/blenloader_diff/CMakeLists.txt
+++ b/source/blender/blenloader_diff/CMakeLists.txt
@@ -2,13 +2,10 @@
#
#SPDX - License - Identifier : GPL - 2.0 - or -later

-set(INC
+set(TEXTCONV_INC
 )

-set(INC_SYS
-)
-
-set(SRC
+set(TEXTCONV_SRC
   intern/blend_textconv.cc
 )

)";
  //   fmt::println(data);
  //   for (const int i : blender::IndexRange(argc)) {
  //     fmt::println("Arg {}: {}", i, argv[i]);
  //   }
  //   fmt::println(
  //       R"(diff --git a/geometry_nodes_essentials.blend b/geometry_nodes_essentials.blend
  // index 2343243..3452343 100644
  // + sd sd fsdChanged
  // )");
  fmt::println(
      R"(diff --git a/geometry_nodes_essentials.blend b/geometry_nodes_essentials.blend
--- a/geometry_nodes_essentials.blend
+++ b/geometry_nodes_essentials.blend
@@ -1 +10000 @@
-Hello
-sdfsa
+Hella
+sdfsd)");
  return 0;
}
