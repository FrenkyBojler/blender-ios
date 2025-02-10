/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_indirect_iterator.hh"
#include "BLI_vector.hh"

#include "BLI_strict_flags.h" /* Keep last. */

namespace blender::tests {
TEST(indirect_iterator, DefaultConstructor)
{
  const IndirectIterator<std::unique_ptr<int>> int_view;
  EXPECT_EQ(int_view.size(), 0);
  EXPECT_TRUE(int_view.is_empty());
  EXPECT_EQ(int_view.begin(), int_view.end());
}

TEST(indirect_iterator, SpanConstructorFromEmptyVector)
{
  const Vector<std::unique_ptr<int>> vec;
  const IndirectIterator int_indirect_iterator = vec.as_span();
  EXPECT_EQ(int_indirect_iterator.size(), 0);
  EXPECT_TRUE(int_indirect_iterator.is_empty());
  EXPECT_EQ(int_indirect_iterator.begin(), int_indirect_iterator.end());
}

static Vector<std::unique_ptr<int>> test_vector_from_size(int64_t size)
{
  Vector<std::unique_ptr<int>> vec;
  for (int64_t i : IndexRange(size)) {
    vec.append(std::make_unique<int>(int(i)));
  }
  const IndirectIterator<std::unique_ptr<int>> int_indirect_iterator(vec.begin(), vec.size());
  EXPECT_EQ(int_indirect_iterator.size(), size);
  EXPECT_EQ(int_indirect_iterator.is_empty(), size == 0);
  return vec;
}

TEST(indirect_iterator, StartSizeConstructor)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(12);

  const IndirectIterator int_indirect_iterator(vec.begin(), vec.size());

  for (const int64_t i : vec.index_range()) {
    int &int_ref = int_indirect_iterator[i];
    EXPECT_EQ(&*vec[i], &int_ref);
  }
}

TEST(indirect_iterator, Iterator)
{
  const int64_t size = 7;
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(size);

  const IndirectIterator int_indirect_iterator = vec.as_span();

  int64_t i = 0;
  /* Iterate with pointer dereference. */
  for (const int &ref : int_indirect_iterator) {
    EXPECT_EQ(vec[i].get(), &ref);
    i++;
  }
  EXPECT_EQ(i, size);
}

TEST(indirect_iterator, IndexOf)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(7);

  const IndirectIterator int_indirect_iterator = vec.as_span();

  for (const int64_t i : vec.index_range()) {
    EXPECT_EQ(int_indirect_iterator.index_of(*vec[i]), i);
  }
}

TEST(indirect_iterator, ForEach)
{
  const Vector<std::unique_ptr<int>> vec = test_vector_from_size(7);

  const IndirectIterator int_indirect_iterator = vec.as_span();

  int64_t total = 0;
  std::for_each(
      int_indirect_iterator.begin(), int_indirect_iterator.end(), [&](int &i) { total += i; });
  EXPECT_EQ(total, 21);
}

}  // namespace blender::tests
