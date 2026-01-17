/* SPDX-FileCopyrightText: 2024 Campbell Barton
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Polygon fill algorithm using sweep-line tessellation.
 *
 * Triangulates polygons using a sweep-line approach that processes events
 * (local minima/maxima) from bottom to top, maintaining active chains that
 * form polygon boundaries between events.
 *
 * C++20 port with performance optimizations.
 */

#include "poly_scanfill_2d.hh"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace poly_fill {
namespace {

/* -------------------------------------------------------------------- */
/** \name Configuration Flags
 * \{ */

/**
 * Sentinel value for "no next vertex" in fill_region loop.
 * Must be distinct from valid vertex indices (non-negative) and
 * tessellation vertex indices (negative starting from -1).
 * Using INT_MIN to avoid conflicts with tessellation vertices.
 */
constexpr int NO_NEXT_VERT = std::numeric_limits<int>::min();

/**
 * Sentinel value for removed/invalid triangle vertices.
 * When a face is marked for removal, all three indices are set to this value.
 * The duplicate vertex check in output filtering will catch these faces.
 */
constexpr int TRI_INDEX_REMOVED = std::numeric_limits<int>::min();

/**
 * When true, remove tessellated vertices by rebuilding tessellation.
 * Useful to disable when debugging.
 */
constexpr bool DO_SIMPLIFY_EDGE_ROTATE = true;

/** Special welding pass for overlapping ENTER/EXIT events. */
constexpr bool DO_SIMPLIFY_EDGE_ROTATE_WELD_FOR_EVENT_OVERLAP = true;

constexpr bool DO_FILL = true;

/**
 * When true, scan ahead for cases that require more sophisticated
 * ear clipping to solve.
 */
constexpr bool DO_FILL_EAR_CLIP = true;

/* -------------------------------------------------------------------- */
/** \name Fan Filling
 *
 * The following options are mutually exclusive.
 * \{ */

/**
 * Fan fill Y-axis aligned events by detecting them at the region end-points
 * and fill them as part of region filling logic.
 *
 * Note that an alternative to this was to splice the events back into the fan.
 * This works reasonably well but introduces some difficult-to-reason-about behavior
 * with overlapping vertices and the potential for vertices to be used multiple times
 * (without more sophisticated tracking), so that option was removed.
 * Check the version control history if this is of interest.
 */
constexpr bool DO_FANFILL_BY_REGION_ENDPOINTS = true;

/**
 * Fan-fill Y-axis aligned vertices (not only the event vertices).
 *
 * NOTE: we may want to remove this in favor of removing all Y axis aligned
 * vertices, performing the tessellation, then adding them back.
 * This is useful for two reasons:
 * - It avoids expensive iterations over long chains of Y aligned vertices
 *   that may need to be performed multiple times.
 * - It avoids having to account for degenerate cases where a polygon doubles
 *   back on itself while remaining Y axis aligned.
 *
 * So it's probably worth removing this logic and "cleaning" then re-applying
 * the geometry after triangulation has been performed, since it means a certain
 * category of problems just don't exist.
 * Also, in many cases a complex polygon won't include these at all,
 * so it avoids checking for scenarios that don't exist in many cases,
 * only paying the minor cost of extra computation when they do
 * (organic shapes, freehand drawing etc, aren't as likely to contain
 * many exactly Y polygon points).
 */
constexpr bool DO_FANFILL_AXIS_ALIGNED = true;

/**
 * Skip triangles where all three vertices are on the same Y coordinate.
 * These are zero-area (degenerate) triangles that occur when horizontal edges
 * pass through sweep events.
 *
 * This is an annoying workaround it would be nice to remove.
 * The workaround skips zero area Y aligned faces from fan-filling.
 */
constexpr bool DO_FANFILL_AXIS_ALIGNED_SKIP = true;

/** Detect when a zero area face would be created by Y axis aligned vertices. */
constexpr bool DO_FANFILL_AXIS_ALIGNED_AVOID_ZERO_AREA = true;

/** \} */

/** Use red-black tree for efficient chain lookup. */
constexpr bool DO_RB_TREE = true;

/** Use binary search for chain coordinate lookups. */
constexpr bool DO_BINARY_SEARCH = true;

/**
 * Use incremental bisect cache for #calc_fan_from_events.
 * When true: Uses a cached lower bound that increases as scan-line moves up.
 * When false: Uses a precomputed hash map of Y coordinates to event ranges.
 */
constexpr bool DO_BINARY_SEARCH_CACHE = true;

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

/**
 * Remove the first occurrence of `value` from `vec` using swap-and-pop.
 * O(1) removal since order is not preserved.
 */
template<typename T> inline void find_and_swap_erase(std::vector<T> &vec, const T &value)
{
  auto it = std::find(vec.begin(), vec.end(), value);
  if (it != vec.end()) {
    *it = vec.back();
    vec.pop_back();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sign Type Classification
 * \{ */

/**
 * Triangle sign classification.
 * Note: Values are significant - enum is cast to int for array indexing and ordering comparisons.
 */
enum class SignType : uint8_t {
  Concave = 0,
  Tangential = 1,
  Convex = 2,
  /** Convex ear where tip is at X-extreme (leftmost or rightmost). Preferred for clipping. */
  ConvexIsTipXExtreme = 3,
  /** Locked ear - must not be clipped (used for edge vertices in ear-clipping). */
  Nop = 4,
};

/** Number of SignType values (for array sizing). */
constexpr size_t SIGN_TYPE_COUNT = 5;

/** Event types. */
enum class EventType : uint8_t {
  None = 0,
  Exit = 1,
  Enter = 2,
};

/** RB tree colors. */
enum class RBColor : uint8_t {
  Red = 0,
  Black = 1,
};

constexpr Scalar SCALAR_INF = std::numeric_limits<Scalar>::infinity();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Math Functions
 * \{ */

/**
 * Branch-less sign classification.
 * Maps: negative -> Concave(0), zero -> Tangential(1), positive -> Convex(2).
 * Uses arithmetic on boolean comparisons to avoid branch mis-prediction.
 */
[[nodiscard]] inline SignType signum_enum(const Scalar a)
{
  /* Original: a > 0.0 ? Convex : (a == 0.0 ? Tangential : Concave) */
  return static_cast<SignType>((a > 0.0) + (a >= 0.0));
}

/**
 * Alternative version of `area_tri_signed_v2`.
 * Needed because of double precision issues.
 * NOTE: removes `/ 2` since it's not needed (we only need the sign).
 */
[[nodiscard]] inline Scalar tri_v2_area_signed_alt_2x(const Vert &v0,
                                                      const Vert &v1,
                                                      const Vert &v2)
{
  const Scalar d2_x = v1[0] - v0[0];
  const Scalar d2_y = v1[1] - v0[1];
  const Scalar d3_x = v2[0] - v0[0];
  const Scalar d3_y = v2[1] - v0[1];
  return (d2_x * d3_y) - (d3_x * d2_y);
}

[[nodiscard]] inline SignType tri_v2_sign(const Vert &v0, const Vert &v1, const Vert &v2)
{
  return signum_enum(tri_v2_area_signed_alt_2x(v0, v1, v2));
}

[[nodiscard]] inline Scalar len_squared_v2v2(const Vert &v0, const Vert &v1)
{
  const Scalar x = v1[0] - v0[0];
  const Scalar y = v1[1] - v0[1];
  return (x * x) + (y * y);
}

/**
 * Calculate Y gradient (dx/dy) of segment.
 * Returns +/-infinity for horizontal segments, 0 for vertical.
 */
[[nodiscard]] inline Scalar segment_v2_v2_y_gradient(const Vert &co0, const Vert &co1)
{
  assert(co0[1] <= co1[1]);
  const Scalar x_delta = co1[0] - co0[0];
  const Scalar y_delta = co1[1] - co0[1];

  /* Unlikely (horizontal). */
  if (y_delta == 0.0) [[unlikely]] {
    if (x_delta < 0.0) {
      return -SCALAR_INF;
    }
    if (x_delta > 0.0) {
      return SCALAR_INF;
    }
    return 0.0;
  }

  assert(y_delta > 0.0);
  return x_delta / y_delta;
}

/** Return corner index opposite to edge (a, b) in a triangle. */
[[nodiscard]] inline int tri_corner_opposite_edge(int a, int b)
{
  if (a > b) {
    std::swap(a, b);
  }
  /* (0,1)->2, (0,2)->1, (1,2)->0 */
  if (a == 0) {
    return (b == 1) ? 2 : 1;
  }
  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small Vector
 *
 * Inline storage for small counts, heap for overflow.
 * \{ */

template<uint32_t N> struct SmallVectorInt {
  union {
    /** Inline storage for up to N ints. */
    int inline_data_[N];
    /** Pointer to heap allocation. */
    int *heap_ptr_;
  };
  /** Number of elements. */
  uint32_t size_ = 0;
  /** 0 = inline mode, >0 = heap mode with this capacity. */
  uint32_t capacity_ = 0;

  SmallVectorInt() : inline_data_{} {}

  ~SmallVectorInt()
  {
    if (capacity_ > 0) {
      delete[] heap_ptr_;
    }
  }

  SmallVectorInt(const SmallVectorInt &other) : size_(other.size_), capacity_(other.capacity_)
  {
    if (capacity_ > 0) {
      heap_ptr_ = new int[capacity_];
      std::copy(other.heap_ptr_, other.heap_ptr_ + size_, heap_ptr_);
    }
    else {
      std::copy(other.inline_data_, other.inline_data_ + N, inline_data_);
    }
  }

  SmallVectorInt &operator=(const SmallVectorInt &other)
  {
    if (this != &other) {
      if (capacity_ > 0) {
        delete[] heap_ptr_;
      }
      size_ = other.size_;
      capacity_ = other.capacity_;
      if (capacity_ > 0) {
        heap_ptr_ = new int[capacity_];
        std::copy(other.heap_ptr_, other.heap_ptr_ + size_, heap_ptr_);
      }
      else {
        std::copy(other.inline_data_, other.inline_data_ + N, inline_data_);
      }
    }
    return *this;
  }

  SmallVectorInt(SmallVectorInt &&other) noexcept : size_(other.size_), capacity_(other.capacity_)
  {
    if (capacity_ > 0) {
      heap_ptr_ = other.heap_ptr_;
      other.heap_ptr_ = nullptr;
    }
    else {
      std::copy(other.inline_data_, other.inline_data_ + N, inline_data_);
    }
    other.size_ = 0;
    other.capacity_ = 0;
  }

  SmallVectorInt &operator=(SmallVectorInt &&other) noexcept
  {
    if (this != &other) {
      if (capacity_ > 0) {
        delete[] heap_ptr_;
      }
      size_ = other.size_;
      capacity_ = other.capacity_;
      if (capacity_ > 0) {
        heap_ptr_ = other.heap_ptr_;
        other.heap_ptr_ = nullptr;
      }
      else {
        std::copy(other.inline_data_, other.inline_data_ + N, inline_data_);
      }
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }

  void push_back(const int value)
  {
    if (capacity_ == 0) {
      if (size_ < N) {
        inline_data_[size_] = value;
        size_++;
        return;
      }
      /* Transition to heap mode. */
      capacity_ = N * 2;
      int *new_ptr = new int[capacity_];
      std::copy(inline_data_, inline_data_ + N, new_ptr);
      new_ptr[N] = value;
      heap_ptr_ = new_ptr;
      size_ = N + 1;
    }
    else {
      if (size_ == capacity_) {
        const uint32_t new_capacity = capacity_ * 2;
        int *new_ptr = new int[new_capacity];
        std::copy(heap_ptr_, heap_ptr_ + size_, new_ptr);
        delete[] heap_ptr_;
        heap_ptr_ = new_ptr;
        capacity_ = new_capacity;
      }
      heap_ptr_[size_] = value;
      size_++;
    }
  }

  [[nodiscard]] uint32_t size() const noexcept
  {
    return size_;
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return size_ == 0;
  }

  [[nodiscard]] const int *data() const noexcept
  {
    return capacity_ == 0 ? inline_data_ : heap_ptr_;
  }

  [[nodiscard]] int *data() noexcept
  {
    return capacity_ == 0 ? inline_data_ : heap_ptr_;
  }

  [[nodiscard]] const int &operator[](uint32_t i) const noexcept
  {
    assert(i < size_);
    return data()[i];
  }

  [[nodiscard]] int &operator[](uint32_t i) noexcept
  {
    assert(i < size_);
    return data()[i];
  }

  [[nodiscard]] const int &back() const noexcept
  {
    assert(size_ > 0);
    return data()[size_ - 1];
  }

  [[nodiscard]] int first_unchecked() const noexcept
  {
    assert(size_ > 0);
    return data()[0];
  }

  [[nodiscard]] const int *begin() const noexcept
  {
    return data();
  }
  [[nodiscard]] const int *end() const noexcept
  {
    return data() + size_;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forward Declarations
 * \{ */

struct VertsTable;
struct FaceResult;
struct VertChain;
struct VertChain_List;
struct IndexSubset;
struct VertChain_RegionInfo;
struct SweepInterval;

/** Context passed to face construction functions. */
struct FaceConstructContext {
  const PolyFillParams &params;
  std::vector<FaceResult> &faces_result;
  VertsTable &verts_table;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertsTable
 *
 * Manages vertex coordinates including tessellated vertices.
 * \{ */

/** Combined key for tessellation vertex lookup: edge (v0, v1) + y-coordinate. */
struct TessVertKey {
  int v0;
  int v1;
  Scalar co_y;

  bool operator==(const TessVertKey &other) const
  {
    return v0 == other.v0 && v1 == other.v1 && co_y == other.co_y;
  }
};

struct TessVertKeyHash {
  size_t operator()(const TessVertKey &k) const noexcept
  {
    /* Note but this hashing function may seem weak.
     * however we can assume all of the inputs already have good distribution,
     * so there isn't the need to excessively randomized bits. */

    /* Normalize -0.0 to +0.0 so they hash identically (since -0.0 == +0.0). */
    const Scalar co_y = (k.co_y != 0.0) ? k.co_y : Scalar(0.0);
    size_t h;
    if constexpr (sizeof(Scalar) == sizeof(uint64_t)) {
      uint64_t bits;
      std::memcpy(&bits, &co_y, sizeof(Scalar));
      h = ((int64_t(k.v0) << 32) | uint32_t(k.v1)) + bits;
    }
    else {
      uint32_t bits;
      std::memcpy(&bits, &co_y, sizeof(Scalar));
      h = (int64_t(k.v0) << 32) | (uint32_t(k.v1) + bits);
    }
    return h;
  }
};

struct VertsTable {
  std::span<const Vert> coords;
  /**
   * Tessellation vertices are interpolated between existing edges,
   * created during the sweep-line fill.
   * Indices that reference these vertices use negative numbers:
   * -1 for 0, -2 for 1 and so on.
   */
  std::vector<Vert> coords_tess;
  /**
   * Flattened map: `(v0, v1, co_y)` -> `tess_vert_index` for a direct lookup.
   * \note it would be correct to map `(v0, v1)` to a map of `(co_y -> index)`.
   * But this results in 2x lookups which results in slightly worse performance.
   */
  std::unordered_map<TessVertKey, int, TessVertKeyHash> tess_verts_map;

  explicit VertsTable(std::span<const Vert> coords_in) : coords(coords_in)
  {
    /* Don't reserve `coords_tess` because some polygons may have no tessellation
     * (or very low), and it's not practical to predict. */
  }

  [[nodiscard]] const Vert &co_at_index(const int i) const
  {
    if (i < 0) [[unlikely]] {
      return coords_tess[i ^ -1];
    }
    return coords[i];
  }

  int ensure_tess_vert(const int v0, const int v1, const Scalar co_y);
};

/** Helper function to calculate X from Y and vertex indices. */
[[nodiscard]] inline std::pair<Scalar, int> x_from_y_and_verts(
    std::span<const Vert> verts, const Scalar y, const int v0, const int v1, const int i_default)
{
  assert(v0 >= 0);
  assert(v1 >= 0);
  const Scalar y_min = verts[v0][1];
  const Scalar y_max = verts[v1][1];
  assert(y_min <= y_max);
  assert(y >= y_min);
  assert(y <= y_max);

  const Scalar x_0 = verts[v0][0];
  const Scalar x_1 = verts[v1][0];

  /* Avoid double precision issues for exact matches. */
  if (y == y_min) {
    return {x_0, v0};
  }
  if (y == y_max) {
    return {x_1, v1};
  }

  const Scalar y_span = y_max - y_min;
  const Scalar y_factor = y - y_min;
  const Scalar unit_factor = y_factor / y_span;

  return {(x_1 * unit_factor) + (x_0 * (1.0 - unit_factor)), i_default};
}

[[nodiscard]] inline Scalar x_from_y_and_coords(const Scalar y, const Vert &co0, const Vert &co1)
{
  const Scalar y_min = co0[1];
  const Scalar y_max = co1[1];
  assert(y_min <= y_max);
  assert(y >= y_min);
  assert(y <= y_max);

  const Scalar x_0 = co0[0];
  const Scalar x_1 = co1[0];

  /* Avoid double precision issues for exact matches. */
  if (y == y_min) {
    return x_0;
  }
  if (y == y_max) {
    return x_1;
  }

  const Scalar y_span = y_max - y_min;
  const Scalar y_factor = y - y_min;
  const Scalar unit_factor = y_factor / y_span;

  return (x_1 * unit_factor) + (x_0 * (1.0 - unit_factor));
}

int VertsTable::ensure_tess_vert(const int v0, const int v1, const Scalar co_y)
{
  assert(coords[v0][1] != coords[v1][1]);

  /* Single lookup: returns existing entry or inserts placeholder. */
  auto [it, inserted] = tess_verts_map.try_emplace(TessVertKey{v0, v1, co_y}, 0);
  if (!inserted) {
    return it->second;
  }

  /* New entry: compute coordinates and assign index. */
  const int i = int(coords_tess.size()) ^ -1;
  std::pair<Scalar, int> co_x_index = x_from_y_and_verts(coords, co_y, v0, v1, -1);
  assert(co_x_index.second == -1);
  coords_tess.push_back({co_x_index.first, co_y});
  it->second = i;
  return i;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FaceResult
 *
 * Stores a triangle face.
 * \{ */

struct FaceResult {
  Face tri;

  explicit FaceResult(const Face &tri_in) : tri(tri_in) {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertEarClipCount
 *
 * Counts ear types for ear clipping.
 * \{ */

/**
 * Tracks count of each ear type during ear-clipping.
 * Used to efficiently find clippable ears without scanning the entire list.
 */
struct VertEarClipCount {
  std::array<int, SIGN_TYPE_COUNT> values = {0, 0, 0, 0, 0};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertEar
 *
 * Vertex ear for ear clipping algorithm.
 * \{ */

struct VertEar {
  SignType ty;
  int vert_index;
  VertEar *ear_prev = nullptr;
  VertEar *ear_next = nullptr;

  explicit VertEar(const int vert_index_in) : vert_index(vert_index_in) {}

  /** True if this ear's vertex is leftmost or rightmost among its neighbors. */
  [[nodiscard]] bool is_tip_x_extreme(const VertsTable &verts_table) const
  {
    const Scalar prev_x = verts_table.co_at_index(ear_prev->vert_index)[0];
    const Scalar curr_x = verts_table.co_at_index(vert_index)[0];
    const Scalar next_x = verts_table.co_at_index(ear_next->vert_index)[0];
    return (curr_x <= prev_x) == (curr_x <= next_x);
  }

  [[nodiscard]] SignType sign_calc(const VertsTable &verts_table) const
  {
    return tri_v2_sign(verts_table.co_at_index(ear_prev->vert_index),
                       verts_table.co_at_index(vert_index),
                       verts_table.co_at_index(ear_next->vert_index));
  }

  void sign_calc_init(VertEarClipCount &counts, const VertsTable &verts_table)
  {
    ty = sign_calc(verts_table);
    if (ty == SignType::Convex && is_tip_x_extreme(verts_table)) {
      ty = SignType::ConvexIsTipXExtreme;
    }
    counts.values[int(ty)]++;
  }

  void sign_calc_update(VertEarClipCount &counts, const VertsTable &verts_table)
  {
    const SignType ty_prev = ty;
    SignType ty_curr = sign_calc(verts_table);
    if (ty_prev != ty_curr) {
      if (ty_curr == SignType::Convex) {
        if (is_tip_x_extreme(verts_table)) {
          ty_curr = SignType::ConvexIsTipXExtreme;
        }
      }
      counts.values[int(ty_prev)]--;
      counts.values[int(ty_curr)]++;
      ty = ty_curr;
    }
  }

  void unlink_and_update(VertEarClipCount &counts, const VertsTable &verts_table)
  {
    counts.values[int(ty)]--;

    /* Unlink. */
    ear_prev->ear_next = ear_next;
    ear_next->ear_prev = ear_prev;

    /* Calculate (only when not convex).
     * The cast to int handles ordering: `Concave(0), Tangential(1) < Convex(2)`.
     * `Nop(4)` is also < Convex for update logic purposes. */
    if (int(ear_prev->ty) < int(SignType::Convex)) {
      ear_prev->sign_calc_update(counts, verts_table);
    }
    else if (ear_prev->ty == SignType::Convex) {
      if (ear_prev->is_tip_x_extreme(verts_table)) {
        counts.values[int(SignType::Convex)]--;
        ear_prev->ty = SignType::ConvexIsTipXExtreme;
        counts.values[int(SignType::ConvexIsTipXExtreme)]++;
      }
    }

    if (int(ear_next->ty) < int(SignType::Convex)) {
      ear_next->sign_calc_update(counts, verts_table);
    }
    else if (ear_next->ty == SignType::Convex) {
      if (ear_next->is_tip_x_extreme(verts_table)) {
        counts.values[int(SignType::Convex)]--;
        ear_next->ty = SignType::ConvexIsTipXExtreme;
        counts.values[int(SignType::ConvexIsTipXExtreme)]++;
      }
    }

    assert(this != ear_next);
    assert(this != ear_prev);
    assert(ear_prev != ear_next);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name IndexSubset
 *
 * Efficient view into vertex index arrays.
 * \{ */

/**
 * This type avoids duplicating arrays of indices
 * only to add beginning/end indices.
 */
struct IndexSubset {
  std::span<const int> index_list_;
  /** Optional vertex to prepend. Avoids copying arrays just to add boundary vertices. */
  std::optional<int> index_beg_;
  /** Optional vertex to append. Avoids copying arrays just to add boundary vertices. */
  std::optional<int> index_end_;

  IndexSubset(std::span<const int> list,
              std::optional<int> beg = std::nullopt,
              std::optional<int> end = std::nullopt)
      : index_list_(list), index_beg_(beg), index_end_(end)
  {
  }

  [[nodiscard]] int length() const noexcept
  {
    int result = int(index_list_.size());
    if (index_beg_.has_value()) {
      result++;
    }
    if (index_end_.has_value()) {
      result++;
    }
    return result;
  }

  [[nodiscard]] bool is_empty() const noexcept
  {
    if (!index_list_.empty()) {
      return false;
    }
    if (index_beg_.has_value()) {
      return false;
    }
    if (index_end_.has_value()) {
      return false;
    }
    return true;
  }

  [[nodiscard]] int at_index(int index) const noexcept
  {
    assert(index >= 0);
    if (index_beg_.has_value()) {
      if (index == 0) {
        return *index_beg_;
      }
      index--;
    }
    if (!index_end_.has_value()) {
      assert(index < int(index_list_.size()));
    }
    else {
      assert(index <= int(index_list_.size()));
      if (index == int(index_list_.size())) {
        return *index_end_;
      }
    }
    return index_list_[index];
  }

  [[nodiscard]] int at_first() const noexcept
  {
    if (index_beg_.has_value()) {
      return *index_beg_;
    }
    if (!index_list_.empty()) {
      return index_list_.front();
    }
    if (index_end_.has_value()) {
      return *index_end_;
    }
    assert(false); /* Index out of range. */
    return -1;
  }

  [[nodiscard]] int at_last() const noexcept
  {
    if (index_end_.has_value()) {
      return *index_end_;
    }
    if (!index_list_.empty()) {
      return index_list_.back();
    }
    if (index_beg_.has_value()) {
      return *index_beg_;
    }
    assert(false); /* Index out of range. */
    return -1;
  }

  /**
   * Create a sub-view of this IndexSubset from index `beg` to `end` (exclusive).
   * Indices are relative to this subset's logical view, not the underlying array.
   * The returned subset correctly handles index_beg/index_end boundaries, preserving
   * them if they fall within the requested range.
   */
  [[nodiscard]] IndexSubset subspan(const int beg, const int end) const
  {
    assert(0 <= beg && beg <= length());
    assert(0 <= end && end <= length());
    assert(beg <= end);

    int len_final = end - beg;
    int new_beg_offset = beg;

    std::optional<int> new_index_beg;
    std::optional<int> new_index_end;

    if (len_final > 0 && index_beg_.has_value()) {
      if (beg == 0) {
        new_index_beg = index_beg_;
        len_final--;
      }
      else {
        new_beg_offset--;
      }
    }

    if (len_final > 0 && index_end_.has_value()) {
      if (end == length()) {
        new_index_end = index_end_;
        len_final--;
      }
    }

    return IndexSubset(
        index_list_.subspan(new_beg_offset, len_final), new_index_beg, new_index_end);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertStep
 *
 * Tracks previous and current vertex during chain traversal.
 * \{ */

/**
 * Tracks (prev, curr) vertex pair for walking the polygon boundary.
 * Used to determine the next vertex when traversing edges.
 */
struct VertStep {
  int prev;
  int curr;

  VertStep(const int prev_in, const int curr_in) : prev(prev_in), curr(curr_in) {}

  void push(const int next_index)
  {
    prev = curr;
    curr = next_index;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertChain_RegionInfo
 *
 * Represents a region to be filled, bounded by left and right chains.
 * \{ */

/**
 * A region bounded by left and right vertex chains with optional fan vertices.
 * Passed to the callback in #SweepInterval::step_regions.
 */
struct VertChain_RegionInfo {
  IndexSubset indices_l;
  IndexSubset indices_r;
  std::vector<int> fan_beg;
  std::vector<int> fan_end;

  VertChain_RegionInfo(IndexSubset indices_l_in,
                       IndexSubset indices_r_in,
                       std::vector<int> fan_beg_in,
                       std::vector<int> fan_end_in)
      : indices_l(std::move(indices_l_in)),
        indices_r(std::move(indices_r_in)),
        fan_beg(std::move(fan_beg_in)),
        fan_end(std::move(fan_end_in))
  {
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event
 *
 * Represents an enter or exit event in the sweep line algorithm.
 * \{ */

struct Event {
  /**
   * Use `SmallVectorInt` as practically all events have only 2 vertices.
   * Unless there are exactly overlapping vertices.
   */
  SmallVectorInt<2> vert_index_list;
  EventType ty;
  /** Chain pairs starting at this event (ENTER). Each pair is (left_chain, right_chain). */
  std::vector<std::pair<VertChain *, VertChain *>> chains_beg;
  /** Chains ending at this event (EXIT). */
  std::vector<VertChain *> chains_end;

  Event(const int vert_index, const EventType ty_in) : ty(ty_in)
  {
    assert(vert_index >= 0);
    vert_index_list.push_back(vert_index);
  }

  /**
   * Get the coordinate of the first vertex in this event.
   * Multiple vertices in an event always share the same location,
   * so using the first is not arbitrary.
   * Events are never created from tessellated vertices,
   * so the index is always non-negative.
   */
  [[nodiscard]] const Vert &co(std::span<const Vert> verts) const
  {
    const int vert_index = vert_index_list.first_unchecked();
    assert(vert_index >= 0);
    return verts[vert_index];
  }
};

/** Get the other vertex connected to `vert_curr` (not `vert_prev`). */
[[nodiscard]] inline int edge_other(const int vert_prev,
                                    const int vert_curr,
                                    const VertsEdgeMap &verts_edge_map)
{
  const std::array<int, 2> &adj = verts_edge_map[vert_curr];
  if (adj[0] == vert_prev) {
    return adj[1];
  }
  assert(adj[1] == vert_prev);
  return adj[0];
}

/**
 * Walk edges from vert_curr until Y coordinate changes.
 * Used to skip over Y-aligned edge sequences when determining event types.
 * Returns the first vertex with a different Y, or loops back to i_init if all are aligned.
 */
[[nodiscard]] inline int edge_other_until_y_axis_changes(const int vert_prev_in,
                                                         const int vert_curr_in,
                                                         std::span<const Vert> verts,
                                                         const VertsEdgeMap &verts_edge_map)
{
  const Scalar y = verts[vert_prev_in][1];
  assert(y == verts[vert_curr_in][1]);
  const int i_init = vert_prev_in;
  int vert_prev = vert_prev_in;
  int vert_curr = vert_curr_in;

  while (vert_curr != i_init) {
    const std::array<int, 2> &adj = verts_edge_map[vert_curr];
    const int a = adj[0];
    const int b = adj[1];
    if (vert_prev == a) {
      vert_prev = vert_curr;
      vert_curr = b;
    }
    else if (vert_prev == b) {
      vert_prev = vert_curr;
      vert_curr = a;
    }
    else {
      assert(false); /* Invalid topology. */
    }

    if (y != verts[vert_curr][1]) {
      return vert_curr;
    }
  }

  /* Unlikely, this could loop back to a circle that exists on the same Y axis. */
  return i_init;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Walking Functions
 *
 * Standalone functions for walking polygon edges and building index lists.
 * \{ */

/**
 * Walk edges from the current step position until reaching an event vertex.
 * Appends visited vertex indices to the output vector and updates the step state.
 */
inline bool walk_until_next_event_impl(VertStep &step,
#ifndef NDEBUG
                                       const VertsTable &verts_table,
#else
                                       const VertsTable & /*verts_table*/,
#endif
                                       const VertsEdgeMap &verts_edge_map,
                                       const std::vector<EventType> &verts_event,
#ifndef NDEBUG
                                       const bool degenerate,
#else
                                       const bool /*degenerate*/,
#endif
                                       std::vector<int> &indices)
{
#ifndef NDEBUG
  const Vert &co_curr = verts_table.co_at_index(step.curr);
#endif
  while (true) {
    const int vert_step = edge_other(step.prev, step.curr, verts_edge_map);
#ifndef NDEBUG
    const Vert &co_next = verts_table.co_at_index(step.curr);
    assert(co_curr[1] <= co_next[1]);
#endif
    step.push(vert_step);
#ifndef NDEBUG
    /* Only run this O(n) check in debug builds. */
    if (!degenerate) {
      assert(std::find(indices.begin(), indices.end(), vert_step) == indices.end());
    }
#endif
    indices.push_back(vert_step);
    const EventType event_type = verts_event[vert_step];
    if (event_type == EventType::Enter || event_type == EventType::Exit) {
      return true;
    }
  }
  /* Unreachable: loop always exits via return. */
  assert(false);
  return false;
}

/**
 * Build a complete index list by walking from start_index until reaching an event.
 */
[[nodiscard]] inline std::vector<int> build_indices_until_event(
    const int start_index,
    VertStep &step,
    const VertsTable &verts_table,
    const VertsEdgeMap &verts_edge_map,
    const std::vector<EventType> &verts_event,
    const bool degenerate)
{
  std::vector<int> indices;
  indices.push_back(start_index);
  walk_until_next_event_impl(step, verts_table, verts_edge_map, verts_event, degenerate, indices);
  return indices;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertChainBisectCacheY
 *
 * Cache for binary search in VertChain::calc_x_at_y.
 * \{ */

/**
 * Caches the result of the last binary search to accelerate subsequent queries.
 * Since the sweep-line moves monotonically upward, consecutive queries often
 * fall within the same segment or an adjacent one.
 */
struct VertChainBisectCacheY {
  /** Last segment index found, -1 when unset. */
  int index = -1;
  /** Vertex pointers for the cached segment: [index] and [index + 1]. */
  const Vert *co_of_index[2] = {nullptr, nullptr};
};

/**
 * Simple lookup cache for calc_x_at_y.
 * Stores the last (Y, X) pair to avoid recomputation when the same Y is queried.
 *
 * \note In tests this typically achieves 50-80% hit rate depending on polygon complexity,
 * where higher complexity tends towards better cache reuse.
 */
struct VertChainXAtYCache {
  /** Cached Y value, SCALAR_INF when unset. */
  Scalar y = SCALAR_INF;
  /** Cached X result for the stored Y. */
  Scalar x = 0.0;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertChain
 *
 * A chain of vertices forming one side of the polygon boundary.
 * \{ */

/**
 * A chain of vertices forming one side of the polygon boundary.
 *
 * In the sweep-line algorithm, chains represent monotonic sequences of vertices
 * that extend from one event (local minimum/maximum) to another. Each chain
 * stores the sequence of vertex indices traversed, along with metadata for:
 * - Doubly-linked list navigation (`chain_prev`, `chain_next`).
 * - Red-black tree ordering for efficient spatial queries (`rb_*` fields).
 * - Split points where horizontal scan lines intersect the chain.
 *
 * Vertex Indexing:
 * - Non-negative indices reference original polygon vertices.
 * - Negative indices reference tessellated (interpolated) vertices
 *   (using XOR complement encoding: `index ^ -1` gives position in `coords_tess`).
 *   Note that filling uses the split-points however they are eventually
 *   collapsed (see `rotate_edges_away_pass`).
 */
struct VertChain {

  VertChain *chain_next = nullptr;
  VertChain *chain_prev = nullptr;

  /**
   * Vertex indices used to divide the chain at split points.
   * Each entry is (index in `indices`, vertex index).
   */
  std::vector<std::pair<
      /** The index in `indices`. */
      int,
      /** The vertex index. */
      int>>
      splits;

  /** Cache for calc_x_at_y binary search. */
  mutable VertChainBisectCacheY bisect_cache;
  /** Cache for calc_x_at_y result lookup (50-80% hit rate). */
  mutable VertChainXAtYCache x_at_y_cache;

  bool is_enter = false;

  /**
   * Store as an optimization because:
   * - Many chains will *not* contain any splits that exactly match an existing vertex.
   * - When false, `apply_split_y_axis_aligned_bias` can be skipped entirely,
   *   which will be the case for practically all polygons that don't
   *   contain Y axis aligned vertices.
   */
  bool needs_y_axis_aligned_bias_calc = false;

  /** RB-tree pointers. */
  VertChain *rb_left = nullptr;
  VertChain *rb_right = nullptr;
  VertChain *rb_parent = nullptr;
  RBColor rb_color = RBColor::Red;
  /**
   * UID is a tie breaker so there are *never* duplicates in the tree.
   * This avoids the need to deal with exact duplicates.
   * Note that these UID values may be swapped between `VertChain`s,
   * but must _always_ be unique.
   * Initialized to zero but must be non-zero before use in the RB-tree.
   *
   * \note A 32-bit integer is sufficient as it exceeds any practical vertex count.
   */
  uint32_t rb_uid = 0;

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif

  /**
   * Always a pair, this is almost always (`indices[-2]`, `indices[-1]`),
   * but may not be in the case of tessellated vertices.
   *
   * \note Must be last member. Actual storage is over-allocated after the struct.
   */
  int indices_num = 0;
  int indices[0];

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

  /* Private constructor, use create() instead. */
  explicit VertChain(const int indices_num_in) : indices_num(indices_num_in) {}

  /* Non-copyable, non-movable (due to over-allocation). */
  VertChain(const VertChain &) = delete;
  VertChain &operator=(const VertChain &) = delete;
  VertChain(VertChain &&) = delete;
  VertChain &operator=(VertChain &&) = delete;

  static VertChain *create(const std::vector<int> &indices_in)
  {
    const size_t alloc_size = sizeof(VertChain) + indices_in.size() * sizeof(int);
    void *mem = operator new(alloc_size);
    VertChain *chain = new (mem) VertChain(int(indices_in.size()));
    std::memcpy(chain->indices, indices_in.data(), indices_in.size() * sizeof(int));
    return chain;
  }

  static void destroy(VertChain *chain)
  {
    chain->~VertChain();
    operator delete(chain);
  }

  /**
   * Find the segment index containing `co_y` using binary search with caching.
   *
   * \param r_co_pair Output: pointers to segment endpoints.
   * - When exact match: `r_co_pair[0] = null`, `r_co_pair[1] = Y[lo]`.
   * - When interpolating: `r_co_pair[0] = Y[lo-1]`, `r_co_pair[1] = Y[lo]`.
   *
   * \return The upper index of the segment (`lo`),
   * where `co_y` falls between `indices[lo-1]` and `indices[lo]`.
   * Special return values:
   *  -2: `co_y` is below the chain (out of bounds below).
   *  -1: `co_y` is above the chain (out of bounds above).
   */
  [[nodiscard]] int find_segment_for_y(const Scalar co_y,
                                       const VertsTable &verts_table,
                                       const Vert *r_co_pair[2]) const
  {
    /* NOTE(@ideasman42): for complex polygons which cause the sweep-line
     * to track many vertex-chains simultaneously.
     * This function may be called many times (via `calc_x_at_y`).
     * So even minor performance improvements may be worth considering.
     *
     * Consider the #VertChain may reference a large number of vertices (via `indices`).
     * Since this is a monotonic chain increasing on the Y axis,
     * a binary search can be used however the overhead from calling this
     * (a sorting comparator for example) can be significant.
     * There is also some overhead from looking up vertex coordinates `co_of_index`.
     *
     * To minimize this overhead the following optimizations have been made:
     * - `calc_x_at_y` caches the result from the last call
     *   (gives 50-80% hit rate in my tests).
     * - This function caches the last "span", so the lookup can be skipped
     *   entirely when `co_y` is within the span.
     * - When the `co_y` is outside the span it's used to narrow the bisect rage.
     * - When `co_y` is before the next hit, an exponential search is used to
     *   set the upper bounds, to account for the sweep-lines incremental movement
     *   up the chains.
     */

    const int n = indices_num;

    if constexpr (DO_BINARY_SEARCH) {
      int lo = 0;
      int hi = n;

      /* Check if cached segment contains co_y.
       * Use half-open interval (y_of_index, y_of_index_next] to match binary search
       * semantics which finds first index where Y >= co_y.
       * Also handle exact match at y_of_index (returns index directly).
       * On cache miss, narrow the search range based on cached values. */
      if constexpr (DO_BINARY_SEARCH_CACHE) {
        if (bisect_cache.index != -1) {
          const Scalar y_of_index = (*bisect_cache.co_of_index[0])[1];
          const Scalar y_of_index_next = (*bisect_cache.co_of_index[1])[1];
          if (y_of_index < co_y && co_y <= y_of_index_next) {
            /* Cache hit: within segment or exact match at upper bound. */
            if (co_y == y_of_index_next) {
              r_co_pair[0] = nullptr;
              r_co_pair[1] = bisect_cache.co_of_index[1];
            }
            else {
              r_co_pair[0] = bisect_cache.co_of_index[0];
              r_co_pair[1] = bisect_cache.co_of_index[1];
            }
            return bisect_cache.index + 1;
          }
          if (co_y == y_of_index) {
            /* Cache hit: exact match at lower bound. */
            r_co_pair[0] = nullptr;
            r_co_pair[1] = bisect_cache.co_of_index[0];
            return bisect_cache.index;
          }

          if (co_y < y_of_index) {
            /* Result must be <= index. */
            hi = bisect_cache.index + 1;
          }
          else {
            lo = bisect_cache.index + 2;
            if (lo >= n) {
              /* Clamping `lo` is mainly a sanity check. */
              lo = n;
              /* Already set. */
              assert(hi == n);
            }
            /* `co_y` > `y_of_index_next`: result must be > index + 1. */
            else if (verts_table.co_at_index(indices[lo])[1] >= co_y) {
              /* Result is at `lo`, minimal search range. */
              hi = lo + 1;
            }
            else {
              /* Use exponential search to narrow the upper bound since the sweep
               * line moves monotonically upward, so the result is likely close
               * to the cached position. Check `Y[lo]` first as the most likely case. */
              int step = 1;
              while (lo + step < n && verts_table.co_at_index(indices[lo + step])[1] < co_y) {
                step *= 2;
              }
              hi = std::min(lo + step + 1, n);
            }
          }
        }
      }

      /* Binary search for first index with Y >= `co_y`. */
      while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (verts_table.co_at_index(indices[mid])[1] < co_y) {
          lo = mid + 1;
        }
        else {
          hi = mid;
        }
      }

      /* Update cache if we found a valid segment. */
      if constexpr (DO_BINARY_SEARCH_CACHE) {
        if (lo > 0 && lo < n) {
          bisect_cache.index = lo - 1;
          bisect_cache.co_of_index[0] = &verts_table.co_at_index(indices[lo - 1]);
          bisect_cache.co_of_index[1] = &verts_table.co_at_index(indices[lo]);
        }
      }

      if (lo == 0) {
        /* Check for exact match at first vertex. */
        const Vert *co_first = &verts_table.co_at_index(indices[0]);
        if (co_y == (*co_first)[1]) {
          r_co_pair[0] = nullptr;
          r_co_pair[1] = co_first;
          return 1; /* Treat as segment [0,1]. */
        }
        /* Out of bounds below: return first two vertices. */
        r_co_pair[0] = co_first;
        r_co_pair[1] = &verts_table.co_at_index(indices[1]);
        return -2;
      }
      if (lo == n) {
        /* Out of bounds above: return last two vertices. */
        r_co_pair[0] = &verts_table.co_at_index(indices[n - 2]);
        r_co_pair[1] = &verts_table.co_at_index(indices[n - 1]);
        return -1;
      }

      /* Valid segment found. */
      const Vert *co_curr = &verts_table.co_at_index(indices[lo]);
      if (co_y == (*co_curr)[1]) {
        r_co_pair[0] = nullptr;
        r_co_pair[1] = co_curr;
      }
      else {
        r_co_pair[0] = &verts_table.co_at_index(indices[lo - 1]);
        r_co_pair[1] = co_curr;
      }
      return lo;
    }
    else {
      /* Linear scan fallback. */
      /* It's common to request this value, so do it first. */
      const Vert *co_first = &verts_table.co_at_index(indices[0]);
      if (co_y <= (*co_first)[1]) {
        if (co_y == (*co_first)[1]) {
          r_co_pair[0] = nullptr;
          r_co_pair[1] = co_first;
          return 1;
        }
        /* Out of bounds below: return first two vertices. */
        r_co_pair[0] = co_first;
        r_co_pair[1] = &verts_table.co_at_index(indices[1]);
        return -2;
      }
      const Vert *prev = co_first;
      for (int i = 1; i < n; i++) {
        const Vert *co_curr = &verts_table.co_at_index(indices[i]);
        if (co_y <= (*co_curr)[1]) {
          if (co_y == (*co_curr)[1]) {
            r_co_pair[0] = nullptr;
            r_co_pair[1] = co_curr;
          }
          else {
            r_co_pair[0] = prev;
            r_co_pair[1] = co_curr;
          }
          return i;
        }
        prev = co_curr;
      }
      /* Out of bounds above: return last two vertices. */
      r_co_pair[0] = &verts_table.co_at_index(indices[n - 2]);
      r_co_pair[1] = &verts_table.co_at_index(indices[n - 1]);
      return -1;
    }
  }

  [[nodiscard]] Scalar calc_x_at_y(const Scalar co_y,
                                   const VertsTable &verts_table,
                                   const bool degenerate) const
  {
    /* Check simple lookup cache first. */
    if (x_at_y_cache.y == co_y) {
      return x_at_y_cache.x;
    }

    const Vert *co_pair[2];
    const int lo = find_segment_for_y(co_y, verts_table, co_pair);

    Scalar result;
    if (lo >= 0) {
      if (co_pair[0] == nullptr) {
        /* Exact match. */
        result = (*co_pair[1])[0];
      }
      else {
        /* Interpolate between `co_pair[0]` and `co_pair[1]`. */
        result = x_from_y_and_coords(co_y, *co_pair[0], *co_pair[1]);
      }
    }
    else {
      /* Failure mode: note that in this case there is no correct answer.
       * It would make sense to return #std::nullopt however in this case the caller
       * would need to deal with this corner case, adding unnecessary complexity.
       * It just so happens that returning the "closest" value is better than nothing,
       * so return the closest value here. */
      assert(degenerate);
      (void)degenerate;

      /* `co_pair[0]` is first vertex for below (-2), `co_pair[1]` is last vertex for above (-1).
       * Out-of-bounds always returns two valid vertices. */
      result = (lo == -2) ? (*co_pair[0])[0] : (*co_pair[1])[0];
    }

    /* Update cache. */
    x_at_y_cache.y = co_y;
    x_at_y_cache.x = result;

    return result;
  }

  [[nodiscard]] std::pair<Scalar, std::pair<const Vert *, const Vert *>> calc_x_at_y_with_coords(
      const Scalar co_y, const VertsTable &verts_table, const bool degenerate) const
  {
    const Vert *co_pair[2];
    const int lo = find_segment_for_y(co_y, verts_table, co_pair);

    std::pair<Scalar, std::pair<const Vert *, const Vert *>> result;

    if (lo >= 0) {
      if (co_pair[0] == nullptr) {
        /* Exact match. */
        const Vert *co_curr = co_pair[1];
        std::pair<const Vert *, const Vert *> coords;
        if (lo + 1 < indices_num) {
          coords = {co_curr, &verts_table.co_at_index(indices[lo + 1])};
        }
        else {
          /* It's not obvious what is correct in this case.
           * Use the previous vertex if available. */
          if (lo > 0) {
            const Vert *co_prev = &verts_table.co_at_index(indices[lo - 1]);
            coords = {co_prev, co_curr};
          }
          else {
            coords = {co_curr, co_curr};
          }
        }
        result = {(*co_curr)[0], coords};
      }
      else {
        /* Interpolate between `co_pair[0]` and `co_pair[1]`. */
        const Scalar co_x = x_from_y_and_coords(co_y, *co_pair[0], *co_pair[1]);
        result = {co_x, {co_pair[0], co_pair[1]}};
      }
    }
    else {
      /* See comments in `calc_x_at_y`. */
      assert(degenerate);
      (void)degenerate;

      /* `co_pair[0]` is first vertex for below (-2), `co_pair[1]` is last vertex for above (-1).
       * Out-of-bounds always returns two valid vertices. */
      const Vert *co_result = (lo == -2) ? co_pair[0] : co_pair[1];
      result = {(*co_result)[0], {co_pair[0], co_pair[1]}};
    }

    return result;
  }

  int split_at_y(const Scalar co_y, VertsTable &verts_table)
  {
    int v_index_prev = -1;
    /* This value is never used. */
    const Vert *co0 = nullptr;
    for (int i = 0; i < indices_num; i++) {
      const int v_index = indices[i];
      const Vert &co1 = verts_table.co_at_index(v_index);
      if (co_y == co1[1]) {
        /* Check against the last value as this happens
         * when there are multiple events sharing a Y axis. */
        const std::pair<int, int> div = {i, 0};
        if (splits.empty() || splits.back() != div) {
          assert(std::find(splits.begin(), splits.end(), div) == splits.end());
          splits.push_back(div);

          if (!needs_y_axis_aligned_bias_calc) {
            if ((i + 1 < indices_num) && (verts_table.co_at_index(indices[i + 1])[1] == co_y)) {
              needs_y_axis_aligned_bias_calc = true;
            }
          }
        }
        return v_index;
      }
      if (i > 0 && ((*co0)[1] < co_y && co_y < co1[1])) {
        const int v_index_tess = verts_table.ensure_tess_vert(v_index_prev, v_index, co_y);
        const std::pair<int, int> div = {i - 1, v_index_tess};
        if (splits.empty() || splits.back() != div) {
          assert(std::find(splits.begin(), splits.end(), div) == splits.end());
          splits.push_back(div);
        }
        return v_index_tess;
      }

      co0 = &co1;
      v_index_prev = v_index;
    }
    /* Should never happen! */
    assert(false);
    return 0;
  }

  [[nodiscard]] IndexSubset calc_split_indices(int split_index) const
  {
    std::pair<int, int> split_enter;
    std::pair<int, int> split_exit;

    if (split_index == -1) {
      split_enter = {0, 0};
      /* Is this needed? */
      if (!splits.empty()) {
        if (splits[0].first == 0 && splits[0].second == 0) {
          split_index = 1;
        }
      }
    }
    else {
      split_enter = splits[split_index];
    }

    /* -1 advances to zero which works well in this case. */
    /* If split_index + 1 doesn't exist, use the end of indices. */
    if (split_index + 1 < int(splits.size())) {
      split_exit = splits[split_index + 1];
    }
    else {
      assert(indices_num > 0);
      split_exit = {indices_num - 1, 0};
    }

    /* Calculate indices between enter/exit. */
    const int split_enter_index = split_enter.first;
    const int split_enter_vert = split_enter.second;
    const int split_exit_index = split_exit.first;
    const int split_exit_vert = split_exit.second;

    const int index_list_beg = split_enter_index + ((split_enter_vert == 0) ? 0 : 1);
    const int index_list_end = split_exit_index + 1;

    std::optional<int> index_beg = (split_enter_vert != 0) ? std::optional(split_enter_vert) :
                                                             std::nullopt;
    std::optional<int> index_end = (split_exit_vert != 0) ? std::optional(split_exit_vert) :
                                                            std::nullopt;

    return IndexSubset(
        {indices + index_list_beg, size_t(index_list_end - index_list_beg)}, index_beg, index_end);
  }

  /**
   * Seek for next indices on the same Y axis.
   */
  [[nodiscard]] std::optional<int> seek_next_indices_on_y(const int i,
                                                          const bool seek_x_positive,
                                                          const VertsTable &verts_table) const
  {
    const int v_index_base = indices[i];
    const Vert &co_base = verts_table.co_at_index(v_index_base);
    const Scalar y = co_base[1];

    /* Step into the next.
     * First, skip over any vertices with identical coordinates. */
    int i_test = i + 1;
    const int i_end = indices_num;
    while (i_test < i_end) {
      const Vert &co_test = verts_table.co_at_index(indices[i_test]);
      if (co_test[0] != co_base[0] || co_test[1] != co_base[1]) {
        /* The first different coordinate has been found. */
        break;
      }
      i_test++;
    }

    if (i_test >= i_end) {
      return std::nullopt;
    }

    /* Ensure it's aligned Y and in the right direction. */
    const Vert &co_first_diff = verts_table.co_at_index(indices[i_test]);
    if (co_first_diff[1] != y) {
      return std::nullopt;
    }

    bool x_ok = seek_x_positive ? (co_first_diff[0] > co_base[0]) :
                                  (co_first_diff[0] < co_base[0]);
    if (!x_ok) {
      /* Keep searching on the same Y axis. */
      return std::nullopt;
    }

    /* Found a valid match. Now continue searching to find the LAST vertex at the same Y. */
    int i_result = i_test;
    for (int i_seek = i_test + 1; i_seek < i_end; i_seek++) {
      const Vert &co_seek = verts_table.co_at_index(indices[i_seek]);
      if (co_seek[1] != y) {
        break; /* Y changed, stop searching. */
      }
      /* Update i_result to include ALL vertices at the same Y. */
      i_result = i_seek;
    }

    return i_result;
  }

  /**
   * Account for a split being added exactly existing vertices,
   * when there are multiple vertices on that Y axis.
   * In this case any of these vertices *could* be considered valid.
   *
   * Given these splits define regions to fill, once the value of ``VertChain.is_enter`` is known.
   * The region end-points are adjusted so that triangles will connect to the nearest verities
   * (event vertices in this case as they will have created the splits).
   */
  void apply_split_y_axis_aligned_bias(const VertsTable &verts_table)
  {
    assert(needs_y_axis_aligned_bias_calc);
    needs_y_axis_aligned_bias_calc = false;

    for (auto &[split_index, split_vert] : splits) {
      /* Ignore tessellated vertices as these will never have exactly adjacent vertices. */
      if (split_vert < 0) {
        continue;
      }
      /* Ensure it's not tessellated. */
      assert(indices[split_index] >= 0);
      const std::optional<int> i_next = seek_next_indices_on_y(split_index, is_enter, verts_table);
      if (i_next.has_value()) {
        /* Only re-assigning the first element is needed. */
        split_index = *i_next;
      }
    }
  }
};

/**
 * Determine if left/right chains should be swapped based on winding.
 *
 * \return 1 if swap needed (a > b), -1 if no swap (a < b), 0 if equal.
 */
[[nodiscard]] inline int region_order_swap(const VertChain *chain_l,
                                           const VertChain *chain_r,
                                           const VertsTable &verts_table)
{
  /* NOTE: winding is currently measured using a simplistic
   * check for the first edges the chain begins with.
   * This is quite a good test which would only fail in degenerate cases.
   * Nevertheless, when this is wrong it will produce flipped faces.
   * A "complete" test would be to calculating the winding of this region
   * as a polygon, however this is only needed if the polygon is self-intersecting. */

  assert(chain_l != chain_r);

  /* Degenerate, shouldn't happen but don't hard error if it does. */
  if (chain_l->indices_num <= 1 && chain_r->indices_num <= 1) [[unlikely]] {
    assert(false);
    return 0;
  }

  const Vert &co_l0 = verts_table.co_at_index(chain_l->indices[0]);

  if (chain_l->indices[0] != chain_r->indices[0]) [[unlikely]] {
    const Vert &co_r0 = verts_table.co_at_index(chain_r->indices[0]);
    if (co_l0 != co_r0) {
      /* Region ordering only makes sense when both regions start from the same vertex. */
      assert(false);
      return 0;
    }
  }

  const Vert &co_l1 = verts_table.co_at_index(chain_l->indices[1]);
  const Vert &co_r1 = verts_table.co_at_index(chain_r->indices[1]);
  const SignType sign = tri_v2_sign(co_l1, co_l0, co_r1);

  if (sign == SignType::Concave) {
    return 1;
  }
  if (sign == SignType::Convex) {
    return -1;
  }
  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name VertChain_List
 *
 * Doubly linked list with integrated RB tree.
 * \{ */

struct VertChain_List {
  VertChain *head = nullptr;
  VertChain *tail = nullptr;
  VertChain *rb_root = nullptr;

  [[nodiscard]] bool is_empty() const
  {
    return head == nullptr;
  }

  template<typename ValueFn> void add_tail(VertChain *x, ValueFn &&value_fn)
  {
    if (head == nullptr) {
      assert(tail == nullptr);
      assert(x->chain_prev == nullptr);
      assert(x->chain_next == nullptr);
      head = x;
      tail = x;
      if constexpr (DO_RB_TREE) {
        rb_insert(x, std::forward<ValueFn>(value_fn));
      }
      return;
    }

    assert(tail != nullptr && tail->chain_next == nullptr);
    assert(x->chain_prev == nullptr);
    assert(x->chain_next == nullptr);
    x->chain_prev = tail;
    x->chain_prev->chain_next = x;
    tail = x;

    if constexpr (DO_RB_TREE) {
      rb_insert(x, std::forward<ValueFn>(value_fn));
    }
  }

  template<typename ValueFn> void add_head(VertChain *x, ValueFn &&value_fn)
  {
    if (head == nullptr) {
      assert(tail == nullptr);
      assert(x->chain_prev == nullptr);
      assert(x->chain_next == nullptr);
      head = x;
      tail = x;
      if constexpr (DO_RB_TREE) {
        rb_insert(x, std::forward<ValueFn>(value_fn));
      }
      return;
    }

    assert(head != nullptr && head->chain_prev == nullptr);
    assert(x->chain_prev == nullptr);
    assert(x->chain_next == nullptr);
    x->chain_next = head;
    x->chain_next->chain_prev = x;
    head = x;

    if constexpr (DO_RB_TREE) {
      rb_insert(x, std::forward<ValueFn>(value_fn));
    }
  }

  /**
   * Add `item` after `other` in the list.
   */
  template<typename ValueFn> void add_after(VertChain *item, VertChain *other, ValueFn &&value_fn)
  {
    /* Ensure the item isn't already linked. */
    assert(item->chain_prev == nullptr);
    assert(item->chain_next == nullptr);

    VertChain *next_node = other->chain_next;

    /* Link `item` to its neighbors. */
    item->chain_prev = other;
    item->chain_next = next_node;

    /* Update `other` to point to `item`. */
    other->chain_next = item;

    /* Update the node following `other` to point back to `item`,
     * or update the list tail if `other` was the tail. */
    if (next_node != nullptr) {
      next_node->chain_prev = item;
    }
    else {
      tail = item;
    }

    if constexpr (DO_RB_TREE) {
      rb_insert_after(item, other, std::forward<ValueFn>(value_fn));
    }
  }

  /**
   * Add `item` before `other` in the list.
   */
  template<typename ValueFn> void add_before(VertChain *item, VertChain *other, ValueFn &&value_fn)
  {
    /* Ensure the item isn't already linked. */
    assert(item->chain_prev == nullptr);
    assert(item->chain_next == nullptr);

    VertChain *prev_node = other->chain_prev;

    /* Link `item` to its neighbors. */
    item->chain_next = other;
    item->chain_prev = prev_node;

    /* Update `other` to point back to `item`. */
    other->chain_prev = item;

    /* Update the node preceding `other` to point forward to `item`,
     * or update the list head if `other` was the beginning. */
    if (prev_node != nullptr) {
      prev_node->chain_next = item;
    }
    else {
      head = item;
    }

    if constexpr (DO_RB_TREE) {
      rb_insert_before(item, other, std::forward<ValueFn>(value_fn));
    }
  }

  void rem_link(VertChain *item)
  {
    if constexpr (DO_RB_TREE) {
      /* Remove from RB tree first. */
      rb_delete(item);
    }

    if (item == head) {
      assert(item->chain_prev == nullptr);
      if (item == tail) {
        head = nullptr;
        tail = nullptr;
        item->chain_next = nullptr;
        item->chain_prev = nullptr;
        return;
      }
      head = head->chain_next;
      assert(head != nullptr);
      head->chain_prev = nullptr;
      item->chain_next = nullptr;
      item->chain_prev = nullptr;
      return;
    }

    if (item == tail) {
      assert(item->chain_next == nullptr);
      tail = tail->chain_prev;
      assert(tail != nullptr);
      tail->chain_next = nullptr;
      item->chain_next = nullptr;
      item->chain_prev = nullptr;
      return;
    }

    assert(item->chain_next != nullptr);
    assert(item->chain_prev != nullptr);
    item->chain_next->chain_prev = item->chain_prev;
    item->chain_prev->chain_next = item->chain_next;
    item->chain_next = nullptr;
    item->chain_prev = nullptr;
  }

  /**
   * Find a node using the Red-Black tree and user comparator.
   *
   * Search for the value closest to `value` but never less than `value`.
   * (Standard BST Lower Bound / Ceiling).
   *
   * Special case for the sweep-line logic:
   * There may be duplicate values, so always scan for the lowest UUID.
   * This is done using the flat linked list order.
   */
  template<typename ValueFn>
  [[nodiscard]] VertChain *lookup_by_value_and_scan_for_min(const Scalar value,
                                                            ValueFn &&value_fn) const
  {
    if constexpr (DO_RB_TREE) {
      /* Search for the value closest to `value` but never less than `value`.
       * (Standard BST Lower Bound / Ceiling). */
      VertChain *chain_next = nullptr;
      VertChain *chain_other = rb_root;
      while (chain_other) {
        const int cmp = rb_value_cmp_expanded_value_only(
            value, chain_other, std::forward<ValueFn>(value_fn));
        if (cmp == 0) {
          /* Special case for the sweep-line logic,
           * there may be duplicate values, always scan for the lowest UUID.
           * Do this using the flat linked list order. */
          while (chain_other->rb_left != nullptr &&
                 rb_value_cmp_expanded_value_only(
                     value, chain_other->rb_left, std::forward<ValueFn>(value_fn)) == 0)
          {
            chain_other = chain_other->rb_left;
          }
          return chain_other;
        }
        else if (cmp < 0) {
          /* `value <= chain_other.value`. Current is a candidate (>= value).
           * We want the smallest of these, so try going left. */
          chain_next = chain_other;
          chain_other = chain_other->rb_left;
        }
        else {
          chain_other = chain_other->rb_right;
        }
      }
      return chain_next;
    }
    else {
      /* Slow, non-tree method. */
      for (VertChain *chain_other = head; chain_other; chain_other = chain_other->chain_next) {
        const Scalar co_other_x = value_fn(chain_other);
        if (co_other_x >= value) {
          return chain_other;
        }
      }
      return nullptr;
    }
  }

 private:
  /* -------------------------------------------------------------------------
   * RB-tree: Comparators.
   */
  template<typename ValueFn>
  static int rb_value_cmp_expanded_value_only(const Scalar value_a,
                                              const VertChain *item_b,
                                              ValueFn &&value_fn)
  {
    const Scalar value_b = value_fn(item_b);
    if (value_a < value_b) {
      return -1;
    }
    if (value_a > value_b) {
      return +1;
    }
    return 0;
  }

  template<typename ValueFn>
  static int rb_value_cmp(const VertChain *item_a, const VertChain *item_b, ValueFn &&value_fn)
  {
    assert(item_a != item_b);
    assert(item_a->rb_uid != item_b->rb_uid);

    const Scalar value_a = value_fn(item_a);
    const Scalar value_b = value_fn(item_b);
    if (value_a < value_b) {
      return -1;
    }
    if (value_a > value_b) {
      return +1;
    }

    if constexpr (DO_RB_TREE) {
      const uint32_t uid_a = item_a->rb_uid;
      const uint32_t uid_b = item_b->rb_uid;
      if (uid_a < uid_b) {
        return -1;
      }
      if (uid_a > uid_b) {
        return +1;
      }
    }

    assert(false); /* Unreachable. */
    return 0;
  }

  /* -------------------------------------------------------------------------
   * RB-tree: Helpers (rotations).
   */
  void rb_rotate_left(VertChain *x)
  {
    assert(x->rb_right != nullptr);
    VertChain *y = x->rb_right;
    x->rb_right = y->rb_left;
    if (y->rb_left) {
      y->rb_left->rb_parent = x;
    }
    y->rb_parent = x->rb_parent;
    if (x->rb_parent == nullptr) {
      rb_root = y;
    }
    else if (x == x->rb_parent->rb_left) {
      x->rb_parent->rb_left = y;
    }
    else {
      x->rb_parent->rb_right = y;
    }
    y->rb_left = x;
    x->rb_parent = y;
  }

  void rb_rotate_right(VertChain *x)
  {
    assert(x->rb_left != nullptr);
    VertChain *y = x->rb_left;
    x->rb_left = y->rb_right;
    if (y->rb_right) {
      y->rb_right->rb_parent = x;
    }
    y->rb_parent = x->rb_parent;
    if (x->rb_parent == nullptr) {
      rb_root = y;
    }
    else if (x == x->rb_parent->rb_right) {
      x->rb_parent->rb_right = y;
    }
    else {
      x->rb_parent->rb_left = y;
    }
    y->rb_right = x;
    x->rb_parent = y;
  }

  /* -------------------------------------------------------------------------
   * RB-tree: Insert.
   */
  template<typename ValueFn> void rb_insert(VertChain *node, ValueFn &&value_fn)
  {
    /* BST insert. */
    assert(node->rb_uid != 0);
    VertChain *parent = nullptr;
    VertChain *cur = rb_root;
    while (cur) {
      parent = cur;
      if (rb_value_cmp(node, cur, std::forward<ValueFn>(value_fn)) < 0) {
        cur = cur->rb_left;
      }
      else {
        cur = cur->rb_right;
      }
    }
    node->rb_parent = parent;

    if (parent == nullptr) {
      rb_root = node;
      node->rb_color = RBColor::Black;
      return;
    }
    else if (rb_value_cmp(node, parent, std::forward<ValueFn>(value_fn)) < 0) {
      parent->rb_left = node;
    }
    else {
      parent->rb_right = node;
    }

    rb_fix_insert(node);
  }

  /**
   * Optimized RB tree insertion when node is being inserted before other in the linked list.
   * Uses other as a hint - if node can be directly inserted as a child of other, this avoids
   * traversing from the root.
   */
  template<typename ValueFn>
  void rb_insert_before(VertChain *node, VertChain *other, ValueFn &&value_fn)
  {
    assert(node->rb_uid != 0);

    const int cmp_result = rb_value_cmp(node, other, std::forward<ValueFn>(value_fn));

    if (cmp_result > 0) {
      /* `node > other`: check if we can insert as `other.rb_right`. */
      if (other->rb_right == nullptr) {
        /* Verify node <= all ancestors where other is in left subtree. */
        bool valid = true;
        VertChain *ancestor = other->rb_parent;
        VertChain *cur = other;
        while (ancestor != nullptr) {
          if (cur == ancestor->rb_left) {
            /* `other` is in ancestor's left subtree, so we need node <= ancestor. */
            if (rb_value_cmp(node, ancestor, std::forward<ValueFn>(value_fn)) > 0) {
              valid = false;
              break;
            }
          }
          cur = ancestor;
          ancestor = ancestor->rb_parent;
        }

        if (valid) {
          other->rb_right = node;
          node->rb_parent = other;
          rb_fix_insert(node);
          return;
        }
      }
    }
    else {
      /* `node < other`: check if we can insert as `other.rb_left`. */
      if (other->rb_left == nullptr) {
        /* Verify node >= all ancestors where other is in right subtree. */
        bool valid = true;
        VertChain *ancestor = other->rb_parent;
        VertChain *cur = other;
        while (ancestor != nullptr) {
          if (cur == ancestor->rb_right) {
            /* `other` is in ancestor's right subtree, so we need `node >= ancestor`. */
            if (rb_value_cmp(node, ancestor, std::forward<ValueFn>(value_fn)) < 0) {
              valid = false;
              break;
            }
          }
          cur = ancestor;
          ancestor = ancestor->rb_parent;
        }

        if (valid) {
          other->rb_left = node;
          node->rb_parent = other;
          rb_fix_insert(node);
          return;
        }
      }
    }

    /* Fall back to standard BST insert from root. */
    rb_insert(node, std::forward<ValueFn>(value_fn));
  }

  /**
   * Optimized RB tree insertion when node is being inserted after other in the linked list.
   * Uses other as a hint - if node can be directly inserted as a child of other, this avoids
   * traversing from the root.
   */
  template<typename ValueFn>
  void rb_insert_after(VertChain *node, VertChain *other, ValueFn &&value_fn)
  {
    assert(node->rb_uid != 0);

    const int cmp_result = rb_value_cmp(node, other, std::forward<ValueFn>(value_fn));

    if (cmp_result > 0) {
      /* `node > other`: check if we can insert as `other.rb_right`. */
      if (other->rb_right == nullptr) {
        /* Verify node <= all ancestors where other is in left subtree. */
        bool valid = true;
        VertChain *ancestor = other->rb_parent;
        VertChain *cur = other;
        while (ancestor != nullptr) {
          if (cur == ancestor->rb_left) {
            /* `other` is in ancestor's left subtree, so we need node <= ancestor. */
            if (rb_value_cmp(node, ancestor, std::forward<ValueFn>(value_fn)) > 0) {
              valid = false;
              break;
            }
          }
          cur = ancestor;
          ancestor = ancestor->rb_parent;
        }

        if (valid) {
          other->rb_right = node;
          node->rb_parent = other;
          rb_fix_insert(node);
          return;
        }
      }
    }
    else {
      /* `node < other`: check if we can insert as `other.rb_left`. */
      if (other->rb_left == nullptr) {
        bool valid = true;
        VertChain *ancestor = other->rb_parent;
        VertChain *cur = other;
        while (ancestor != nullptr) {
          if (cur == ancestor->rb_right) {
            /* `other` is in ancestor's right subtree, so we need `node >= ancestor`. */
            if (rb_value_cmp(node, ancestor, std::forward<ValueFn>(value_fn)) < 0) {
              valid = false;
              break;
            }
          }
          cur = ancestor;
          ancestor = ancestor->rb_parent;
        }

        if (valid) {
          other->rb_left = node;
          node->rb_parent = other;
          rb_fix_insert(node);
          return;
        }
      }
    }

    /* Fall back to standard BST insert from root. */
    rb_insert(node, std::forward<ValueFn>(value_fn));
  }

  void rb_fix_insert(VertChain *x)
  {
    while (x->rb_parent && x->rb_parent->rb_color == RBColor::Red) {
      assert(x->rb_parent->rb_parent != nullptr);
      if (x->rb_parent == x->rb_parent->rb_parent->rb_left) {
        VertChain *y = x->rb_parent->rb_parent->rb_right;
        if (y && y->rb_color == RBColor::Red) {
          x->rb_parent->rb_color = RBColor::Black;
          y->rb_color = RBColor::Black;
          x->rb_parent->rb_parent->rb_color = RBColor::Red;
          x = x->rb_parent->rb_parent;
        }
        else {
          assert(x->rb_parent != nullptr);
          if (x == x->rb_parent->rb_right) {
            x = x->rb_parent;
            rb_rotate_left(x);
          }
          assert(x->rb_parent != nullptr);
          assert(x->rb_parent->rb_parent != nullptr);
          x->rb_parent->rb_color = RBColor::Black;
          x->rb_parent->rb_parent->rb_color = RBColor::Red;
          rb_rotate_right(x->rb_parent->rb_parent);
        }
      }
      else {
        assert(x->rb_parent != nullptr);
        assert(x->rb_parent->rb_parent != nullptr);
        VertChain *y = x->rb_parent->rb_parent->rb_left;
        if (y && y->rb_color == RBColor::Red) {
          x->rb_parent->rb_color = RBColor::Black;
          y->rb_color = RBColor::Black;
          x->rb_parent->rb_parent->rb_color = RBColor::Red;
          x = x->rb_parent->rb_parent;
        }
        else {
          assert(x->rb_parent != nullptr);
          if (x == x->rb_parent->rb_left) {
            x = x->rb_parent;
            rb_rotate_right(x);
          }
          assert(x->rb_parent != nullptr);
          x->rb_parent->rb_color = RBColor::Black;
          assert(x->rb_parent->rb_parent);
          x->rb_parent->rb_parent->rb_color = RBColor::Red;
          rb_rotate_left(x->rb_parent->rb_parent);
        }
      }
    }
    assert(rb_root != nullptr);
    rb_root->rb_color = RBColor::Black;
  }

  /* -------------------------------------------------------------------------
   * RB-tree: Delete.
   */
  void rb_transplant(VertChain *u, VertChain *v)
  {
    if (u->rb_parent == nullptr) {
      rb_root = v;
    }
    else if (u == u->rb_parent->rb_left) {
      u->rb_parent->rb_left = v;
    }
    else {
      u->rb_parent->rb_right = v;
    }
    if (v) {
      v->rb_parent = u->rb_parent;
    }
  }

  static VertChain *rb_tree_minimum(VertChain *x)
  {
    while (x->rb_left) {
      x = x->rb_left;
    }
    return x;
  }

  void rb_delete(VertChain *z)
  {
    VertChain *y = z;
    RBColor y_original_color = y->rb_color;
    VertChain *x = nullptr;
    VertChain *x_parent = nullptr;

    if (z->rb_left == nullptr) {
      x = z->rb_right;
      x_parent = z->rb_parent;
      rb_transplant(z, z->rb_right);
    }
    else if (z->rb_right == nullptr) {
      x = z->rb_left;
      x_parent = z->rb_parent;
      rb_transplant(z, z->rb_left);
    }
    else {
      y = rb_tree_minimum(z->rb_right);
      y_original_color = y->rb_color;
      x = y->rb_right;
      if (y->rb_parent == z) {
        x_parent = y;
      }
      else {
        x_parent = y->rb_parent;
        rb_transplant(y, y->rb_right);
        y->rb_right = z->rb_right;
        y->rb_right->rb_parent = y;
      }
      rb_transplant(z, y);
      y->rb_left = z->rb_left;
      y->rb_left->rb_parent = y;
      y->rb_color = z->rb_color;
    }

    z->rb_left = nullptr;
    z->rb_right = nullptr;
    z->rb_parent = nullptr;

    if (y_original_color == RBColor::Black) {
      rb_fix_delete(x, x_parent);
    }
  }

  void rb_fix_delete(VertChain *x, VertChain *x_parent)
  {
    while (x != rb_root && (x == nullptr || x->rb_color == RBColor::Black)) {
      if (x_parent == nullptr) {
        break;
      }
      if (x == x_parent->rb_left) {
        VertChain *w = x_parent->rb_right;
        if (w && w->rb_color == RBColor::Red) {
          w->rb_color = RBColor::Black;
          x_parent->rb_color = RBColor::Red;
          rb_rotate_left(x_parent);
          w = x_parent->rb_right;
        }
        if (w == nullptr || ((w->rb_left == nullptr || w->rb_left->rb_color == RBColor::Black) &&
                             (w->rb_right == nullptr || w->rb_right->rb_color == RBColor::Black)))
        {
          if (w) {
            w->rb_color = RBColor::Red;
          }
          x = x_parent;
          x_parent = x->rb_parent;
        }
        else {
          if (w->rb_right == nullptr || w->rb_right->rb_color == RBColor::Black) {
            if (w->rb_left) {
              w->rb_left->rb_color = RBColor::Black;
            }
            w->rb_color = RBColor::Red;
            rb_rotate_right(w);
            w = x_parent->rb_right;
          }
          if (w) {
            w->rb_color = x_parent->rb_color;
          }
          x_parent->rb_color = RBColor::Black;
          if (w && w->rb_right) {
            w->rb_right->rb_color = RBColor::Black;
          }
          rb_rotate_left(x_parent);
          x = rb_root;
          x_parent = nullptr;
        }
      }
      else {
        VertChain *w = x_parent->rb_left;
        if (w && w->rb_color == RBColor::Red) {
          w->rb_color = RBColor::Black;
          x_parent->rb_color = RBColor::Red;
          rb_rotate_right(x_parent);
          w = x_parent->rb_left;
        }
        if (w == nullptr || ((w->rb_right == nullptr || w->rb_right->rb_color == RBColor::Black) &&
                             (w->rb_left == nullptr || w->rb_left->rb_color == RBColor::Black)))
        {
          if (w) {
            w->rb_color = RBColor::Red;
          }
          x = x_parent;
          x_parent = x->rb_parent;
        }
        else {
          if (w->rb_left == nullptr || w->rb_left->rb_color == RBColor::Black) {
            if (w->rb_right) {
              w->rb_right->rb_color = RBColor::Black;
            }
            w->rb_color = RBColor::Red;
            rb_rotate_left(w);
            w = x_parent->rb_left;
          }
          if (w) {
            w->rb_color = x_parent->rb_color;
          }
          x_parent->rb_color = RBColor::Black;
          if (w && w->rb_left) {
            w->rb_left->rb_color = RBColor::Black;
          }
          rb_rotate_right(x_parent);
          x = rb_root;
          x_parent = nullptr;
        }
      }
    }
    if (x) {
      x->rb_color = RBColor::Black;
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper functions for face validation and appending
 * \{ */

[[nodiscard]] inline bool face_is_valid(const Face &f, const VertsTable &verts_table)
{
  const Vert &co0 = verts_table.co_at_index(f[0]);
  const Vert &co1 = verts_table.co_at_index(f[1]);
  const Vert &co2 = verts_table.co_at_index(f[2]);
  return tri_v2_sign(co0, co1, co2) != SignType::Concave;
}

/** Overload taking pre-fetched coordinates to avoid redundant lookups. */
[[nodiscard]] inline bool face_is_valid_co(const Vert &co0, const Vert &co1, const Vert &co2)
{
  return tri_v2_sign(co0, co1, co2) != SignType::Concave;
}

inline void faces_append_with_checks(FaceConstructContext &fctx,
                                     const int v0,
                                     const int v1,
                                     const int v2)
{
  std::vector<FaceResult> &faces_result = fctx.faces_result;
  const VertsTable &verts_table = fctx.verts_table;
  [[maybe_unused]] const PolyFillParams &params = fctx.params;

  /* Debug assertions (match Python's assert statements). */
  assert(params.degenerate || (v0 != v1 && v0 != v2 && v1 != v2));

  if (v0 == v1 || v1 == v2 || v0 == v2) [[unlikely]] {
    return;
  }

  const Vert &co0 = verts_table.co_at_index(v0);
  const Vert &co1 = verts_table.co_at_index(v1);
  const Vert &co2 = verts_table.co_at_index(v2);

  /* Skip zero-area faces where all vertices are on the same Y (matching Python). */
  if constexpr (DO_FANFILL_AXIS_ALIGNED && DO_FANFILL_AXIS_ALIGNED_SKIP) {
    if (co0[1] == co1[1] && co1[1] == co2[1]) [[unlikely]] {
      return;
    }
  }

  const SignType sign = tri_v2_sign(co0, co1, co2);

  /* Debug assertion for non-degenerate mode. */
  assert(params.degenerate || sign != SignType::Concave);

  if (sign == SignType::Concave) [[unlikely]] {
    return;
  }

  /* Debug assertion for axis-aligned faces. */
  if constexpr (DO_FANFILL_AXIS_ALIGNED) {
    assert(params.degenerate || !(co0[1] == co1[1] && co1[1] == co2[1]));
  }

  /* Note: TANGENTIAL triangles are kept (matching Python behavior). */

  faces_result.emplace_back(Face{v0, v1, v2});
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FacePair
 *
 * Pair of faces sharing an edge (for edge rotation).
 * \{ */

/**
 * Pair of faces sharing an edge at a tessellated vertex.
 * Used during edge rotation: f_a and f_b share edge (v_pivot, v_other).
 * When f_a == f_b, the edge is a boundary (only one adjacent face).
 */
struct FacePair {
  FaceResult *f_a;
  FaceResult *f_b;

  FacePair(FaceResult *a, FaceResult *b) : f_a(a), f_b(b) {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forward declarations for edge rotation
 * \{ */

std::vector<int> rotate_edges_away_pass(VertsTable &verts_table,
                                        std::vector<FaceResult> &faces_result,
                                        std::vector<std::vector<FaceResult *>> &coords_face_vec,
                                        std::span<const int> verts_tess_to_handle,
                                        bool force_collapse_degenerate);

void rotate_edges_away(VertsTable &verts_table, std::vector<FaceResult> &faces_result);

/** \} */

/* -------------------------------------------------------------------- */
/** \name EventBisectCacheY
 *
 * Cache for binary search lower bound in #calc_fan_from_events.
 * \{ */

/**
 * Cache for binary search lower bound in #calc_fan_from_events.
 * Since the scan-line moves up on the Y axis, we can reuse the previous
 * search result as a starting point for the next search.
 * Only used when #DO_BINARY_SEARCH_CACHE is true.
 */
struct EventBisectCacheY {
  /** Index of -1 indicates the cache is unset. */
  int index = -1;
  Scalar y = 0.0;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name SweepInterval
 *
 * Main sweep line algorithm state.
 * \{ */

/**
 * A kind of sweep-line, however it sweeps an interval,
 * resulting in segments which can be evaluated.
 */
struct SweepInterval {
  const PolyFillParams &params;

  VertsTable &verts_table;
  const VertsEdgeMap &verts_edge_map;

  VertChain_List chain_list;
  std::vector<Event> events;
  int event_index = 0;

  std::vector<EventType> verts_event;
  /** Maps vertex index to event index. Uses -1 for vertices not in any event. */
  std::vector<int> verts_event_map;

  /**
   * RB-tree UID, only ever increase.
   * This value can be assigned to the next `VertChain`, then increased.
   */
  uint32_t rb_uid_value_max = 1;

  /**
   * List of data needed for creating a new filled area.
   * Each entry is `((left_chain, split_index), (right_chain, split_index))`.
   */
  std::vector<std::pair<std::pair<VertChain *, int>, std::pair<VertChain *, int>>> span_list;

  /** Storage for all chains, freed in destructor. */
  std::vector<VertChain *> chains_storage;

  SweepInterval(const PolyFillParams &params_in,
                VertsTable &verts_table_in,
                const VertsEdgeMap &verts_edge_map_in)
      : params(params_in), verts_table(verts_table_in), verts_edge_map(verts_edge_map_in)
  {
    events = events_calc(verts_table.coords, verts_edge_map);

    verts_event.resize(verts_table.coords.size(), EventType::None);
    for (const auto &e : events) {
      for (const int j : e.vert_index_list) {
        verts_event[j] = e.ty;
      }
    }

    verts_event_map.resize(verts_table.coords.size(), -1);
    for (int i = 0; i < int(events.size()); i++) {
      for (const int j : events[i].vert_index_list) {
        verts_event_map[j] = i;
      }
    }

    span_list.reserve(events.size());
  }

  ~SweepInterval()
  {
    for (VertChain *chain : chains_storage) {
      VertChain::destroy(chain);
    }
  }

  /* Non-copyable, non-movable. */
  SweepInterval(const SweepInterval &) = delete;
  SweepInterval &operator=(const SweepInterval &) = delete;
  SweepInterval(SweepInterval &&) = delete;
  SweepInterval &operator=(SweepInterval &&) = delete;

  VertChain *create_chain(const int start_index, VertStep step)
  {
    std::vector<int> indices = build_indices_until_event(
        start_index, step, verts_table, verts_edge_map, verts_event, params.degenerate);
    VertChain *chain = VertChain::create(indices);
    chains_storage.push_back(chain);
    return chain;
  }

  static std::vector<Event> events_calc(std::span<const Vert> verts,
                                        const VertsEdgeMap &verts_edge_map)
  {
    std::vector<Event> result;
    /* Minimum 2 events (enter/exit) for closed polygons; shape-dependent so not worth estimating.
     */
    result.reserve(2);

    assert(verts.size() == verts_edge_map.size());
    for (int i = 0; i < int(verts_edge_map.size()); i++) {
      int j = verts_edge_map[i][0];
      int k = verts_edge_map[i][1];

      const Vert &co_b = verts[i];

      /* Compute sign first, then swap indices if needed to avoid copying vertices. */
      const SignType sign = tri_v2_sign(verts[j], co_b, verts[k]);
      if (sign == SignType::Concave) {
        std::swap(j, k);
      }

      /* Now access coordinates by reference. */
      const Vert &co_a = verts[j];
      const Vert &co_c = verts[k];

      if (co_a[1] > co_b[1]) {
        if (co_c[1] > co_b[1]) {
          result.emplace_back(i, EventType::Enter);
        }
        else if (co_c[1] == co_b[1]) {
          const int i_next = edge_other_until_y_axis_changes(i, k, verts, verts_edge_map);
          if (verts[i_next][1] > co_b[1]) {
            result.emplace_back(i, EventType::Enter);
          }
        }
      }
      else if (co_a[1] < co_b[1]) {
        if (co_c[1] < co_b[1]) {
          result.emplace_back(i, EventType::Exit);
        }
        else if (co_c[1] == co_b[1]) {
          const int i_next = edge_other_until_y_axis_changes(i, k, verts, verts_edge_map);
          if (verts[i_next][1] < co_b[1]) {
            result.emplace_back(i, EventType::Exit);
          }
        }
      }
    }

    std::sort(result.begin(), result.end(), [&verts](const Event &a, const Event &b) {
      const Vert &co_a = verts[a.vert_index_list.first_unchecked()];
      const Vert &co_b = verts[b.vert_index_list.first_unchecked()];
      if (co_a[1] != co_b[1]) {
        return co_a[1] < co_b[1];
      }
      if (a.ty != b.ty) {
        return a.ty < b.ty;
      }
      if (co_a[0] != co_b[0]) {
        return co_a[0] < co_b[0];
      }
      return a.vert_index_list.first_unchecked() < b.vert_index_list.first_unchecked();
    });

    /* Merge events at the same location - O(n) two-pointer compaction. */
    if (result.size() > 1) {
      int write = 1;
      for (int read = 1; read < int(result.size()); read++) {
        const Vert &co_write = verts[result[write - 1].vert_index_list.first_unchecked()];
        const Vert &co_read = verts[result[read].vert_index_list.first_unchecked()];
        if (co_write == co_read && result[write - 1].ty == result[read].ty) {
          assert(result[read].vert_index_list.size() == 1);
          result[write - 1].vert_index_list.push_back(
              result[read].vert_index_list.first_unchecked());
        }
        else {
          if (write != read) {
            result[write] = std::move(result[read]);
          }
          write++;
        }
      }
      result.erase(result.begin() + ptrdiff_t(write), result.end());
    }

    return result;
  }

  /** Check if any ENTER event falls within the X range (x_min, x_max). */
  [[nodiscard]] bool has_event_in_x_range(int event_beg,
                                          int event_end,
                                          Scalar x_min,
                                          Scalar x_max) const
  {
    assert(event_beg <= event_end);
    if (event_beg == event_end) {
      return false;
    }
    assert(events[event_beg].co(verts_table.coords)[1] ==
           events[event_end - 1].co(verts_table.coords)[1]);
    assert(x_min != x_max);

    if constexpr (DO_BINARY_SEARCH) {
      if (events[event_beg].co(verts_table.coords)[0] >= x_max) {
        return false;
      }
      if (events[event_end - 1].co(verts_table.coords)[0] <= x_min) {
        return false;
      }

      auto it = std::upper_bound(
          events.begin() + ptrdiff_t(event_beg),
          events.begin() + ptrdiff_t(event_end),
          x_min,
          [this](Scalar val, const Event &e) { return val < e.co(verts_table.coords)[0]; });

      if (it != events.begin() + ptrdiff_t(event_end)) {
        assert(it->ty == EventType::Enter);
        if (it->co(verts_table.coords)[0] < x_max) {
          return true;
        }
      }
      return false;
    }
    else {
      for (int i = event_beg; i < event_end; i++) {
        const Event &e = events[i];
        if (e.co(verts_table.coords)[0] > x_min && e.co(verts_table.coords)[0] < x_max) {
          return true;
        }
      }
      return false;
    }
  }

  /**
   * Calculate fan vertices from events at the same Y coordinate.
   *
   * This function finds events (local minima/maxima) that fall on the same
   * Y coordinate as the edge defined by `vert_index_beg` and `vert_index_end`,
   * and returns any intermediate vertices that need to be incorporated via
   * fan-filling to avoid creating degenerate or overlapping triangles.
   *
   * NOTE: we might want to store a lookup for this,
   * as it happens bisecting is quite fast and this only runs on beginning
   * & end-points of regions to be filled (not on every vertex - where a
   * direct lookup would be more appropriate).
   * So this may be acceptable as-is.
   */
  [[nodiscard]] std::vector<int> calc_fan_from_events(int vert_index_beg,
                                                      int vert_index_end,
                                                      EventBisectCacheY &event_bisect_cache,
                                                      const bool event_bisect_cache_update) const
  {
    std::vector<int> result;
    result.reserve(8);

    const Vert &co0 = verts_table.co_at_index(vert_index_beg);
    const Vert &co1 = verts_table.co_at_index(vert_index_end);
    assert(co0[1] == co1[1]);

    const Scalar y = co0[1];
    const Scalar x_beg = co0[0];
    const Scalar x_end = co1[0];

    /* Use the bisect cache to optimize the search.
     * The scanline moves up on the Y axis, so it's beneficial to keep increasing
     * the "low" value. If the cached index is valid (not -1) and the cached Y value
     * is less than or equal to the Y value being searched, use the cached index
     * as the "low" argument to bisect to skip already-processed events.
     * Also use exponential search to narrow the upper bound since the next Y
     * is likely close to the cached position.
     * If cached Y > y, the result must be before the cached index. */
    int search_low = 0;
    int search_hi = int(events.size());
    if constexpr (DO_BINARY_SEARCH_CACHE) {
      if (event_bisect_cache.index != -1) {
        if (event_bisect_cache.y <= y) {
          search_low = event_bisect_cache.index;
          /* Exponential search for upper bound. */
          if (search_low < search_hi) {
            if (events[search_low].co(verts_table.coords)[1] >= y) {
              /* Result is at search_low, minimal search range. */
              search_hi = search_low + 1;
            }
            else {
              int step = 1;
              while (search_low + step < search_hi &&
                     events[search_low + step].co(verts_table.coords)[1] < y)
              {
                step *= 2;
              }
              search_hi = std::min(search_low + step + 1, search_hi);
            }
          }
        }
        else {
          /* Cached Y > y: result must be at or before cached index.
           * Clamp to events.size() in case cache.index == events.size()
           * (which happens when previous search found no events >= cache.y). */
          search_hi = std::min(event_bisect_cache.index + 1, search_hi);
        }
      }
    }

    /* Binary search for first event at or above Y, starting from cached low bound. */
    auto it = std::lower_bound(
        events.begin() + search_low,
        events.begin() + search_hi,
        y,
        [&](const Event &e, Scalar val) { return e.co(verts_table.coords)[1] < val; });
    search_low = int(it - events.begin());

    if constexpr (DO_BINARY_SEARCH_CACHE) {
      if (event_bisect_cache_update) {
        /* Update the cache with the result of this bisect call.
         * This allows subsequent calls with Y >= co0[1] to skip events before e_index_beg. */
        event_bisect_cache.index = search_low;
        event_bisect_cache.y = y;
      }
    }

    if (it == events.end() || it->co(verts_table.coords)[1] != y) {
      return result;
    }

    for (; it != events.end() && it->co(verts_table.coords)[1] == y; ++it) {
      const Event &e = *it;

      /* Skip if this event contains the beg or end vertex. */
      bool skip = false;
      for (const int v : e.vert_index_list) {
        if (v == vert_index_beg || v == vert_index_end) {
          skip = true;
          break;
        }
      }
      if (skip) {
        continue;
      }

      const Scalar x_event = e.co(verts_table.coords)[0];
      if (x_beg < x_event && x_event < x_end) {
        if (!DO_FANFILL_AXIS_ALIGNED) {
          result.push_back(e.vert_index_list[0]);
          continue;
        }

        const int e_vert_index = e.vert_index_list[0];
        if (e.ty == EventType::Exit) {
          std::vector<int> fan_temp;
          fan_temp.push_back(e_vert_index);
          /* Collect any prior Y-aligned vertices. */
          bool ok = false;
          for (const int v_prev : verts_edge_map[e_vert_index]) {
            const Vert &co_prev = verts_table.co_at_index(v_prev);
            if (co_prev[1] == y && x_beg < co_prev[0] && co_prev[0] < x_event) {
              fan_temp.push_back(v_prev);
              ok = true;
              break;
            }
          }
          while (ok) {
            ok = false;
            const int last_v = fan_temp.back();
            const int prev_v = fan_temp[fan_temp.size() - 2];
            for (const int v_prev : verts_edge_map[last_v]) {
              if (v_prev != prev_v) {
                const Vert &co_prev = verts_table.co_at_index(v_prev);
                if (co_prev[1] == y && x_beg < co_prev[0] &&
                    co_prev[0] < verts_table.co_at_index(last_v)[0])
                {
                  fan_temp.push_back(v_prev);
                  ok = true;
                  break;
                }
              }
            }
          }
          std::reverse(fan_temp.begin(), fan_temp.end());
          result.insert(result.end(), fan_temp.begin(), fan_temp.end());
        }
        else if (e.ty == EventType::Enter) {
          result.push_back(e_vert_index);

          /* Collect any following Y-aligned vertices. */
          bool ok = false;
          for (const int v_next : verts_edge_map[e_vert_index]) {
            const Vert &co_next = verts_table.co_at_index(v_next);
            if (co_next[1] == y && x_event < co_next[0] && co_next[0] < x_end) {
              result.push_back(v_next);
              ok = true;
              break;
            }
          }
          while (ok) {
            ok = false;
            const int last_v = result.back();
            const int prev_v = result[result.size() - 2];
            for (const int v_next : verts_edge_map[last_v]) {
              if (v_next != prev_v) {
                const Vert &co_next = verts_table.co_at_index(v_next);
                if (co_next[1] == y && verts_table.co_at_index(last_v)[0] < co_next[0] &&
                    co_next[0] < x_end)
                {
                  result.push_back(v_next);
                  ok = true;
                  break;
                }
              }
            }
          }
        }
      }
    }

    return result;
  }

  bool step_event();

  std::pair<int, int> fill_sweep_or_clip(FaceConstructContext &fctx,
                                         const IndexSubset &indices_l,
                                         const IndexSubset &indices_r,
                                         int c_index_l,
                                         int c_index_r,
                                         int c_end_l,
                                         int c_end_r);

  void fill_region(FaceConstructContext &fctx,
                   const IndexSubset &indices_l,
                   const IndexSubset &indices_r,
                   const std::vector<int> &fan_beg,
                   const std::vector<int> &fan_end,
                   [[maybe_unused]] void *edges_result_for_debug = nullptr);

  /**
   * Fill a region of indices using an ear clipping method.
   * This is needed because there are situations where `fill_region`
   * cannot fill triangles by scanning up the indices.
   *
   * This occurs when one side defines a region has an outward protrusion (a lump)
   * on a mostly vertical line. When the opposite side of this lump has no vertices,
   * it's possible a single triangle from the "lump" to the opposite edge will
   * overlap vertices on the side of the lump.
   *
   * In this case it's important to use ear clipping to fill in these regions
   * so it's possible to sweep-fill the un-filled space left by this function.
   */
  void fill_with_ear_clip_strategy(FaceConstructContext &fctx,
                                   const IndexSubset &indices_l,
                                   const IndexSubset &indices_r);

  /**
   * Iterate over all regions and call the provided function for each one.
   * This mirrors the Python `for region in si.step_regions()` pattern.
   *
   * The function is called with a VertChain_RegionInfo struct containing:
   * - indices_l, indices_r: Left and right chain indices
   * - fan_beg, fan_end: Fan vertices at region endpoints
   */
  template<typename Fn> void step_regions(Fn &&region_fn)
  {
    EventBisectCacheY event_bisect_cache;

    /* Process regions from enter events (chains that span the full height). */
    for (const Event &e : events) {
      if (e.ty != EventType::Enter) {
        continue;
      }
      assert(!e.chains_beg.empty());

      for (auto &[chain_l, chain_r] : e.chains_beg) {
        if (chain_l->needs_y_axis_aligned_bias_calc) {
          chain_l->apply_split_y_axis_aligned_bias(verts_table);
        }
        if (chain_r->needs_y_axis_aligned_bias_calc) {
          chain_r->apply_split_y_axis_aligned_bias(verts_table);
        }

        if (!chain_l->is_enter) {
          continue;
        }

        std::vector<int> fan_beg, fan_end;
        if (!chain_l->splits.empty() || !chain_r->splits.empty()) {
          std::optional<int> index_end_l, index_end_r;
          int index_list_len_l, index_list_len_r;

          if (chain_l->splits.empty()) {
            index_list_len_l = chain_l->indices_num;
          }
          else {
            index_list_len_l = chain_l->splits[0].first + 1;
            if (chain_l->splits[0].second != 0) {
              index_end_l = chain_l->splits[0].second;
            }
          }

          if (chain_r->splits.empty()) {
            index_list_len_r = chain_r->indices_num;
          }
          else {
            index_list_len_r = chain_r->splits[0].first + 1;
            if (chain_r->splits[0].second != 0) {
              index_end_r = chain_r->splits[0].second;
            }
          }

          IndexSubset indices_l(
              {chain_l->indices, size_t(index_list_len_l)}, std::nullopt, index_end_l);
          IndexSubset indices_r(
              {chain_r->indices, size_t(index_list_len_r)}, std::nullopt, index_end_r);

          if constexpr (DO_FANFILL_BY_REGION_ENDPOINTS) {
            if (indices_l.at_last() != indices_r.at_last()) {
              /* Don't update cache: the end of the fan is likely to skip too far forward. */
              fan_end = calc_fan_from_events(
                  indices_l.at_last(), indices_r.at_last(), event_bisect_cache, false);
            }
          }

          region_fn(
              VertChain_RegionInfo(indices_l, indices_r, std::move(fan_beg), std::move(fan_end)));
        }
        else {
          assert(!chain_r->is_enter);
          IndexSubset indices_l({chain_l->indices, size_t(chain_l->indices_num)});
          IndexSubset indices_r({chain_r->indices, size_t(chain_r->indices_num)});

          region_fn(
              VertChain_RegionInfo(indices_l, indices_r, std::move(fan_beg), std::move(fan_end)));
        }
      }
    }

    /* Process span list (regions split by intermediate Y coordinates). */
    for (auto &[span_l, span_r] : span_list) {
      const VertChain *chain_l = span_l.first;
      const int split_index_l = span_l.second;
      const VertChain *chain_r = span_r.first;
      const int split_index_r = span_r.second;

      IndexSubset indices_l = chain_l->calc_split_indices(split_index_l);
      IndexSubset indices_r = chain_r->calc_split_indices(split_index_r);

      std::vector<int> fan_beg, fan_end;
      if constexpr (DO_FANFILL_BY_REGION_ENDPOINTS) {
        if (indices_l.at_first() != indices_r.at_first()) {
          fan_beg = calc_fan_from_events(
              indices_l.at_first(), indices_r.at_first(), event_bisect_cache, true);
        }
        if (indices_l.at_last() != indices_r.at_last()) {
          /* Don't update cache: the end of the fan is likely to skip too far forward. */
          fan_end = calc_fan_from_events(
              indices_l.at_last(), indices_r.at_last(), event_bisect_cache, false);
        }
      }

      region_fn(
          VertChain_RegionInfo(indices_l, indices_r, std::move(fan_beg), std::move(fan_end)));
    }
  }
};

bool SweepInterval::step_event()
{
  if (event_index >= int(events.size())) [[unlikely]] {
    return false;
  }

  const int event_index_beg = event_index;

  int event_exit_beg = 0;
  int event_exit_end = 0;
  int event_enter_beg = 0;
  int event_enter_end = 0;

  const Scalar co_y = events[event_index].co(verts_table.coords)[1];

  while (event_index < int(events.size())) {
    const Event &e = events[event_index];
    if (event_index > event_index_beg) {
      if (co_y != e.co(verts_table.coords)[1]) {
        break;
      }
    }
    if (e.ty == EventType::Exit) {
      if (event_exit_beg == event_exit_end) {
        event_exit_beg = event_index;
        event_exit_end = event_index + 1;
      }
      else {
        event_exit_end = event_index + 1;
      }
    }
    else if (e.ty == EventType::Enter) {
      if (event_enter_beg == event_enter_end) {
        event_enter_beg = event_index;
        event_enter_end = event_index + 1;
      }
      else {
        event_enter_end = event_index + 1;
      }
    }
    event_index++;
  }

  for (int index = event_exit_beg; index < event_exit_end; index++) {
    assert(events[index].ty == EventType::Exit);
    for (VertChain *chain_iter : events[index].chains_end) {
      chain_list.rem_link(chain_iter);
    }
  }

  /* Setup new chains. */
  for (int index = event_enter_beg; index < event_enter_end; index++) {
    Event &e = events[index];

    assert(!e.vert_index_list.empty());
    for (const int vert_index_curr : e.vert_index_list) {
      const int vert_index_prev = verts_edge_map[vert_index_curr][0];
      const int vert_index_next = verts_edge_map[vert_index_curr][1];

      VertChain *chain_l = create_chain(vert_index_curr,
                                        VertStep(vert_index_prev, vert_index_curr));
      VertChain *chain_r = create_chain(vert_index_curr,
                                        VertStep(vert_index_next, vert_index_curr));

      e.chains_beg.push_back({chain_l, chain_r});

      for (VertChain *chain_iter : {chain_l, chain_r}) {
        const int event_index_end =
            verts_event_map[chain_iter->indices[chain_iter->indices_num - 1]];
        events[event_index_end].chains_end.push_back(chain_iter);
      }
    }

    /* Fast-path for common case of a single vertex for one event. */
    if (e.vert_index_list.size() == 1) {
      /* Order pairs. */
      if (region_order_swap(e.chains_beg[0].first, e.chains_beg[0].second, verts_table) > 0) {
        std::swap(e.chains_beg[0].first, e.chains_beg[0].second);
      }
    }
    else {
      /* Sort chains that all start from the same event (sharing an initial vertex location).
       * This can be complex as these chains can define a "tree" structure,
       * with some chain pairs being inside others, or adjacent to. */

      /* 1) Order pairs. */
      for (auto &chain_pair : e.chains_beg) {
        assert(chain_pair.first->indices[0] == chain_pair.second->indices[0]);
        if (region_order_swap(chain_pair.first, chain_pair.second, verts_table) > 0) {
          std::swap(chain_pair.first, chain_pair.second);
        }
      }

      /* 2) Sort the pairs with respect to each other. */
      std::stable_sort(e.chains_beg.begin(),
                       e.chains_beg.end(),
                       [&verts_table = verts_table](const std::pair<VertChain *, VertChain *> &a,
                                                    const std::pair<VertChain *, VertChain *> &b) {
                         return region_order_swap(a.first, b.first, verts_table) < 0;
                       });

      /* 3) More involved path for events with multiple vertices. */
      std::vector<VertChain *> chains_flat;
      chains_flat.reserve(e.chains_beg.size() * 2);
      for (const auto &chain_pair : e.chains_beg) {
        chains_flat.push_back(chain_pair.first);
        chains_flat.push_back(chain_pair.second);
      }

      std::stable_sort(chains_flat.begin(),
                       chains_flat.end(),
                       [&verts_table = verts_table](VertChain *a, VertChain *b) {
                         return region_order_swap(a, b, verts_table) < 0;
                       });

      /* Re-pair up sorted chains. */
      e.chains_beg.clear();
      for (size_t i = 0; i < chains_flat.size(); i += 2) {
        e.chains_beg.push_back({chains_flat[i], chains_flat[i + 1]});
      }
    }

    /* Assign UIDs as tie breakers. */
    if constexpr (DO_RB_TREE) {
      for (auto &[chain_l, chain_r] : e.chains_beg) {
        chain_l->rb_uid = rb_uid_value_max;
        rb_uid_value_max++;
        chain_r->rb_uid = rb_uid_value_max;
        rb_uid_value_max++;
      }
    }
  }

  /* Process chain list and splits - "Divide Chains".
   * For every event on this Y axis, divide the surrounding chains. */
  if (!chain_list.is_empty()) {
    /* Process ALL events at this Y level, looking for surrounding chains. */
    int event_index_iter = event_index_beg;
    while (event_index_iter < event_index) {
      Event &e = events[event_index_iter];
      int event_index_next = event_index_iter + 1;
      const Scalar co_x = e.co(verts_table.coords)[0];

      bool has_event_enter_prev_aligned = false;

      /* Get surrounding Y-axis aligned events (if they exist). */
      Event *e_prev = nullptr;
      Event *e_next = nullptr;
      if (e.ty == EventType::Enter) {
        if (event_index_iter > event_enter_beg && event_index_iter < event_enter_end) {
          e_prev = &events[event_index_iter - 1];
          assert(e_prev->ty == EventType::Enter);
        }
        if (event_index_iter + 1 < event_enter_end && event_index_iter >= event_enter_beg) {
          e_next = &events[event_index_iter + 1];
          assert(e_next->ty == EventType::Enter);
        }
      }

      /* Search chain_list for the pair surrounding this event. */
      VertChain *chain_pair_l = nullptr;
      VertChain *chain_pair_r = nullptr;

      VertChain *chain_l = chain_list.head;
      while (chain_l && chain_l->chain_next) {
        VertChain *chain_r = chain_l->chain_next;

        const Scalar co_x_l = chain_l->calc_x_at_y(co_y, verts_table, params.degenerate);
        const Scalar co_x_r = chain_r->calc_x_at_y(co_y, verts_table, params.degenerate);

        if (co_x_l < co_x && co_x < co_x_r) {
          if (chain_l->is_enter) {
            /* Handle axis-aligned events. */
            if (e_prev != nullptr && co_x_l < e_prev->co(verts_table.coords)[0]) {
              assert(!e_prev->chains_beg.empty());
              chain_l = e_prev->chains_beg.back().second;
              has_event_enter_prev_aligned = true;
            }
            if (e_next != nullptr && co_x_r > e_next->co(verts_table.coords)[0]) {
              assert(!e_next->chains_beg.empty());
              chain_r = e_next->chains_beg[0].first;
            }
            chain_pair_l = chain_l;
            chain_pair_r = chain_r;
          }
          break;
        }

        assert(params.degenerate || (chain_l->is_enter != chain_r->is_enter));
        chain_l = chain_r;
      }

      if (chain_pair_l != nullptr && chain_pair_r != nullptr) {
        /* There is a pair of chains surrounding `co_x`, add spans. */
        const int v_l = chain_pair_l->split_at_y(co_y, verts_table);
        const int v_r = chain_pair_r->split_at_y(co_y, verts_table);

        const Scalar co_x_l = verts_table.co_at_index(v_l)[0];
        const Scalar co_x_r = verts_table.co_at_index(v_r)[0];

        if (e.ty == EventType::Exit) {
          /* Step past Y aligned exit events which will be included in the fan. */
          for (int event_index_search = event_index_iter + 1; event_index_search < event_index;
               event_index_search++)
          {
            if (events[event_index_search].ty == EventType::Exit) {
              if (events[event_index_search].co(verts_table.coords)[0] >= co_x_r) {
                break;
              }
              event_index_next = event_index_search + 1;
            }
          }
        }

        if (e.ty == EventType::Enter) {
          assert(!e.chains_beg.empty());
          /* Create span from outer left chain to inner left chain. */
          if (!has_event_enter_prev_aligned) {
            VertChain *chain_other = e.chains_beg[0].first;
            chain_other->split_at_y(co_y, verts_table);
            assert(!chain_pair_l->splits.empty());
            assert(!chain_other->splits.empty());
            span_list.push_back({{chain_pair_l, int(chain_pair_l->splits.size()) - 1},
                                 {chain_other, int(chain_other->splits.size()) - 1}});
          }

          /* Create span from inner right chain to outer right chain. */
          VertChain *chain_other = e.chains_beg.back().second;
          chain_other->split_at_y(co_y, verts_table);
          assert(!chain_other->splits.empty());
          assert(!chain_pair_r->splits.empty());
          span_list.push_back({{chain_other, int(chain_other->splits.size()) - 1},
                               {chain_pair_r, int(chain_pair_r->splits.size()) - 1}});
        }
        else if (e.ty == EventType::Exit) {
          /* Check if there's an ENTER event in the x-range. */
          if (!has_event_in_x_range(event_enter_beg, event_enter_end, co_x_l, co_x_r)) {
            assert(!chain_pair_l->splits.empty());
            assert(!chain_pair_r->splits.empty());
            span_list.push_back({{chain_pair_l, int(chain_pair_l->splits.size()) - 1},
                                 {chain_pair_r, int(chain_pair_r->splits.size()) - 1}});
          }
        }
      }

      event_index_iter = event_index_next;
    }
  }

  /* Add new chains to list. */
  auto value_from_chain_fn = [co_y, &verts_table = verts_table, degenerate = params.degenerate](
                                 const VertChain *chain) -> Scalar {
    return chain->calc_x_at_y(co_y, verts_table, degenerate);
  };

  for (int index = event_enter_beg; index < event_enter_end; index++) {
    Event &e = events[index];
    const Scalar co_x = e.co(verts_table.coords)[0];
    assert(!e.chains_beg.empty());

    if ((chain_list.tail == nullptr) ||
        (co_x > chain_list.tail->calc_x_at_y(co_y, verts_table, params.degenerate)))
    {
      for (auto &[chain_l, chain_r] : e.chains_beg) {
        chain_list.add_tail(chain_l, value_from_chain_fn);
        chain_list.add_tail(chain_r, value_from_chain_fn);
        chain_l->is_enter = true;
        chain_r->is_enter = false;
      }
    }
    else if ((chain_list.head == nullptr) ||
             (co_x < chain_list.head->calc_x_at_y(co_y, verts_table, params.degenerate)))
    {
      for (auto &[chain_l, chain_r] : e.chains_beg) {
        chain_list.add_head(chain_r, value_from_chain_fn);
        chain_list.add_head(chain_l, value_from_chain_fn);
        chain_l->is_enter = true;
        chain_r->is_enter = false;
      }
    }
    else {
      assert(chain_list.tail != nullptr);
      VertChain *chain_other = chain_list.lookup_by_value_and_scan_for_min(co_x,
                                                                           value_from_chain_fn);
      if (!chain_other) {
        chain_other = chain_list.tail;
      }

      assert(chain_other != nullptr);
      const std::pair<Scalar, std::pair<const Vert *, const Vert *>> co_other_result =
          chain_other->calc_x_at_y_with_coords(co_y, verts_table, params.degenerate);
      Scalar co_other_x = co_other_result.first;
      const std::pair<const Vert *, const Vert *> &co_other_coords = co_other_result.second;

      if (co_x == co_other_x) {
        /* The following paragraph explains how an event is handled
         * when it starts overlapping an existing chain.
         *
         * First of all, chains that all start from a single event
         * have special ordering logic. See `region_order_swap`.
         * An event that begins *exactly* on a chain doesn't have special ordering logic.
         *
         * This is a special case, but the handling is fairly simple, that is:
         * Overlapping events are always assumed to be *after* an existing chain.
         *
         * - An "event" may begin *exactly* on the location of an existing chain.
         * - In this case always extend the point after the existing chain.
         * - This works as long as the polygons have been properly pre-processed,
         *   That is, edges do not describe polygons that go *through* other edges.
         *
         * Since these are aligned, check if the first chain is before/after the existing chain. */
        const Vert &chain_first_v0 = verts_table.co_at_index(e.chains_beg[0].first->indices[0]);
        const Vert &chain_first_v1 = verts_table.co_at_index(e.chains_beg[0].first->indices[1]);
        const bool co_other_is_after = segment_v2_v2_y_gradient(*co_other_coords.first,
                                                                *co_other_coords.second) >
                                       segment_v2_v2_y_gradient(chain_first_v0, chain_first_v1);

        if (co_other_is_after) {
          /* Extend `chains_beg` before `chain_other`. */
          for (auto &[chain_l, chain_r] : e.chains_beg) {
            chain_list.add_before(chain_l, chain_other, value_from_chain_fn);
            chain_list.add_before(chain_r, chain_other, value_from_chain_fn);
            chain_l->is_enter = chain_other->is_enter;
            chain_r->is_enter = !chain_other->is_enter;
          }
        }
        else {
          /* Extend `chains_beg` after `chain_other`. */
          VertChain *chain_last = chain_other;
          for (auto &[chain_l, chain_r] : e.chains_beg) {
            chain_list.add_after(chain_l, chain_last, value_from_chain_fn);
            chain_last = chain_l;
            chain_list.add_after(chain_r, chain_last, value_from_chain_fn);
            chain_last = chain_r;
            chain_l->is_enter = !chain_other->is_enter;
            chain_r->is_enter = chain_other->is_enter;
          }
        }
      }
      else {
        /* Extend `chains_beg` before `chain_other`. */
        assert((chain_other->chain_next == nullptr) || (co_x < co_other_x));
        for (auto &[chain_l, chain_r] : e.chains_beg) {
          chain_list.add_before(chain_l, chain_other, value_from_chain_fn);
          chain_list.add_before(chain_r, chain_other, value_from_chain_fn);
          chain_l->is_enter = chain_other->is_enter;
          chain_r->is_enter = !chain_other->is_enter;
        }
      }
    }
  }

  return true;
}

/**
 * Attempt to fill triangles using simple sweep, fall back to ear-clipping if needed.
 *
 * Detects when one edge spans multiple vertices on the opposite chain,
 * which creates a "cave" requiring ear-clipping to fill correctly.
 * Returns updated (c_index_l, c_index_r) after filling, or unchanged if no fill occurred.
 */
std::pair<int, int> SweepInterval::fill_sweep_or_clip(FaceConstructContext &fctx,
                                                      const IndexSubset &indices_l,
                                                      const IndexSubset &indices_r,
                                                      int c_index_l,
                                                      int c_index_r,
                                                      const int c_end_l,
                                                      const int c_end_r)
{
  VertsTable &verts_table_local = fctx.verts_table;

  const int v_curr_l = indices_l.at_index(c_index_l);
  const int v_curr_r = indices_r.at_index(c_index_r);

  const int v_next_l = (c_index_l + 1 <= c_end_l) ? indices_l.at_index(c_index_l + 1) :
                                                    NO_NEXT_VERT;
  const int v_next_r = (c_index_r + 1 <= c_end_r) ? indices_r.at_index(c_index_r + 1) :
                                                    NO_NEXT_VERT;

  const std::pair<int, int> fallback = {c_index_l, c_index_r};

  /* Both must exist. */
  if (v_next_l == NO_NEXT_VERT || v_next_r == NO_NEXT_VERT) {
    return fallback;
  }

  const int v_next_next_l = (c_index_l + 2 <= c_end_l) ? indices_l.at_index(c_index_l + 2) :
                                                         NO_NEXT_VERT;
  const int v_next_next_r = (c_index_r + 2 <= c_end_r) ? indices_r.at_index(c_index_r + 2) :
                                                         NO_NEXT_VERT;

  /* At least one must have a triangle. */
  if (v_next_next_l == NO_NEXT_VERT && v_next_next_r == NO_NEXT_VERT) {
    return fallback;
  }

  /* Cache coordinate lookups. */
  const Vert &co_curr_l = verts_table_local.co_at_index(v_curr_l);
  const Vert &co_curr_r = verts_table_local.co_at_index(v_curr_r);
  const Vert &co_next_l = verts_table_local.co_at_index(v_next_l);
  const Vert &co_next_r = verts_table_local.co_at_index(v_next_r);

  /* Check if the right edge spans many points on the left. */
  if (v_next_next_l != NO_NEXT_VERT) {
    const Vert &co_next_next_l = verts_table_local.co_at_index(v_next_next_l);
    if (co_curr_l[1] > co_curr_r[1] && co_next_next_l[1] < co_next_r[1]) {
      const Scalar edge_co_end_y = co_next_r[1];
      int found_l = c_index_l + 2;
      int test_l = found_l + 1;
      while (test_l <= c_end_l &&
             verts_table_local.co_at_index(indices_l.at_index(test_l))[1] < edge_co_end_y)
      {
        found_l = test_l;
        test_l++;
      }

      const int length_l = found_l + 1;

      if constexpr (DO_FILL_EAR_CLIP) {
        fill_with_ear_clip_strategy(fctx,
                                    indices_l.subspan(c_index_l, length_l),
                                    indices_r.subspan(c_index_r, c_index_r + 2));
        return {length_l - 1, c_index_r + 1};
      }
    }
  }

  /* Check if the left edge spans many points on the right. */
  if (v_next_next_r != NO_NEXT_VERT) {
    const Vert &co_next_next_r = verts_table_local.co_at_index(v_next_next_r);
    if (co_curr_r[1] > co_curr_l[1] && co_next_next_r[1] < co_next_l[1]) {
      const Scalar edge_co_end_y = co_next_l[1];
      int found_r = c_index_r + 2;
      int test_r = found_r + 1;
      while (test_r <= c_end_r &&
             verts_table_local.co_at_index(indices_r.at_index(test_r))[1] < edge_co_end_y)
      {
        found_r = test_r;
        test_r++;
      }

      const int length_r = found_r + 1;

      if constexpr (DO_FILL_EAR_CLIP) {
        fill_with_ear_clip_strategy(fctx,
                                    indices_l.subspan(c_index_l, c_index_l + 2),
                                    indices_r.subspan(c_index_r, length_r));
        return {c_index_l + 1, length_r - 1};
      }
    }
  }

  return fallback;
}

void SweepInterval::fill_region(FaceConstructContext &fctx,
                                const IndexSubset &indices_l,
                                const IndexSubset &indices_r,
                                const std::vector<int> &fan_beg,
                                const std::vector<int> &fan_end,
                                [[maybe_unused]] void *edges_result_for_debug)
{
  VertsTable &verts_table_local = fctx.verts_table;

  int c_index_l = 0;
  int c_end_l = indices_l.length() - 1;
  int c_index_r = 0;
  int c_end_r = indices_r.length() - 1;

  if (indices_l.at_first() == indices_r.at_first()) {
    if (c_end_l == c_index_l) {
      c_index_r++;
      assert(c_end_r >= c_index_r);
    }
    else if (c_end_r == c_index_r) {
      c_index_l++;
    }
    else {
      const Scalar y_next_l = verts_table_local.co_at_index(indices_l.at_index(c_index_l + 1))[1];
      const Scalar y_next_r = verts_table_local.co_at_index(indices_r.at_index(c_index_r + 1))[1];
      if (y_next_l <= y_next_r) {
        c_index_l++;
      }
      else {
        c_index_r++;
      }
    }
  }
  else if (!fan_beg.empty()) {
    /* Fill the first triangle using a fan, then advance the polygon. */
    assert(indices_l.at_first() != indices_r.at_first());
    /* First step. */
    bool do_l = c_end_l - c_index_l > 0;
    bool do_r = c_end_r - c_index_r > 0;
    if (do_l && do_r) {
      /* Find closest to lower vertex. */
      const Scalar y_next_l = verts_table_local.co_at_index(indices_l.at_index(c_index_l + 1))[1];
      const Scalar y_next_r = verts_table_local.co_at_index(indices_r.at_index(c_index_r + 1))[1];
      if (y_next_l > y_next_r) {
        do_l = false;
      }
      else {
        do_r = false;
      }
    }
    else {
      assert(do_l || do_r);
    }

    const int v_pivot = do_l ? indices_l.at_index(c_index_l + 1) :
                               indices_r.at_index(c_index_r + 1);
    int v_index_prev = fan_beg[0];

    /* First triangle: `(v_pivot, indices_l.at_index(c_index_l), fan_beg[0])`. */
    faces_append_with_checks(fctx, v_pivot, indices_l.at_index(c_index_l), v_index_prev);

    /* Fan triangles. */
    for (int i = 1; i < int(fan_beg.size()); i++) {
      const int v_index_curr = fan_beg[i];
      faces_append_with_checks(fctx, v_pivot, v_index_prev, v_index_curr);
      v_index_prev = v_index_curr;
    }

    /* Last triangle: (v_pivot, v_index_prev, indices_r.at_index(c_index_r)). */
    faces_append_with_checks(fctx, v_pivot, v_index_prev, indices_r.at_index(c_index_r));

    if (do_l) {
      c_index_l++;
    }
    else {
      c_index_r++;
    }
  }

  /* Handle matching end vertices. */
  if (indices_l.at_index(c_end_l) == indices_r.at_index(c_end_r)) {
    /* When the vertices match,
     * step forward to define the upper-most "edge".
     * Bias the right side as a tie-breaker (although it doesn't matter currently). */
    if (c_end_l == c_index_l) [[unlikely]] {
      c_end_r--;
      assert(c_end_r >= c_index_r);
    }
    else if (c_end_r == c_index_r) {
      c_end_l--;
      assert(c_end_l >= c_index_l);
    }
    else if (verts_table_local.co_at_index(indices_l.at_index(c_end_l - 1))[1] >
             verts_table_local.co_at_index(indices_r.at_index(c_end_r - 1))[1])
    {
      c_end_l--;
    }
    else {
      c_end_r--;
    }
  }
  else if (!fan_end.empty()) {
    /* Fill the last triangle using a fan, then advance the polygon. */
    assert(indices_l.at_index(c_end_l) != indices_r.at_index(c_end_r));
    /* First step. */
    bool do_l = c_end_l - c_index_l > 0;
    bool do_r = c_end_r - c_index_r > 0;
    if (do_l && do_r) {
      /* Find closest to upper vertex. */
      if (verts_table_local.co_at_index(indices_l.at_index(c_end_l - 1))[1] >
          verts_table_local.co_at_index(indices_r.at_index(c_end_r - 1))[1])
      {
        do_r = false;
      }
      else {
        do_l = false;
      }
    }
    else {
      assert(do_l || do_r);
    }

    const int v_pivot = do_l ? indices_l.at_index(c_end_l - 1) : indices_r.at_index(c_end_r - 1);
    const int v_last_l = indices_l.at_index(c_end_l);
    const int v_last_r = indices_r.at_index(c_end_r);

    faces_append_with_checks(fctx, v_pivot, fan_end[0], v_last_l);
    for (int i = 1; i < int(fan_end.size()); i++) {
      faces_append_with_checks(fctx, v_pivot, fan_end[i], fan_end[i - 1]);
    }
    faces_append_with_checks(fctx, v_pivot, v_last_r, fan_end.back());

    if (do_l) {
      c_end_l--;
    }
    else {
      c_end_r--;
    }
  }

  /* Early exit checks. */
  if (c_index_l > c_end_l) {
    return;
  }
  if (c_index_r > c_end_r) {
    return;
  }

  int fail_iter_max = (c_end_l - c_index_l) + (c_end_r - c_index_r) + 2;

  while ((c_index_l < c_end_l) || (c_index_r < c_end_r)) {
    fail_iter_max--;
    if (fail_iter_max <= 0) [[unlikely]] {
      break;
    }

    const int v_curr_l = indices_l.at_index(c_index_l);
    const int v_curr_r = indices_r.at_index(c_index_r);

    const int v_next_l = (c_index_l < c_end_l) ? indices_l.at_index(c_index_l + 1) : NO_NEXT_VERT;
    const int v_next_r = (c_index_r < c_end_r) ? indices_r.at_index(c_index_r + 1) : NO_NEXT_VERT;

    /* Exit early if both chains are exhausted. */
    if (v_next_l == NO_NEXT_VERT && v_next_r == NO_NEXT_VERT) {
      break;
    }

    /* Triangle selection - all cases set `tri_v0/1/2` and fall through to common handling. */
    int tri_v0, tri_v1, tri_v2;

    /* Check if either edge ends (single-chain cases). */
    if (v_next_l == NO_NEXT_VERT) {
      /* Single right chain. */
      const Vert &co_curr_l = verts_table_local.co_at_index(v_curr_l);
      const Vert &co_curr_r = verts_table_local.co_at_index(v_curr_r);
      const Vert &co_next_r = verts_table_local.co_at_index(v_next_r);

      /* Check for concave triangle - return if so. */
      if (tri_v2_sign(co_curr_l, co_curr_r, co_next_r) == SignType::Concave) {
        return;
      }
      tri_v0 = v_curr_l;
      tri_v1 = v_curr_r;
      tri_v2 = v_next_r;
      c_index_r++;
    }
    else if (v_next_r == NO_NEXT_VERT) {
      /* Single left chain. */
      const Vert &co_next_l = verts_table_local.co_at_index(v_next_l);
      const Vert &co_curr_l = verts_table_local.co_at_index(v_curr_l);
      const Vert &co_curr_r = verts_table_local.co_at_index(v_curr_r);

      /* Check for concave triangle - return if so. */
      if (tri_v2_sign(co_next_l, co_curr_l, co_curr_r) == SignType::Concave) {
        return;
      }
      tri_v0 = v_next_l;
      tri_v1 = v_curr_l;
      tri_v2 = v_curr_r;
      c_index_l++;
    }
    else {
      /* Both have next vertices - choose based on Y coordinate. */
      const Vert &co_next_l = verts_table_local.co_at_index(v_next_l);
      const Vert &co_next_r = verts_table_local.co_at_index(v_next_r);

      if constexpr (DO_FILL_EAR_CLIP) {
        std::pair<int, int> new_c_indices = fill_sweep_or_clip(
            fctx, indices_l, indices_r, c_index_l, c_index_r, c_end_l, c_end_r);
        int new_c_index_l = new_c_indices.first;
        int new_c_index_r = new_c_indices.second;
        if (new_c_index_l != c_index_l || new_c_index_r != c_index_r) {
          c_index_l = new_c_index_l;
          c_index_r = new_c_index_r;
          continue;
        }
      }

      /* Axis aligned handling - just skip degenerate edges with identical coordinates. */
      const Vert &co_curr_l = verts_table_local.co_at_index(v_curr_l);
      const Vert &co_curr_r = verts_table_local.co_at_index(v_curr_r);

      /* Skip degenerate edges where consecutive vertices have identical coordinates. */
      if constexpr (DO_FANFILL_AXIS_ALIGNED_AVOID_ZERO_AREA) {
        if (co_next_l[1] == co_curr_l[1] && co_next_l[0] == co_curr_l[0]) {
          c_index_l++;
          continue;
        }
        if (co_next_r[1] == co_curr_r[1] && co_next_r[0] == co_curr_r[0]) {
          c_index_r++;
          continue;
        }
      }

      /* Both have next vertices - use smart sweep that checks for concave triangles.
       * Define the two possible triangles.
       * - `tri_l`: advance left side - vertices (v_next_l, v_curr_l, v_curr_r).
       * - `tri_r`: advance right side - vertices (v_curr_l, v_curr_r, v_next_r).
       */
      const SignType sign_l = tri_v2_sign(co_next_l, co_curr_l, co_curr_r);
      const SignType sign_r = tri_v2_sign(co_curr_l, co_curr_r, co_next_r);

      if (co_next_l[1] < co_next_r[1]) {
        /* Try left first, then right. */
        if (sign_l != SignType::Concave) {
          tri_v0 = v_next_l;
          tri_v1 = v_curr_l;
          tri_v2 = v_curr_r;
          c_index_l++;
        }
        else if (sign_r != SignType::Concave) {
          tri_v0 = v_curr_l;
          tri_v1 = v_curr_r;
          tri_v2 = v_next_r;
          c_index_r++;
        }
        else {
          break;
        }
      }
      else {
        /* Try right first, then left. */
        if (sign_r != SignType::Concave) {
          tri_v0 = v_curr_l;
          tri_v1 = v_curr_r;
          tri_v2 = v_next_r;
          c_index_r++;
        }
        else if (sign_l != SignType::Concave) {
          tri_v0 = v_next_l;
          tri_v1 = v_curr_l;
          tri_v2 = v_curr_r;
          c_index_l++;
        }
        else {
          break;
        }
      }
    }

    /* Get coordinates for the selected triangle. */
    const Vert &tri_co0 = verts_table_local.co_at_index(tri_v0);
    const Vert &tri_co1 = verts_table_local.co_at_index(tri_v1);
    const Vert &tri_co2 = verts_table_local.co_at_index(tri_v2);

    /* `DO_FANFILL_AXIS_ALIGNED_SKIP`: skip if all 3 Y coords are equal (before `AVOID_ZERO_AREA`).
     */
    if constexpr (DO_FANFILL_AXIS_ALIGNED && DO_FANFILL_AXIS_ALIGNED_SKIP) {
      if (tri_co0[1] == tri_co1[1] && tri_co1[1] == tri_co2[1]) {
        continue;
      }
    }

    if constexpr (DO_FANFILL_AXIS_ALIGNED_AVOID_ZERO_AREA) {
      /* Y-Aligned Edge Fan-Fill Logic:
       *
       * When a triangle has one edge that is perfectly horizontal (Y-aligned),
       * additional vertices on that same Y level may need to be incorporated
       * via fan-filling to avoid creating degenerate or overlapping triangles.
       *
       * The algorithm identifies:
       *   - The two corners forming the Y-aligned (horizontal) edge.
       *   - The third corner (the "tip" of the triangle).
       *
       * We then scan for additional vertices along both the left and right
       * chains that fall on the same Y coordinate as the horizontal edge and
       * lie within the X bounds of that edge. For each such vertex found,
       * a fan triangle is created connecting it to the tip vertex
       * and its neighbors along the horizontal edge.
       *
       * The direction of iteration (forward or reversed) depends on whether
       * the horizontal edge is at the maximum Y (top) or minimum Y (bottom)
       * of the triangle, as this affects which way we traverse the vertex chains.
       *
       * Note that strict ordering ensures:
       * - The 0th triangle-corner is always on the left.
       * - The 1st triangle-corner is always on the right.
       * The middle (non-aligned) may be from either side.
       * These rules simplify the following checks somewhat,
       * in that we only have to check 0-1 or 1-2 (0-2 has been handled already).
       * - Left side is the Y aligned edge (0-1).
       * - Right side is the Y aligned edge (1-2).
       */

      /* Check for horizontal edge fan-fill. */
      /* First check if top edge (0-2) is horizontal - this is handled differently. */
      if (tri_co0[1] == tri_co2[1]) {
        /* Top edge is horizontal. */
        const Scalar tri_co_upper_y = tri_co0[1];
        const Scalar tri_co_min_x = tri_co0[0];
        const Scalar tri_co_max_x = tri_co2[0];

        /* Handle left chain - scan forward for vertices at the same Y. */
        int v_test_l_prev = tri_v0;
        for (int c_index_test = c_index_l; c_index_test <= c_end_l; c_index_test++) {
          const int v_test_l = indices_l.at_index(c_index_test);
          const Vert &co_test = verts_table_local.co_at_index(v_test_l);
          if (co_test[1] != tri_co_upper_y)
            break;
          if (co_test[0] < tri_co_min_x)
            break;
          if (v_test_l != v_test_l_prev) {
            faces_append_with_checks(fctx, v_test_l_prev, tri_v1, v_test_l);
            v_test_l_prev = v_test_l;
            c_index_l++;
          }
        }
        if (v_test_l_prev != tri_v0) {
          tri_v0 = v_test_l_prev;
        }

        /* Handle right chain - scan forward for vertices at the same Y. */
        int v_test_r_prev = tri_v2;
        for (int c_index_test = c_index_r; c_index_test <= c_end_r; c_index_test++) {
          const int v_test_r = indices_r.at_index(c_index_test);
          const Vert &co_test = verts_table_local.co_at_index(v_test_r);
          if (co_test[1] != tri_co_upper_y)
            break;
          if (co_test[0] > tri_co_max_x)
            break;
          if (v_test_r != v_test_r_prev) {
            faces_append_with_checks(fctx, v_test_r, tri_v1, v_test_r_prev);
            v_test_r_prev = v_test_r;
            c_index_r++;
          }
        }
        if (v_test_r_prev != tri_v2) {
          tri_v2 = v_test_r_prev;
        }
      }
      else {
        /* Check if edge 0-1 or 1-2 is horizontal. */
        /* order: +1 if edge 0-1 is horizontal, -1 if edge 1-2 is horizontal, 0 if neither. */
        int order = 0;
        if (tri_co0[1] == tri_co1[1]) {
          order = 1;
        }
        else if (tri_co1[1] == tri_co2[1]) {
          order = -1;
        }

        if (order != 0) {
          /* One edge of the triangle is horizontal. Create fan triangles. */
          const Scalar tri_co_edge_y = (order == 1) ? tri_co0[1] : tri_co1[1];
          const Scalar tri_co_other_y = (order == 1) ? tri_co2[1] : tri_co0[1];
          Scalar tri_co_min_x = (order == 1) ? std::min(tri_co0[0], tri_co1[0]) :
                                               std::min(tri_co1[0], tri_co2[0]);
          Scalar tri_co_max_x = (order == 1) ? std::max(tri_co0[0], tri_co1[0]) :
                                               std::max(tri_co1[0], tri_co2[0]);
          const int v_pivot = (order == 1) ? tri_v2 : tri_v0;
          const bool is_edge_max = tri_co_edge_y > tri_co_other_y;

          /* Handle left chain - scan for vertices on the horizontal edge. */
          const int v_test_l_beg = (order == 1) ? tri_v0 : tri_v1;
          const int v_test_l_end = (order == 1) ? tri_v1 : tri_v2;
          int v_test_l_prev = v_test_l_beg;
          bool is_first_l = true;

          const int scan_l_beg = is_edge_max ? c_index_l : (c_index_l - 1);
          const int scan_l_end = is_edge_max ? (c_end_l + 1) : -1;
          const int scan_l_step = is_edge_max ? 1 : -1;

          for (int c_index_test = scan_l_beg; c_index_test != scan_l_end;
               c_index_test += scan_l_step)
          {
            const int v_test_l = indices_l.at_index(c_index_test);
            if (is_first_l) {
              is_first_l = false;
              if (v_test_l == v_test_l_prev)
                continue;
            }
            if (v_test_l == v_test_l_end)
              break;
            const Vert &co_test = verts_table_local.co_at_index(v_test_l);
            if (co_test[1] != tri_co_edge_y)
              break;
            if (co_test[0] < tri_co_min_x || co_test[0] > tri_co_max_x)
              break;

            faces_append_with_checks(fctx, v_pivot, v_test_l_prev, v_test_l);
            v_test_l_prev = v_test_l;
            if (is_edge_max) {
              c_index_l++;
            }
          }

          /* Update triangle if we found additional vertices on left. */
          if (v_test_l_prev != v_test_l_beg) {
            if (order == 1) {
              tri_v0 = v_test_l_prev;
            }
            else {
              tri_v1 = v_test_l_prev;
            }
          }

          /* Handle right chain - scan for vertices on the horizontal edge. */
          const int v_test_r_beg = (order == 1) ? tri_v1 : tri_v2;
          const int v_test_r_end = (order == 1) ? tri_v0 : tri_v1;
          int v_test_r_prev = v_test_r_beg;
          bool is_first_r = true;

          const int scan_r_beg = is_edge_max ? c_index_r : (c_index_r - 1);
          const int scan_r_end = is_edge_max ? (c_end_r + 1) : -1;
          const int scan_r_step = is_edge_max ? 1 : -1;

          for (int c_index_test = scan_r_beg; c_index_test != scan_r_end;
               c_index_test += scan_r_step)
          {
            const int v_test_r = indices_r.at_index(c_index_test);
            if (is_first_r) {
              is_first_r = false;
              if (v_test_r == v_test_r_prev)
                continue;
            }
            if (v_test_r == v_test_r_end)
              break;
            const Vert &co_test = verts_table_local.co_at_index(v_test_r);
            if (co_test[1] != tri_co_edge_y)
              break;
            if (co_test[0] < tri_co_min_x || co_test[0] > tri_co_max_x)
              break;

            faces_append_with_checks(fctx, v_pivot, v_test_r, v_test_r_prev);
            v_test_r_prev = v_test_r;
            if (is_edge_max) {
              c_index_r++;
            }
          }

          /* Update triangle if we found additional vertices on right. */
          if (v_test_r_prev != v_test_r_beg) {
            if (order == 1) {
              tri_v1 = v_test_r_prev;
            }
            else {
              tri_v2 = v_test_r_prev;
            }
          }
        }
      }
    }

    faces_append_with_checks(fctx, tri_v0, tri_v1, tri_v2);
  }
}

void SweepInterval::fill_with_ear_clip_strategy(FaceConstructContext &fctx,
                                                const IndexSubset &indices_l,
                                                const IndexSubset &indices_r)
{
  [[maybe_unused]] const PolyFillParams &params_local = fctx.params;
  VertsTable &verts_table_local = fctx.verts_table;

  const int len_l = indices_l.length();
  const int len_r = indices_r.length();

  /* Determine which side is the edge (length 2) and which is the main indices. */
  int side;
  if (len_r == 2) {
    assert(len_l > 2);
    side = 0;
  }
  else if (len_l == 2) {
    assert(len_r > 2);
    side = 1;
  }
  else {
    assert(false); /* One side must have length 2. */
    return;
  }

  /* Use pointer to avoid copying; store subspan results when needed. */
  const IndexSubset *indices = (side == 0) ? &indices_l : &indices_r;
  std::optional<IndexSubset> indices_storage;
  const IndexSubset &indices_other = (side == 0) ? indices_r : indices_l;

  /* Perform a simple sweep for lower, then upper indices.
   * In the best case, this is all that is needed,
   * no clipping is performed and the function returns. */

  const int v_edge_0 = indices_other.at_index(0);
  const int v_edge_1 = indices_other.at_index(1);

  const Vert &co_edge_0 = verts_table_local.co_at_index(v_edge_0);
  const Vert &co_edge_1 = verts_table_local.co_at_index(v_edge_1);

  /* Pre-compute side-dependent values to avoid repeated branches. */
  const int v_edge_first = (side == 0) ? v_edge_0 : v_edge_1;
  const int v_edge_second = (side == 0) ? v_edge_1 : v_edge_0;

  /* Simple scan-fill in the remainder (at the beginning).
   * In many cases this is all that is needed - fast & simple. */
  int i = 1;
  while (i < indices->length()) {
    const int v0 = indices->at_index((side == 0) ? i : i - 1);
    const int v1 = indices->at_index((side == 0) ? i - 1 : i);

    const Vert &co0 = verts_table_local.co_at_index(v0);
    const Vert &co1 = verts_table_local.co_at_index(v1);

    if (tri_v2_sign(co0, co1, co_edge_0) == SignType::Convex) {
      faces_append_with_checks(fctx, v0, v1, v_edge_0);
    }
    else {
      break;
    }
    i++;
  }

  if (i == indices->length()) {
    faces_append_with_checks(fctx, indices->at_last(), v_edge_first, v_edge_second);
    /* Done. */
    return;
  }

  indices_storage = indices->subspan(i - 1, indices->length());
  indices = &indices_storage.value();

  /* Fill in the end (it may be able to avoid clipping).
   * In many cases this is all that is needed - fast & simple. */
  i = indices->length() - 1;
  while (i > 0) {
    const int v0 = indices->at_index((side == 0) ? i : i - 1);
    const int v1 = indices->at_index((side == 0) ? i - 1 : i);

    const Vert &co0 = verts_table_local.co_at_index(v0);
    const Vert &co1 = verts_table_local.co_at_index(v1);

    if (tri_v2_sign(co0, co1, co_edge_1) == SignType::Convex) {
      faces_append_with_checks(fctx, v0, v1, v_edge_1);
    }
    else {
      break;
    }
    i--;
  }

  if (i == 0) {
    faces_append_with_checks(fctx, indices->at_first(), v_edge_first, v_edge_second);
    /* Done. */
    return;
  }

  indices_storage = indices->subspan(0, i + 1);
  indices = &indices_storage.value();

  /* At this point "simple" cases have been handled,
   * where it's possible to fill in the beginning and end of the bounds.
   * Now we move to the full "ear clipping" method.
   *
   * Since this involves removing indices, use a simple linked data structure,
   * built to avoid a lot of inefficient memory-moving.
   * Note that for the purpose of the ear-clipping logic it's important not to clip ears
   * defined by the two vertices that make up the edge (`indices_other`)
   * as they may contain vertices.
   *
   * The exact strategy for which ear to clip first impacts performance somewhat,
   * as it's inefficient to re-check ears for clipping that cannot be clipped.
   * The topology of the resulting faces is also impacted by the order of clipping.
   *
   * This method aims to produce good results, while avoiding expensive re-checking.
   *
   * Details:
   *
   * - Prioritize ears where the X-tip is either on the left or right of the two other vertices.
   *   This results in a vertical zigzag pattern, similar to the horizontal zigzag pattern
   *   used by scanning up Y sorted edges.
   * - Scan in both directions, for predictable performance,
   *   also to avoid arriving at an ear the "long" way around.
   * - Searching for the best `CONVEX_IS_TIP_X_EXTREME` *does* add some overhead,
   *   after all any `CONVEX` ear can be clipped.
   *   The overall result however is that ear "caves" are filled from one side to another
   *   with less searching once the end of the cave has been found.
   *
   * Build the ear data with the correct winding so `side` can be ignored. */

  const int indices_len = indices->length();
  std::vector<VertEar> ears;
  ears.reserve(indices_len + 2); /* +2 for edge vertices. */

  /* Build linked list from indices. */
  for (int j = 0; j < indices_len; j++) {
    ears.emplace_back(indices->at_index(j));
  }

  /* Ears from the `indices_other`. */
  VertEar ear_v0_storage(v_edge_0);
  VertEar ear_v1_storage(v_edge_1);
  ears.push_back(ear_v0_storage);
  ears.push_back(ear_v1_storage);

  const int n = ears.size();
  VertEar *ear_v0 = &ears[n - 2];
  VertEar *ear_v1 = &ears[n - 1];

  VertEarClipCount counts;

  /* Close the loop. */
  if (side == 1) {
    ear_v0->ear_next = &ears[0];
    ear_v0->ear_prev = ear_v1;

    ear_v1->ear_next = ear_v0;
    ear_v1->ear_prev = &ears[indices_len - 1];

    /* Weld up with `indices`. */
    ears[0].ear_prev = ear_v0;
    ears[indices_len - 1].ear_next = ear_v1;

    /* Link the middle elements. */
    for (int j = 0; j < indices_len; j++) {
      if (j > 0) {
        ears[j].ear_prev = &ears[j - 1];
      }
      if (j < indices_len - 1) {
        ears[j].ear_next = &ears[j + 1];
      }
    }
  }
  else {
    ear_v0->ear_prev = &ears[0];
    ear_v0->ear_next = ear_v1;

    ear_v1->ear_prev = ear_v0;
    ear_v1->ear_next = &ears[indices_len - 1];

    /* Weld up with `indices`. */
    ears[0].ear_next = ear_v0;
    ears[indices_len - 1].ear_prev = ear_v1;

    /* Link the middle elements (reversed). */
    for (int j = 0; j < indices_len; j++) {
      if (j > 0) {
        ears[j].ear_next = &ears[j - 1];
      }
      if (j < indices_len - 1) {
        ears[j].ear_prev = &ears[j + 1];
      }
    }
  }

  int ear_count = indices_len + 2;

  /* The `side` has now been handled, the winding is now correct. */

  /* Calculate sign. */
  VertEar *ear_init = &ears[0];
  Scalar ear_init_x = verts_table_local.co_at_index(ear_init->vert_index)[0];

  VertEar *ear = &ears[0];
  if (side == 1) {
    for (int j = 0; j < indices_len; j++) {
      assert(ear == ear->ear_next->ear_prev);
      assert(ear == ear->ear_prev->ear_next);
      ear->sign_calc_init(counts, verts_table_local);
      /* Track the best ear to start with (skip the initial search). */
      if (ear->ty == SignType::ConvexIsTipXExtreme) {
        const Scalar ear_test_x = verts_table_local.co_at_index(ear->vert_index)[0];
        if (ear_test_x > ear_init_x) {
          ear_init_x = ear_test_x;
          ear_init = ear;
        }
      }
      ear = ear->ear_next;
    }
  }
  else {
    for (int j = 0; j < indices_len; j++) {
      assert(ear == ear->ear_next->ear_prev);
      assert(ear == ear->ear_prev->ear_next);
      /* Track the best ear to start with (skip the initial search). */
      ear->sign_calc_init(counts, verts_table_local);
      if (ear->ty == SignType::ConvexIsTipXExtreme) {
        const Scalar ear_test_x = verts_table_local.co_at_index(ear->vert_index)[0];
        if (ear_test_x < ear_init_x) {
          ear_init_x = ear_test_x;
          ear_init = ear;
        }
      }
      ear = ear->ear_prev;
    }
  }

  /* This is a signal not to clip these ears and not to recalculate the ears
   * since it's important never to clip ears from the 2x edge vertices
   * since the triangles they form may overlap vertices on the opposite side.
   * For this reason, they are locked.
   * Note that we could also define a non-circular structure, in practice
   * this adds more overhead - having to account for the none next/previous. */
  ear_v0->ty = SignType::Nop;
  ear_v1->ty = SignType::Nop;

  /* Tie break on the right-most for side == 1, otherwise the left-most. */
  const Scalar prev_x = verts_table_local.co_at_index(ear_init->ear_prev->vert_index)[0];
  const Scalar next_x = verts_table_local.co_at_index(ear_init->ear_next->vert_index)[0];

  VertEar *ear_prev;
  VertEar *ear_next;
  if ((prev_x >= next_x) == (side == 1)) {
    ear_prev = ear_init->ear_prev;
    ear_next = ear_init;
  }
  else {
    ear_prev = ear_init;
    ear_next = ear_init->ear_next;
  }

  /* Sweep forward & backward, switching when non-convex vertices are met.
   * Taking care never to get into an eternal loop by only switching
   * when the opposite direction contains a "convex" ear to step onto.
   *
   * Use forward based on side for matching stepping based on the winding. */
  while (ear_count > 2) {
    SignType clip_type;
    if (counts.values[int(SignType::ConvexIsTipXExtreme)] > 0) {
      clip_type = SignType::ConvexIsTipXExtreme;
    }
    else if (counts.values[int(SignType::Convex)] > 0) {
      assert(ear_prev->ty != SignType::ConvexIsTipXExtreme); /* Unreachable, invalid counts. */
      assert(ear_next->ty != SignType::ConvexIsTipXExtreme); /* Unreachable, invalid counts. */
      clip_type = SignType::Convex;
    }
    else if (counts.values[int(SignType::Tangential)] > 0) {
      clip_type = SignType::Tangential;
      assert(ear_prev->ty != SignType::Convex); /* Unreachable, invalid counts. */
      assert(ear_next->ty != SignType::Convex); /* Unreachable, invalid counts. */
    }
    else {
      break;
    }

    /* Scan until a usable ear is found. */
    while (ear_prev->ty != clip_type && ear_next->ty != clip_type) {
      ear_prev = ear_prev->ear_prev;
      ear_next = ear_next->ear_next;
    }

    VertEar *ear_clip;
    if (ear_prev->ty == clip_type && ear_next->ty == clip_type) {
      /* Tie break on the outer most ear. */
      const Scalar x_p = verts_table_local.co_at_index(ear_prev->vert_index)[0];
      const Scalar x_n = verts_table_local.co_at_index(ear_next->vert_index)[0];
      if ((x_p > x_n) == (side == 1)) {
        ear_clip = ear_prev;
      }
      else {
        ear_clip = ear_next;
      }
    }
    else if (ear_prev->ty == clip_type) {
      ear_clip = ear_prev;
    }
    else if (ear_next->ty == clip_type) {
      ear_clip = ear_next;
    }
    else {
      assert(false); /* Unreachable. */
      break;
    }

    /* The ears to continue searching with. */
    ear_prev = ear_clip->ear_prev;
    ear_next = ear_clip->ear_next;

    assert(ear_clip->ty == clip_type);
    assert(params_local.degenerate ||
           (ear_clip->vert_index != v_edge_0 && ear_clip->vert_index != v_edge_1));

    faces_append_with_checks(
        fctx, ear_prev->vert_index, ear_clip->vert_index, ear_next->vert_index);

    ear_clip->unlink_and_update(counts, verts_table_local);
    ear_count--;

    /* Continue clipping `ear_prev` / `ear_next`. */
  }

  /*
   * Desperate mode (degenerate shape), clip all remaining.
   * Generally the following code should not run unless the shape is degenerate or self
   * intersecting.
   */
  while (ear_count > 2) {
    VertEar *ear_clip = ear_next;
    ear_next = ear_next->ear_next;
    /* Although they could be removed,
     * it's simplest to always remove other ears, by convention and to avoid
     * bad geometry *even* in the case of degenerate ears, clipping them is likely to be worse. */
    if (ear_clip->ty == SignType::Nop) {
      /* Skip. */
    }
    else {
      if (ear_clip->ty == SignType::Concave) {
        faces_append_with_checks(fctx,
                                 ear_clip->ear_next->vert_index,
                                 ear_clip->vert_index,
                                 ear_clip->ear_prev->vert_index);
      }
      else {
        faces_append_with_checks(fctx,
                                 ear_clip->ear_prev->vert_index,
                                 ear_clip->vert_index,
                                 ear_clip->ear_next->vert_index);
      }

      ear_clip->unlink_and_update(counts, verts_table_local);
      ear_count--;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge rotation to remove tessellated vertices
 * \{ */

/**
 * Remove tessellated vertices by rotating edges away from them.
 *
 * For each tessellated vertex (pivot), find all adjacent face pairs sharing
 * edges at the pivot. Collapse edges by rotating: the two faces become one,
 * eliminating the pivot vertex from those faces.
 *
 * Edges are processed shortest-first to minimize distortion.
 * Returns vertices that couldn't be collapsed (for retry with force_collapse).
 */
std::vector<int> rotate_edges_away_pass(VertsTable &verts_table,
                                        [[maybe_unused]] std::vector<FaceResult> &faces_result,
                                        std::vector<std::vector<FaceResult *>> &coords_face_vec,
                                        std::span<const int> verts_tess_to_handle,
                                        const bool force_collapse_degenerate)
{
  std::vector<int> verts_tess_to_handle_next;

  for (const int v_pivot_base : verts_tess_to_handle) {
    const int v_pivot = v_pivot_base ^ -1;

    /* Skip vertices not used in any faces. */
    std::vector<FaceResult *> &faces_of_pivot = coords_face_vec[v_pivot_base];
    if (faces_of_pivot.empty()) {
      continue;
    }

    std::unordered_map<int, FacePair> edge_map;

    for (FaceResult *f : faces_of_pivot) {
      /* Find pivot position directly (only 3 elements). */
      int v_in_face = -1;
      if (f->tri[0] == v_pivot) {
        v_in_face = 0;
      }
      else if (f->tri[1] == v_pivot) {
        v_in_face = 1;
      }
      else if (f->tri[2] == v_pivot) {
        v_in_face = 2;
      }

      /* Skip faces that no longer contain the pivot (can happen after welding). */
      if (v_in_face == -1) {
        continue;
      }

      /* Rotate face so pivot is at index 0. */
      if (v_in_face == 1) {
        f->tri = {f->tri[1], f->tri[2], f->tri[0]};
      }
      else if (v_in_face == 2) {
        f->tri = {f->tri[2], f->tri[0], f->tri[1]};
      }
      assert(f->tri[0] == v_pivot);

      const int v_other_a = f->tri[1];
      const int v_other_b = f->tri[2];

      auto it_a = edge_map.find(v_other_a);
      if (it_a != edge_map.end()) {
        it_a->second.f_b = f;
      }
      else {
        edge_map.emplace(v_other_a, FacePair(f, f));
      }

      auto it_b = edge_map.find(v_other_b);
      if (it_b != edge_map.end()) {
        it_b->second.f_a = f;
      }
      else {
        edge_map.emplace(v_other_b, FacePair(f, f));
      }
    }

    std::vector<std::pair<int, FacePair *>> verts_to_collapse;
    verts_to_collapse.reserve(edge_map.size());
    for (auto &[v_other, fpair] : edge_map) {
      if (fpair.f_a != fpair.f_b) {
        verts_to_collapse.push_back({v_other, &fpair});
      }
    }

    const Vert &co_pivot = verts_table.co_at_index(v_pivot);
    std::sort(verts_to_collapse.begin(),
              verts_to_collapse.end(),
              [&verts_table, &co_pivot](const std::pair<int, FacePair *> &a,
                                        const std::pair<int, FacePair *> &b) {
                const Scalar len_a = len_squared_v2v2(co_pivot, verts_table.co_at_index(a.first));
                const Scalar len_b = len_squared_v2v2(co_pivot, verts_table.co_at_index(b.first));
                if (len_a != len_b) {
                  return len_a < len_b;
                }
                return a.first < b.first;
              });

    std::vector<std::pair<int, FacePair *>> verts_to_collapse_next;
    bool any_success = false;
    bool all_failed = false;

    while (!verts_to_collapse.empty()) {
      const std::pair<int, FacePair *> collapse_item = verts_to_collapse.back();
      int v_other = collapse_item.first;
      FacePair *const fpair = collapse_item.second;
      verts_to_collapse.pop_back();

      FaceResult *f_a = fpair->f_a;
      FaceResult *f_b = fpair->f_b;

      /* Find other vertices directly (only 2 values to check). */
      int v_other_a = -1, v_other_b = -1;
      for (const int v : f_a->tri) {
        if (v != v_pivot && v != v_other) {
          v_other_a = v;
          break;
        }
      }
      for (const int v : f_b->tri) {
        if (v != v_pivot && v != v_other) {
          v_other_b = v;
          break;
        }
      }

      /* Skip if faces share the same third vertex (degenerate case). */
      if (v_other_a == v_other_b) {
        continue;
      }

      /* Cache coordinate lookups for validation. */
      const Vert &co_other = verts_table.co_at_index(v_other);
      const Vert *co_other_a = &verts_table.co_at_index(v_other_a);
      const Vert *co_other_b = &verts_table.co_at_index(v_other_b);

      if (!face_is_valid_co(co_pivot, *co_other_a, *co_other_b)) {
        std::swap(fpair->f_a, fpair->f_b);
        std::swap(f_a, f_b);
        std::swap(v_other_a, v_other_b);
        std::swap(co_other_a, co_other_b);
      }

      assert(face_is_valid_co(co_pivot, *co_other_a, *co_other_b));

      if ((!verts_to_collapse.empty() || !verts_to_collapse_next.empty()) &&
          !force_collapse_degenerate && !face_is_valid_co(co_other, *co_other_b, *co_other_a))
      {
        verts_to_collapse_next.push_back({v_other, fpair});
      }
      else {
        any_success = true;

        for (FaceResult *f_iter : {f_a, f_b}) {
          for (const int v_iter : f_iter->tri) {
            if (v_iter >= 0) {
              continue;
            }
            find_and_swap_erase(coords_face_vec[v_iter ^ -1], f_iter);
          }
        }

        f_a->tri = {v_pivot, v_other_a, v_other_b};
        f_b->tri = {v_other_a, v_other_b, v_other};

        for (FaceResult *f_iter : {f_a, f_b}) {
          for (const int v_iter : f_iter->tri) {
            if (v_iter >= 0) {
              continue;
            }
            coords_face_vec[v_iter ^ -1].push_back(f_iter);
          }
        }

        auto it_other = edge_map.find(v_other_b);
        if (it_other != edge_map.end()) {
          if (f_b == it_other->second.f_a) {
            it_other->second.f_a = f_a;
          }
          else if (f_b == it_other->second.f_b) {
            it_other->second.f_b = f_a;
          }
        }
      }

      if (verts_to_collapse.empty()) {
        if (!verts_to_collapse_next.empty()) {
          std::swap(verts_to_collapse, verts_to_collapse_next);
          if (!force_collapse_degenerate && !any_success) {
            all_failed = true;
            break;
          }
          any_success = false;
        }
        else {
          /* Remove last face by marking it invalid (filtered out in solve()). */
          for (const int v_iter : f_a->tri) {
            if (v_iter >= 0) {
              continue;
            }
            find_and_swap_erase(coords_face_vec[v_iter ^ -1], f_a);
          }
          f_a->tri = {
              TRI_INDEX_REMOVED, TRI_INDEX_REMOVED, TRI_INDEX_REMOVED}; /* Mark as invalid. */
        }
      }
    }

    if (all_failed) {
      verts_tess_to_handle_next.push_back(v_pivot_base);
    }
  }

  return verts_tess_to_handle_next;
}

void rotate_edges_away(VertsTable &verts_table, std::vector<FaceResult> &faces_result)
{
  if (verts_table.coords_tess.empty()) {
    return;
  }

  std::vector<std::vector<FaceResult *>> coords_face_vec(verts_table.coords_tess.size());

  for (FaceResult &f : faces_result) {
    for (const int v : f.tri) {
      if (v < 0) {
        coords_face_vec[v ^ -1].push_back(&f);
      }
    }
  }

  if constexpr (DO_SIMPLIFY_EDGE_ROTATE_WELD_FOR_EVENT_OVERLAP) {
    /* This is a special welding pass that is needed when there are overlapping
     * ENTER/EXIT events (events that share a vertex coordinate *exactly*).
     *
     * Consider the tips of two diamond shapes touching, when one is on top of the other.
     * When this shape is *inside* another shape (defining two diamond holes),
     * the ENTER/EXIT events cause Y aligned edges to be created - connected to adjacent chains.
     *
     * In this case the ENTER/EXIT events will use different vertex indices,
     * causing the edge to be considered a *boundary* which then fails to be removed.
     *
     * Resolve the error by performing a weld pre-pass.
     *
     * NOTE(@ideasman42): de-duplicating the vertices earlier in the filling process could work
     * and has the advantage that a separate pass on the faces isn't needed but seems
     * error prone since it breaks the guarantee that every vertex has exactly 2 connected edges.
     */
    std::unordered_map<int, int> vmap_dupe;

    for (int v_base = 0; v_base < int(coords_face_vec.size()); v_base++) {
      const int v_index_tess = v_base ^ -1;
      /* This vertex may already be removed, no need detect welds. */
      if (vmap_dupe.contains(v_index_tess)) [[unlikely]] {
        continue;
      }
      /* Not essential, the entire block is a NO-OP in this case. */
      const std::vector<FaceResult *> &faces_of_vert = coords_face_vec[v_base];
      if (faces_of_vert.empty()) {
        continue;
      }

      /* Since this is a tessellation vertex,
       * check for adjacent verts connected to geometry directly above or below. */
      int v_above = -1;
      int v_below = -1;

      /* Build topological data. */
      const Vert &v_tess_co = verts_table.co_at_index(v_index_tess);
      for (const FaceResult *f : faces_of_vert) {
        /* Find pivot position directly. */
        int v_in_face;
        if (f->tri[0] == v_index_tess) {
          v_in_face = 0;
        }
        else if (f->tri[1] == v_index_tess) {
          v_in_face = 1;
        }
        else {
          v_in_face = 2;
        }

        /* Find the horizontal edge (if any). */
        int v_in_face_non_y_aligned = -1;
        int v_index_other = -1;
        for (int j = 0; j < 3; j++) {
          if (j == v_in_face) {
            continue;
          }
          const int v_other = f->tri[j];
          const Vert &v_other_co = verts_table.co_at_index(v_other);
          /* Only consider exactly Y aligned vertices. */
          if (v_other_co[1] == v_tess_co[1]) {
            v_in_face_non_y_aligned = tri_corner_opposite_edge(v_in_face, j);
            v_index_other = v_other;
            break;
          }
        }

        if (v_in_face_non_y_aligned == -1) {
          continue;
        }

        const Scalar y_other = verts_table.co_at_index(f->tri[v_in_face_non_y_aligned])[1];
        if (y_other > v_tess_co[1]) {
          v_above = v_index_other;
          if (v_below != -1) {
            break;
          }
        }
        else if (y_other < v_tess_co[1]) {
          v_below = v_index_other;
          if (v_above != -1) {
            break;
          }
        }
      }

      if (v_above != -1 && v_below != -1 && v_above != v_below) {
        vmap_dupe[v_above] = v_below;
      }
    }

    /* Logically this should never happen: ensure the replacement indices are *never*
     * themselves pointing to indices (no chaining of replacements). */
    assert([&]() {
      for (const auto &[k, v] : vmap_dupe) {
        if (vmap_dupe.contains(v)) {
          return false;
        }
      }
      return true;
    }());

    if (!vmap_dupe.empty()) [[unlikely]] {
      for (std::vector<FaceResult *> &faces_of_vert : coords_face_vec) {
        for (FaceResult *f : faces_of_vert) {
          for (int j = 0; j < 3; j++) {
            const auto it = vmap_dupe.find(f->tri[j]);
            if (it != vmap_dupe.end()) {
              f->tri[j] = it->second;
            }
          }
        }
      }
    }
  }
  /* End `DO_SIMPLIFY_EDGE_ROTATE_WELD_FOR_EVENT_OVERLAP` logic. */

  std::vector<int> verts_tess_to_handle;
  verts_tess_to_handle.reserve(verts_table.coords_tess.size());
  for (int i = 0; i < int(verts_table.coords_tess.size()); i++) {
    verts_tess_to_handle.push_back(i);
  }

  int verts_tess_to_handle_len = verts_tess_to_handle.size();
  bool force_collapse_degenerate = false;

  /* NOTE(@ideasman42): regarding redundant processing:
   * It might seem worth tracking "dirty" faces so edges are only considered for rotating
   * when their neighbors have rotated, instead of considering all edges every pass.
   * In practice I found this didn't work very well, giving negligible performance gains.
   *
   * NOTE: a single pass is practically always sufficient. */
  while (!verts_tess_to_handle.empty()) {
    verts_tess_to_handle = rotate_edges_away_pass(verts_table,
                                                  faces_result,
                                                  coords_face_vec,
                                                  /* These can change each call. */
                                                  verts_tess_to_handle,
                                                  force_collapse_degenerate);

    if (force_collapse_degenerate) {
      break;
    }
    /* If nothing was collapsed, force collapse the remaining vertices. */
    if (verts_tess_to_handle_len == int(verts_tess_to_handle.size())) {
      force_collapse_degenerate = true;
    }
    verts_tess_to_handle_len = verts_tess_to_handle.size();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main solve function
 * \{ */

std::vector<Face> solve(const PolyFillParams &params,
                        std::span<const Vert> verts,
                        const VertsEdgeMap &verts_edge_map)
{
  std::vector<FaceResult> faces_result;
  /* Reserve estimated capacity. */
  faces_result.reserve(verts.size() * 2);

  VertsTable verts_table(verts);
  SweepInterval si(params, verts_table, verts_edge_map);

  FaceConstructContext fctx = {params, faces_result, verts_table};

  /* Handle Events in the Sweep-Line.
   * Scan up the Y axis, processing all events. */
  while (si.step_event()) {
    /* Pass. */
  }

  /* Region Extraction.
   * Scan events for monotonic region pairs, filling them with triangles.
   *
   * NOTE: It should be possible to parallelized this as the pairs are isolated.
   * Although the resulting faces will have to be accumulated into local arrays,
   * then merged in the main thread. */

  if constexpr (DO_FILL) {
    si.step_regions([&](const VertChain_RegionInfo &region) {
      if (region.indices_l.is_empty()) {
        return;
      }
      if (region.indices_r.is_empty()) {
        return;
      }
      si.fill_region(fctx, region.indices_l, region.indices_r, region.fan_beg, region.fan_end);
    });
  }

  if constexpr (DO_SIMPLIFY_EDGE_ROTATE && DO_FILL) {
    rotate_edges_away(verts_table, faces_result);

    /* Tessellation vertices (`verts_table.coords_tess`) should *not* be accessed from now on.
     * Comment the following line if they are needed for debugging. */
    verts_table.coords_tess.clear();
  }

  std::vector<Face> faces;
  faces.reserve(faces_result.size());
  for (const FaceResult &f : faces_result) {
    /* Skip degenerate faces with duplicate vertices
     * (also catches invalid faces marked during `rotate_edges_away`). */
    if (f.tri[0] == f.tri[1] || f.tri[1] == f.tri[2] || f.tri[0] == f.tri[2]) {
      continue;
    }
    /* Skip faces that still have tessellation vertices (negative indices). */
    if (f.tri[0] < 0 || f.tri[1] < 0 || f.tri[2] < 0) {
      continue;
    }
    faces.push_back(f.tri);
  }

  return faces;
}

} /* anonymous namespace */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

/** Returns a vertex aligned list of adjacent edges. */
VertsEdgeMap verts_edge_map_calc_from_edges(std::span<const Edge> edges, const int verts_len)
{
  if (edges.empty() || verts_len <= 0) {
    return {};
  }

  VertsEdgeMap result(size_t(verts_len), {-1, -1});

  for (const auto &[v0, v1] : edges) {
    assert(v0 >= 0 && v0 < verts_len);
    assert(v1 >= 0 && v1 < verts_len);
    for (const auto &[v, v_other] : {std::pair<int, int>{v0, v1}, std::pair<int, int>{v1, v0}}) {
      std::array<int, 2> &vmap = result[v];
      /* Would occur if 3+ edges ever share a single vertex. */
      assert(vmap[1] == -1);
      if (vmap[0] == -1) {
        vmap[0] = v_other;
      }
      else {
        vmap[1] = v_other;
      }
    }
  }

  return result;
}

std::vector<Face> poly_fill(std::span<const Vert> verts,
                            std::span<const Edge> edges,
                            const bool degenerate)
{
  VertsEdgeMap edge_map = verts_edge_map_calc_from_edges(edges, int(verts.size()));
  return poly_fill_with_edge_map(verts, edge_map, degenerate);
}

std::vector<Face> poly_fill_with_edge_map(std::span<const Vert> verts,
                                          const VertsEdgeMap &verts_edge_map,
                                          const bool degenerate)
{
  if (verts_edge_map.empty()) {
    return {};
  }

  assert(verts.size() == verts_edge_map.size());

  PolyFillParams params;
  params.degenerate = degenerate;

  return solve(params, verts, verts_edge_map);
}

/** \} */

} /* namespace poly_fill */
