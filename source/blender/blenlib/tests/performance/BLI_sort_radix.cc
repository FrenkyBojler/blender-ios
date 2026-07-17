/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cinttypes>

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_rand.hh"
#include "BLI_time.hh"

namespace blender {

static const int64_t GROUP_COUNTS[] = {8, 64, 512, 4096, 32768, 262144, 1048576, 4194304};
static const int64_t INDEX_COUNTS[] = {100000, 1000000, 10000000};

static Array<int> make_random_indices(const int64_t groups_num, const int64_t indices_num)
{
  RandomNumberGenerator random;
  Array<int> indices(indices_num);
  for (const int64_t i : indices.index_range()) {
    indices[i] = random.get_int32(int32_t(groups_num));
  }
  return indices;
}

static double run_iterations(const FunctionRef<void()> function)
{
  constexpr int iterations = 8;
  double min_time = 1e30;
  for (int repetition = 0; repetition < iterations; repetition++) {
    const double time_before = BLI_time_now_seconds();
    function();
    const double time_after = BLI_time_now_seconds();
    min_time = std::min(min_time, time_after - time_before);
  }
  return min_time;
}

static void print_header(const char *title)
{
  printf("\n%s\n", title);
  printf("%12s", "groups");
  for (const int64_t indices_num : INDEX_COUNTS) {
    printf(" %12" PRId64, indices_num);
  }
  printf("\n");
}

TEST(sort_radix_performance, CountIndices)
{
  print_header("array_utils::count_indices");
  for (const int64_t groups_num : GROUP_COUNTS) {
    printf("%12" PRId64, groups_num);
    for (const int64_t indices_num : INDEX_COUNTS) {
      const Array<int> indices = make_random_indices(groups_num, indices_num);
      Array<int> counts(groups_num, 0);
      const double time = run_iterations([&]() {
        counts.as_mutable_span().fill(0);
        array_utils::count_indices(indices, counts);
      });
      printf(" %12.4f ms", time * 1000.0);
      fflush(stdout);
    }
    printf("\n");
  }
}

TEST(sort_radix_performance, BuildGroupsFromIndices)
{
  print_header("offset_indices::build_groups_from_indices");
  for (const int64_t groups_num : GROUP_COUNTS) {
    printf("%12" PRId64, groups_num);
    for (const int64_t indices_num : INDEX_COUNTS) {
      const Array<int> indices = make_random_indices(groups_num, indices_num);
      Array<int> offset_data;
      Array<int> index_data;
      const double time = run_iterations([&]() {
        offset_indices::build_groups_from_indices(
            indices, int(groups_num), offset_data, index_data);
      });
      printf(" %12.4f ms", time * 1000.0);
      fflush(stdout);
    }
    printf("\n");
  }
}

}  // namespace blender
