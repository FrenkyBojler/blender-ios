/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_smart_ptr_span.hh"
#include "BLI_vector.hh"

#include "BLI_strict_flags.h" /* Keep last. */

namespace blender::tests {
TEST(smart_ptr_span, DefaultConstructor)
{
  const SmartPtrSpan<std::unique_ptr<int>> unique_ptr_span;
  EXPECT_EQ(unique_ptr_span.size(), 0);
  EXPECT_TRUE(unique_ptr_span.is_empty());
  EXPECT_EQ(unique_ptr_span.begin(), unique_ptr_span.end());
}

TEST(smart_ptr_span, SpanConstructorFromEmptyVector)
{
  const Vector<std::unique_ptr<int>> vec;
  const SmartPtrSpan unique_ptr_span = vec.as_span();
  EXPECT_EQ(unique_ptr_span.size(), 0);
  EXPECT_TRUE(unique_ptr_span.is_empty());
  EXPECT_EQ(unique_ptr_span.begin(), unique_ptr_span.end());
}

static Vector<std::unique_ptr<int>> test_vector_from_size(int64_t size)
{
  Vector<std::unique_ptr<int>> vec;
  for (int64_t i : IndexRange(size)) {
    vec.append(std::make_unique<int>(int(i)));
  }
  const SmartPtrSpan unique_ptr_span(vec.begin(), vec.size());
  EXPECT_EQ(unique_ptr_span.size(), size);
  EXPECT_EQ(unique_ptr_span.is_empty(), size == 0);
  return vec;
}

TEST(smart_ptr_span, StartSizeConstructor)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(12);

  const SmartPtrSpan unique_ptr_span(vec.begin(), vec.size());

  for (const int64_t i : vec.index_range()) {
    int &int_ref = unique_ptr_span[i];
    EXPECT_EQ(&*vec[i], &int_ref);
  }
}

TEST(smart_ptr_span, Iterator)
{
  const int64_t size = 7;
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(size);

  const SmartPtrSpan unique_ptr_span = vec.as_span();

  int64_t i = 0;
  /* Iterate with pointer dereference. */
  for (const int &ref : unique_ptr_span) {
    EXPECT_EQ(vec[i].get(), &ref);
    i++;
  }
  EXPECT_EQ(i, size);
}

TEST(smart_ptr_span, IndexOf)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(7);

  const SmartPtrSpan unique_ptr_span = vec.as_span();

  for (const int64_t i : vec.index_range()) {
    EXPECT_EQ(unique_ptr_span.index_of(vec[i].get()), i);
  }
}

TEST(smart_ptr_span, ForEach)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(7);

  const SmartPtrSpan unique_ptr_span = vec.as_span();

  int64_t total = 0;
  std::for_each(unique_ptr_span.begin(), unique_ptr_span.end(), [&](int &i) { total += i; });
  EXPECT_EQ(total, 21);
}
}  // namespace blender::tests
