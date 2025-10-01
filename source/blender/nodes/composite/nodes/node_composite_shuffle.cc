/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_assert.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector_types.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "GPU_material.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_shuffle_cc {

NODE_STORAGE_FUNCS(NodeCMPShuffle)

static void cmp_node_shuffle_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>("Image");
}

static void node_cmp_shuffle_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeCMPShuffle *data = MEM_callocN<NodeCMPShuffle>(__func__);
  data->red = CMP_NODE_SHUFFLE_CHANNEL_RED;
  data->green = CMP_NODE_SHUFFLE_CHANNEL_GREEN;
  data->blue = CMP_NODE_SHUFFLE_CHANNEL_BLUE;
  data->alpha = CMP_NODE_SHUFFLE_CHANNEL_ALPHA;
  data->mode = CMP_NODE_COMBSEP_COLOR_RGB;
  data->ycc_mode = BLI_YCC_ITU_BT709;
  data->use_red_expr = 0;
  data->use_green_expr = 0;
  data->use_blue_expr = 0;
  data->use_alpha_expr = 0;
  data->red_expr = nullptr;
  data->green_expr = nullptr;
  data->blue_expr = nullptr;
  data->alpha_expr = nullptr;
  node->storage = data;
  node->width = 280.0f;
}

static void node_storage_free(bNode *node)
{
  NodeCMPShuffle *storage = (NodeCMPShuffle *)node->storage;
  if (storage == nullptr) {
    return;
  }
  if (storage->red_expr != nullptr) {
    MEM_freeN(storage->red_expr);
  }
  if (storage->green_expr != nullptr) {
    MEM_freeN(storage->green_expr);
  }
  if (storage->blue_expr != nullptr) {
    MEM_freeN(storage->blue_expr);
  }
  if (storage->alpha_expr != nullptr) {
    MEM_freeN(storage->alpha_expr);
  }
  MEM_freeN(storage);
}

static void node_storage_copy(bNodeTree * /*dst_ntree*/, bNode *dest_node, const bNode *src_node)
{
  NodeCMPShuffle *source_storage = (NodeCMPShuffle *)src_node->storage;
  NodeCMPShuffle *destination_storage = (NodeCMPShuffle *)MEM_dupallocN(source_storage);

  if (source_storage->red_expr) {
    destination_storage->red_expr = (char *)MEM_dupallocN(source_storage->red_expr);
  }
  if (source_storage->green_expr) {
    destination_storage->green_expr = (char *)MEM_dupallocN(source_storage->green_expr);
  }
  if (source_storage->blue_expr) {
    destination_storage->blue_expr = (char *)MEM_dupallocN(source_storage->blue_expr);
  }
  if (source_storage->alpha_expr) {
    destination_storage->alpha_expr = (char *)MEM_dupallocN(source_storage->alpha_expr);
  }

  dest_node->storage = destination_storage;
}

static void node_draw_buttons(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  const NodeCMPShuffle *storage = (NodeCMPShuffle *)((bNode *)ptr->data)->storage;

  switch (storage->mode) {
    case CMP_NODE_COMBSEP_COLOR_RGB: {
      uiLayout *split = &layout->split(0.15f, false);
      uiLayout *col_label = &split->column(false);
      uiLayout *label_row = &col_label->row(false);
      label_row->label("R", ICON_NONE);
      uiLayout *col_controls = &split->column(false);
      uiLayout *row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_red_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_red_expr) {
        row->prop(ptr, "red_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "red", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("G", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_green_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_green_expr) {
        row->prop(ptr, "green_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "green", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("B", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_blue_expr) {
        row->prop(ptr, "blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "blue", UI_ITEM_NONE, "", ICON_NONE);
      }
      break;
    }
    case CMP_NODE_COMBSEP_COLOR_HSV: {
      uiLayout *split = &layout->split(0.15f, false);
      uiLayout *col_label = &split->column(false);
      uiLayout *label_row = &col_label->row(false);
      label_row->label("H", ICON_NONE);
      uiLayout *col_controls = &split->column(false);
      uiLayout *row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_red_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_red_expr) {
        row->prop(ptr, "red_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "red", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("S", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_green_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_green_expr) {
        row->prop(ptr, "green_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "green", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("V", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_blue_expr) {
        row->prop(ptr, "blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "blue", UI_ITEM_NONE, "", ICON_NONE);
      }
      break;
    }
    case CMP_NODE_COMBSEP_COLOR_HSL: {
      uiLayout *split = &layout->split(0.15f, false);
      uiLayout *col_label = &split->column(false);
      uiLayout *label_row = &col_label->row(false);
      label_row->label("H", ICON_NONE);
      uiLayout *col_controls = &split->column(false);
      uiLayout *row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_red_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_red_expr) {
        row->prop(ptr, "red_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "red", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("S", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_green_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_green_expr) {
        row->prop(ptr, "green_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "green", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("L", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_blue_expr) {
        row->prop(ptr, "blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "blue", UI_ITEM_NONE, "", ICON_NONE);
      }
      break;
    }
    case CMP_NODE_COMBSEP_COLOR_YCC: {
      uiLayout *split = &layout->split(0.15f, false);
      uiLayout *col_label = &split->column(false);
      uiLayout *label_row = &col_label->row(false);
      label_row->label("Y", ICON_NONE);
      uiLayout *col_controls = &split->column(false);
      uiLayout *row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_red_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_red_expr) {
        row->prop(ptr, "red_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "red", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("Cb", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_green_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_green_expr) {
        row->prop(ptr, "green_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "green", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("Cr", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_blue_expr) {
        row->prop(ptr, "blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "blue", UI_ITEM_NONE, "", ICON_NONE);
      }
      break;
    }
    case CMP_NODE_COMBSEP_COLOR_YUV: {
      uiLayout *split = &layout->split(0.15f, false);
      uiLayout *col_label = &split->column(false);
      uiLayout *label_row = &col_label->row(false);
      label_row->label("Y", ICON_NONE);
      uiLayout *col_controls = &split->column(false);
      uiLayout *row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_red_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_red_expr) {
        row->prop(ptr, "red_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "red", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("U", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_green_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_green_expr) {
        row->prop(ptr, "green_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "green", UI_ITEM_NONE, "", ICON_NONE);
      }

      split = &layout->split(0.15f, false);
      col_label = &split->column(false);
      label_row = &col_label->row(false);
      label_row->label("V", ICON_NONE);
      col_controls = &split->column(false);
      row = &col_controls->row(true);
      row->separator();
      row->prop(ptr, "use_blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      if (storage->use_blue_expr) {
        row->prop(ptr, "blue_expr", UI_ITEM_NONE, "", ICON_NONE);
      }
      else {
        row->prop(ptr, "blue", UI_ITEM_NONE, "", ICON_NONE);
      }
      break;
    }
  }

  uiLayout *split = &layout->split(0.15f, false);
  uiLayout *col_label = &split->column(false);
  uiLayout *label_row = &col_label->row(false);
  label_row->label("A", ICON_NONE);
  uiLayout *col_controls = &split->column(false);
  uiLayout *row = &col_controls->row(true);
  row->separator();
  row->prop(ptr, "use_alpha_expr", UI_ITEM_NONE, "", ICON_NONE);
  if (storage->use_alpha_expr) {
    row->prop(ptr, "alpha_expr", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    row->prop(ptr, "alpha", UI_ITEM_NONE, "", ICON_NONE);
  }

  layout->separator();
  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (storage->mode == CMP_NODE_COMBSEP_COLOR_YCC) {
    layout->prop(ptr, "ycc_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

using namespace blender::compositor;

struct ColorChannels {
  float r, g, b, a;
  float h, s, v, l;
  float y, yuv_u, yuv_v;
  float cb, cr;
  float z;
  float forward_u, forward_v, backward_u, backward_v;
};

class ExpressionEvaluator {
 private:
  const char *expr_;
  size_t pos_;
  const ColorChannels &channels_;

  char peek() const
  {
    return expr_[pos_];
  }

  char get()
  {
    return expr_[pos_++];
  }

  void skip_whitespace()
  {
    while (peek() == ' ' || peek() == '\t') {
      pos_++;
    }
  }

  float parse_number()
  {
    skip_whitespace();
    float result = 0.0f;
    float decimal = 0.0f;
    int decimal_places = 0;
    bool negative = false;

    if (peek() == '-') {
      negative = true;
      pos_++;
    }

    while (peek() >= '0' && peek() <= '9') {
      result = result * 10.0f + (get() - '0');
    }

    if (peek() == '.') {
      pos_++;
      while (peek() >= '0' && peek() <= '9') {
        decimal = decimal * 10.0f + (get() - '0');
        decimal_places++;
      }
    }

    result += decimal / powf(10.0f, float(decimal_places));
    return negative ? -result : result;
  }

  bool match_string(const char *str)
  {
    size_t i = 0;
    while (str[i] != '\0') {
      if (expr_[pos_ + i] != str[i]) {
        return false;
      }
      i++;
    }
    char next = expr_[pos_ + i];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') || next == '.') {
      return false;
    }
    pos_ += i;
    return true;
  }

  float parse_variable()
  {
    skip_whitespace();
    size_t start_pos = pos_;

    if (match_string("forward.u")) {
      return channels_.forward_u;
    }
    pos_ = start_pos;
    if (match_string("forward.v")) {
      return channels_.forward_v;
    }
    pos_ = start_pos;
    if (match_string("backward.u")) {
      return channels_.backward_u;
    }
    pos_ = start_pos;
    if (match_string("backward.v")) {
      return channels_.backward_v;
    }
    pos_ = start_pos;

    if (match_string("fu")) {
      return channels_.forward_u;
    }
    pos_ = start_pos;
    if (match_string("fv")) {
      return channels_.forward_v;
    }
    pos_ = start_pos;
    if (match_string("bu")) {
      return channels_.backward_u;
    }
    pos_ = start_pos;
    if (match_string("bv")) {
      return channels_.backward_v;
    }
    pos_ = start_pos;

    if (match_string("red")) {
      return channels_.r;
    }
    pos_ = start_pos;
    if (match_string("green")) {
      return channels_.g;
    }
    pos_ = start_pos;
    if (match_string("blue")) {
      return channels_.b;
    }
    pos_ = start_pos;
    if (match_string("alpha")) {
      return channels_.a;
    }
    pos_ = start_pos;

    if (match_string("hue")) {
      return channels_.h;
    }
    pos_ = start_pos;
    if (match_string("saturation")) {
      return channels_.s;
    }
    pos_ = start_pos;
    if (match_string("sat")) {
      return channels_.s;
    }
    pos_ = start_pos;
    if (match_string("value")) {
      return channels_.v;
    }
    pos_ = start_pos;
    if (match_string("val")) {
      return channels_.v;
    }
    pos_ = start_pos;

    if (match_string("luminance")) {
      return channels_.l;
    }
    pos_ = start_pos;
    if (match_string("lumi")) {
      return channels_.l;
    }
    pos_ = start_pos;
    if (match_string("luma")) {
      return channels_.y;
    }
    pos_ = start_pos;

    if (match_string("chromiu")) {
      return channels_.yuv_u;
    }
    pos_ = start_pos;
    if (match_string("chromiv")) {
      return channels_.yuv_v;
    }
    pos_ = start_pos;

    if (match_string("Cr")) {
      return channels_.cr;
    }
    pos_ = start_pos;
    if (match_string("Cb")) {
      return channels_.cb;
    }
    pos_ = start_pos;

    char c = get();
    switch (c) {
      case 'r':
        return channels_.r;
      case 'g':
        return channels_.g;
      case 'b':
        return channels_.b;
      case 'a':
        return channels_.a;
      case 'h':
        return channels_.h;
      case 's':
        return channels_.s;
      case 'v':
        return channels_.v;
      case 'l':
        return channels_.l;
      case 'Y':
        return channels_.y;
      case 'U':
        return channels_.yuv_u;
      case 'V':
        return channels_.yuv_v;
      case 'z':
        return channels_.z;
      default:
        return 0.0f;
    }
  }

  float parse_function()
  {
    skip_whitespace();
    size_t start_pos = pos_;

    if (match_string("smoothmax")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float k = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        float h = max_ff(k - fabsf(a - b), 0.0f);
        return max_ff(a, b) + h * h * 0.25f / k;
      }
    }
    pos_ = start_pos;

    if (match_string("smoothmin")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float k = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        float h = max_ff(k - fabsf(a - b), 0.0f);
        return min_ff(a, b) - h * h * 0.25f / k;
      }
    }
    pos_ = start_pos;

    if (match_string("clamp")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float min_val = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float max_val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return clamp_f(val, min_val, max_val);
      }
    }
    pos_ = start_pos;

    if (match_string("mix")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float t = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return interpf(b, a, t);
      }
    }
    pos_ = start_pos;

    if (match_string("min")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return min_ff(a, b);
      }
    }
    pos_ = start_pos;

    if (match_string("max")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return max_ff(a, b);
      }
    }
    pos_ = start_pos;

    if (match_string("pow")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float base = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float exp = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return powf(base, exp);
      }
    }
    pos_ = start_pos;

    if (match_string("sqrt")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return sqrtf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("abs")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return fabsf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("floor")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return floorf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("ceil")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return ceilf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("sin")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return sinf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("cos")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return cosf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("tan")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return tanf(val);
      }
    }
    pos_ = start_pos;

    return parse_variable();
  }

  float parse_factor()
  {
    skip_whitespace();
    char c = peek();

    if (c == '(') {
      pos_++;
      float result = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      return result;
    }

    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      return parse_number();
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      return parse_function();
    }

    return 0.0f;
  }

  float parse_power()
  {
    float result = parse_factor();

    while (true) {
      skip_whitespace();
      if (peek() == '^') {
        pos_++;
        result = powf(result, parse_factor());
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_term()
  {
    float result = parse_power();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '*') {
        pos_++;
        result *= parse_power();
      }
      else if (op == '/') {
        pos_++;
        float divisor = parse_power();
        result = divisor != 0.0f ? result / divisor : 0.0f;
      }
      else if (op == '%') {
        pos_++;
        float divisor = parse_power();
        result = divisor != 0.0f ? fmodf(result, divisor) : 0.0f;
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_expression()
  {
    float result = parse_term();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '+') {
        pos_++;
        result += parse_term();
      }
      else if (op == '-') {
        pos_++;
        result -= parse_term();
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_comparison()
  {
    float result = parse_expression();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '<') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result <= parse_expression()) ? 1.0f : 0.0f;
        }
        else {
          result = (result < parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '>') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result >= parse_expression()) ? 1.0f : 0.0f;
        }
        else {
          result = (result > parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '=') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result == parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '!') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result != parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_logical_and()
  {
    float result = parse_comparison();

    while (true) {
      skip_whitespace();
      size_t start_pos = pos_;
      bool is_and = false;

      if (peek() == '&') {
        pos_++;
        is_and = true;
      }
      else if (match_string("and")) {
        is_and = true;
      }

      if (is_and) {
        float right = parse_comparison();
        result = (result != 0.0f && right != 0.0f) ? 1.0f : 0.0f;
      }
      else {
        pos_ = start_pos;
        break;
      }
    }

    return result;
  }

  float parse_logical_or()
  {
    float result = parse_logical_and();

    while (true) {
      skip_whitespace();
      size_t start_pos = pos_;
      bool is_or = false;

      if (peek() == '|') {
        pos_++;
        is_or = true;
      }
      else if (match_string("or")) {
        is_or = true;
      }

      if (is_or) {
        float right = parse_logical_and();
        result = (result != 0.0f || right != 0.0f) ? 1.0f : 0.0f;
      }
      else {
        pos_ = start_pos;
        break;
      }
    }

    return result;
  }

  float parse_ternary()
  {
    float result = parse_logical_or();

    skip_whitespace();
    if (peek() == '?') {
      pos_++;
      float true_value = parse_ternary();
      skip_whitespace();
      if (peek() == ':') {
        pos_++;
      }
      float false_value = parse_ternary();
      result = (result != 0.0f) ? true_value : false_value;
    }

    return result;
  }

 public:
  ExpressionEvaluator(const char *expr, const ColorChannels &channels)
      : expr_(expr), pos_(0), channels_(channels)
  {
  }

  float evaluate()
  {
    if (expr_ == nullptr || expr_[0] == '\0') {
      return 0.0f;
    }
    return parse_ternary();
  }
};

static float evaluate_expression(const char *expr, const ColorChannels &channels)
{
  if (expr == nullptr || expr[0] == '\0') {
    return 0.0f;
  }
  ExpressionEvaluator evaluator(expr, channels);
  return evaluator.evaluate();
}

static float get_channel_value(const float4 &color, uint8_t channel)
{
  switch (channel) {
    case CMP_NODE_SHUFFLE_CHANNEL_RED:
      return color.x;
    case CMP_NODE_SHUFFLE_CHANNEL_GREEN:
      return color.y;
    case CMP_NODE_SHUFFLE_CHANNEL_BLUE:
      return color.z;
    case CMP_NODE_SHUFFLE_CHANNEL_ALPHA:
      return color.w;
    case CMP_NODE_SHUFFLE_CHANNEL_HUE:
    case CMP_NODE_SHUFFLE_CHANNEL_SATURATION:
    case CMP_NODE_SHUFFLE_CHANNEL_VALUE: {
      float4 hsv;
      rgb_to_hsv(color.x, color.y, color.z, &hsv.x, &hsv.y, &hsv.z);
      if (channel == CMP_NODE_SHUFFLE_CHANNEL_HUE) return hsv.x;
      if (channel == CMP_NODE_SHUFFLE_CHANNEL_SATURATION) return hsv.y;
      return hsv.z;
    }
    case CMP_NODE_SHUFFLE_CHANNEL_LIGHTNESS: {
      float4 hsl;
      rgb_to_hsl(color.x, color.y, color.z, &hsl.x, &hsl.y, &hsl.z);
      return hsl.z;
    }
    case CMP_NODE_SHUFFLE_CHANNEL_Y:
    case CMP_NODE_SHUFFLE_CHANNEL_U:
    case CMP_NODE_SHUFFLE_CHANNEL_V: {
      float4 yuv;
      rgb_to_yuv(color.x, color.y, color.z, &yuv.x, &yuv.y, &yuv.z, BLI_YUV_ITU_BT709);
      if (channel == CMP_NODE_SHUFFLE_CHANNEL_Y) return yuv.x;
      if (channel == CMP_NODE_SHUFFLE_CHANNEL_U) return yuv.y;
      return yuv.z;
    }
    case CMP_NODE_SHUFFLE_CHANNEL_CB:
    case CMP_NODE_SHUFFLE_CHANNEL_CR: {
      float4 ycc;
      rgb_to_ycc(color.x, color.y, color.z, &ycc.x, &ycc.y, &ycc.z, BLI_YCC_ITU_BT709);
      if (channel == CMP_NODE_SHUFFLE_CHANNEL_CB) return ycc.y / 255.0f;
      return ycc.z / 255.0f;
    }
    case CMP_NODE_SHUFFLE_CHANNEL_FORWARD_U:
      return color.x;
    case CMP_NODE_SHUFFLE_CHANNEL_FORWARD_V:
      return color.y;
    case CMP_NODE_SHUFFLE_CHANNEL_BACKWARD_U:
      return color.z;
    case CMP_NODE_SHUFFLE_CHANNEL_BACKWARD_V:
      return color.w;
    case CMP_NODE_SHUFFLE_CHANNEL_DEPTH:
      return color.x;
    case CMP_NODE_SHUFFLE_CHANNEL_BLACK:
      return 0.0f;
    case CMP_NODE_SHUFFLE_CHANNEL_WHITE:
      return 1.0f;
    default:
      return 0.0f;
  }
}

static int node_gpu_material(GPUMaterial *material,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *inputs,
                             GPUNodeStack *outputs)
{
  const NodeCMPShuffle &storage = node_storage(*node);

  if (storage.use_red_expr || storage.use_green_expr || storage.use_blue_expr ||
      storage.use_alpha_expr)
  {
    return false;
  }

  float red = storage.red;
  float green = storage.green;
  float blue = storage.blue;
  float alpha = storage.alpha;

  switch (storage.mode) {
    case CMP_NODE_COMBSEP_COLOR_RGB:
      return GPU_stack_link(material,
                            node,
                            "node_composite_shuffle_rgb",
                            inputs,
                            outputs,
                            GPU_constant(&red),
                            GPU_constant(&green),
                            GPU_constant(&blue),
                            GPU_constant(&alpha));
    case CMP_NODE_COMBSEP_COLOR_HSV:
      return GPU_stack_link(material,
                            node,
                            "node_composite_shuffle_hsv",
                            inputs,
                            outputs,
                            GPU_constant(&red),
                            GPU_constant(&green),
                            GPU_constant(&blue),
                            GPU_constant(&alpha));
    case CMP_NODE_COMBSEP_COLOR_HSL:
      return GPU_stack_link(material,
                            node,
                            "node_composite_shuffle_hsl",
                            inputs,
                            outputs,
                            GPU_constant(&red),
                            GPU_constant(&green),
                            GPU_constant(&blue),
                            GPU_constant(&alpha));
    case CMP_NODE_COMBSEP_COLOR_YUV:
      return GPU_stack_link(material,
                            node,
                            "node_composite_shuffle_yuv",
                            inputs,
                            outputs,
                            GPU_constant(&red),
                            GPU_constant(&green),
                            GPU_constant(&blue),
                            GPU_constant(&alpha));
    case CMP_NODE_COMBSEP_COLOR_YCC:
      switch (storage.ycc_mode) {
        case BLI_YCC_ITU_BT601:
          return GPU_stack_link(material,
                                node,
                                "node_composite_shuffle_ycc_itu_601",
                                inputs,
                                outputs,
                                GPU_constant(&red),
                                GPU_constant(&green),
                                GPU_constant(&blue),
                                GPU_constant(&alpha));
        case BLI_YCC_ITU_BT709:
          return GPU_stack_link(material,
                                node,
                                "node_composite_shuffle_ycc_itu_709",
                                inputs,
                                outputs,
                                GPU_constant(&red),
                                GPU_constant(&green),
                                GPU_constant(&blue),
                                GPU_constant(&alpha));
        case BLI_YCC_JFIF_0_255:
          return GPU_stack_link(material,
                                node,
                                "node_composite_shuffle_ycc_jpeg",
                                inputs,
                                outputs,
                                GPU_constant(&red),
                                GPU_constant(&green),
                                GPU_constant(&blue),
                                GPU_constant(&alpha));
      }
      break;
  }

  return false;
}

class ShuffleRGBFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;
  uint8_t use_red_expr_, use_green_expr_, use_blue_expr_, use_alpha_expr_;
  const char *red_expr_, *green_expr_, *blue_expr_, *alpha_expr_;

 public:
  ShuffleRGBFunction(uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha,
                     uint8_t use_red_expr,
                     uint8_t use_green_expr,
                     uint8_t use_blue_expr,
                     uint8_t use_alpha_expr,
                     const char *red_expr,
                     const char *green_expr,
                     const char *blue_expr,
                     const char *alpha_expr)
      : red_(red),
        green_(green),
        blue_(blue),
        alpha_(alpha),
        use_red_expr_(use_red_expr),
        use_green_expr_(use_green_expr),
        use_blue_expr_(use_blue_expr),
        use_alpha_expr_(use_alpha_expr),
        red_expr_(red_expr),
        green_expr_(green_expr),
        blue_expr_(blue_expr),
        alpha_expr_(alpha_expr)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle RGB", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      ColorChannels channels;
      channels.r = input[i].x;
      channels.g = input[i].y;
      channels.b = input[i].z;
      channels.a = input[i].w;
      rgb_to_hsv(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.v);
      rgb_to_hsl(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.l);
      rgb_to_yuv(channels.r, channels.g, channels.b, &channels.y, &channels.yuv_u, &channels.yuv_v, BLI_YUV_ITU_BT709);
      float ycc_y, ycc_cb, ycc_cr;
      rgb_to_ycc(channels.r, channels.g, channels.b, &ycc_y, &ycc_cb, &ycc_cr, BLI_YCC_ITU_BT709);
      channels.cb = ycc_cb / 255.0f;
      channels.cr = ycc_cr / 255.0f;
      channels.z = 0.0f;
      channels.forward_u = 0.0f;
      channels.forward_v = 0.0f;
      channels.backward_u = 0.0f;
      channels.backward_v = 0.0f;

      float r = use_red_expr_ ? evaluate_expression(red_expr_, channels) :
                                get_channel_value(input[i], red_);
      float g = use_green_expr_ ? evaluate_expression(green_expr_, channels) :
                                  get_channel_value(input[i], green_);
      float b = use_blue_expr_ ? evaluate_expression(blue_expr_, channels) :
                                 get_channel_value(input[i], blue_);
      float a = use_alpha_expr_ ? evaluate_expression(alpha_expr_, channels) :
                                  get_channel_value(input[i], alpha_);
      output[i] = float4(r, g, b, a);
    });
  }
};

class ShuffleHSVFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;
  uint8_t use_red_expr_, use_green_expr_, use_blue_expr_, use_alpha_expr_;
  const char *red_expr_, *green_expr_, *blue_expr_, *alpha_expr_;

 public:
  ShuffleHSVFunction(uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha,
                     uint8_t use_red_expr,
                     uint8_t use_green_expr,
                     uint8_t use_blue_expr,
                     uint8_t use_alpha_expr,
                     const char *red_expr,
                     const char *green_expr,
                     const char *blue_expr,
                     const char *alpha_expr)
      : red_(red),
        green_(green),
        blue_(blue),
        alpha_(alpha),
        use_red_expr_(use_red_expr),
        use_green_expr_(use_green_expr),
        use_blue_expr_(use_blue_expr),
        use_alpha_expr_(use_alpha_expr),
        red_expr_(red_expr),
        green_expr_(green_expr),
        blue_expr_(blue_expr),
        alpha_expr_(alpha_expr)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle HSV", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      ColorChannels channels;
      channels.r = input[i].x;
      channels.g = input[i].y;
      channels.b = input[i].z;
      channels.a = input[i].w;
      rgb_to_hsv(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.v);
      rgb_to_hsl(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.l);
      rgb_to_yuv(channels.r, channels.g, channels.b, &channels.y, &channels.yuv_u, &channels.yuv_v, BLI_YUV_ITU_BT709);
      float ycc_y, ycc_cb, ycc_cr;
      rgb_to_ycc(channels.r, channels.g, channels.b, &ycc_y, &ycc_cb, &ycc_cr, BLI_YCC_ITU_BT709);
      channels.cb = ycc_cb / 255.0f;
      channels.cr = ycc_cr / 255.0f;
      channels.z = 0.0f;
      channels.forward_u = 0.0f;
      channels.forward_v = 0.0f;
      channels.backward_u = 0.0f;
      channels.backward_v = 0.0f;

      float h = use_red_expr_ ? evaluate_expression(red_expr_, channels) :
                                get_channel_value(input[i], red_);
      float s = use_green_expr_ ? evaluate_expression(green_expr_, channels) :
                                  get_channel_value(input[i], green_);
      float v = use_blue_expr_ ? evaluate_expression(blue_expr_, channels) :
                                 get_channel_value(input[i], blue_);
      float a = use_alpha_expr_ ? evaluate_expression(alpha_expr_, channels) :
                                  get_channel_value(input[i], alpha_);

      hsv_to_rgb(h, s, v, &output[i].x, &output[i].y, &output[i].z);
      output[i].w = a;
    });
  }
};

class ShuffleHSLFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;
  uint8_t use_red_expr_, use_green_expr_, use_blue_expr_, use_alpha_expr_;
  const char *red_expr_, *green_expr_, *blue_expr_, *alpha_expr_;

 public:
  ShuffleHSLFunction(uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha,
                     uint8_t use_red_expr,
                     uint8_t use_green_expr,
                     uint8_t use_blue_expr,
                     uint8_t use_alpha_expr,
                     const char *red_expr,
                     const char *green_expr,
                     const char *blue_expr,
                     const char *alpha_expr)
      : red_(red),
        green_(green),
        blue_(blue),
        alpha_(alpha),
        use_red_expr_(use_red_expr),
        use_green_expr_(use_green_expr),
        use_blue_expr_(use_blue_expr),
        use_alpha_expr_(use_alpha_expr),
        red_expr_(red_expr),
        green_expr_(green_expr),
        blue_expr_(blue_expr),
        alpha_expr_(alpha_expr)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle HSL", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      ColorChannels channels;
      channels.r = input[i].x;
      channels.g = input[i].y;
      channels.b = input[i].z;
      channels.a = input[i].w;
      rgb_to_hsv(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.v);
      rgb_to_hsl(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.l);
      rgb_to_yuv(channels.r, channels.g, channels.b, &channels.y, &channels.yuv_u, &channels.yuv_v, BLI_YUV_ITU_BT709);
      float ycc_y, ycc_cb, ycc_cr;
      rgb_to_ycc(channels.r, channels.g, channels.b, &ycc_y, &ycc_cb, &ycc_cr, BLI_YCC_ITU_BT709);
      channels.cb = ycc_cb / 255.0f;
      channels.cr = ycc_cr / 255.0f;
      channels.z = 0.0f;
      channels.forward_u = 0.0f;
      channels.forward_v = 0.0f;
      channels.backward_u = 0.0f;
      channels.backward_v = 0.0f;

      float h = use_red_expr_ ? evaluate_expression(red_expr_, channels) :
                                get_channel_value(input[i], red_);
      float s = use_green_expr_ ? evaluate_expression(green_expr_, channels) :
                                  get_channel_value(input[i], green_);
      float l = use_blue_expr_ ? evaluate_expression(blue_expr_, channels) :
                                 get_channel_value(input[i], blue_);
      float a = use_alpha_expr_ ? evaluate_expression(alpha_expr_, channels) :
                                  get_channel_value(input[i], alpha_);

      hsl_to_rgb(h, s, l, &output[i].x, &output[i].y, &output[i].z);
      output[i].w = a;
    });
  }
};

class ShuffleYUVFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;
  uint8_t use_red_expr_, use_green_expr_, use_blue_expr_, use_alpha_expr_;
  const char *red_expr_, *green_expr_, *blue_expr_, *alpha_expr_;

 public:
  ShuffleYUVFunction(uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha,
                     uint8_t use_red_expr,
                     uint8_t use_green_expr,
                     uint8_t use_blue_expr,
                     uint8_t use_alpha_expr,
                     const char *red_expr,
                     const char *green_expr,
                     const char *blue_expr,
                     const char *alpha_expr)
      : red_(red),
        green_(green),
        blue_(blue),
        alpha_(alpha),
        use_red_expr_(use_red_expr),
        use_green_expr_(use_green_expr),
        use_blue_expr_(use_blue_expr),
        use_alpha_expr_(use_alpha_expr),
        red_expr_(red_expr),
        green_expr_(green_expr),
        blue_expr_(blue_expr),
        alpha_expr_(alpha_expr)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle YUV", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      ColorChannels channels;
      channels.r = input[i].x;
      channels.g = input[i].y;
      channels.b = input[i].z;
      channels.a = input[i].w;
      rgb_to_hsv(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.v);
      rgb_to_hsl(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.l);
      rgb_to_yuv(channels.r, channels.g, channels.b, &channels.y, &channels.yuv_u, &channels.yuv_v, BLI_YUV_ITU_BT709);
      float ycc_y, ycc_cb, ycc_cr;
      rgb_to_ycc(channels.r, channels.g, channels.b, &ycc_y, &ycc_cb, &ycc_cr, BLI_YCC_ITU_BT709);
      channels.cb = ycc_cb / 255.0f;
      channels.cr = ycc_cr / 255.0f;
      channels.z = 0.0f;
      channels.forward_u = 0.0f;
      channels.forward_v = 0.0f;
      channels.backward_u = 0.0f;
      channels.backward_v = 0.0f;

      float y = use_red_expr_ ? evaluate_expression(red_expr_, channels) :
                                get_channel_value(input[i], red_);
      float u = use_green_expr_ ? evaluate_expression(green_expr_, channels) :
                                  get_channel_value(input[i], green_);
      float v = use_blue_expr_ ? evaluate_expression(blue_expr_, channels) :
                                 get_channel_value(input[i], blue_);
      float a = use_alpha_expr_ ? evaluate_expression(alpha_expr_, channels) :
                                  get_channel_value(input[i], alpha_);

      yuv_to_rgb(y, u, v, &output[i].x, &output[i].y, &output[i].z, BLI_YUV_ITU_BT709);
      output[i].w = a;
    });
  }
};

class ShuffleYCCFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;
  int ycc_mode_;
  uint8_t use_red_expr_, use_green_expr_, use_blue_expr_, use_alpha_expr_;
  const char *red_expr_, *green_expr_, *blue_expr_, *alpha_expr_;

 public:
  ShuffleYCCFunction(uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha,
                     int ycc_mode,
                     uint8_t use_red_expr,
                     uint8_t use_green_expr,
                     uint8_t use_blue_expr,
                     uint8_t use_alpha_expr,
                     const char *red_expr,
                     const char *green_expr,
                     const char *blue_expr,
                     const char *alpha_expr)
      : red_(red),
        green_(green),
        blue_(blue),
        alpha_(alpha),
        ycc_mode_(ycc_mode),
        use_red_expr_(use_red_expr),
        use_green_expr_(use_green_expr),
        use_blue_expr_(use_blue_expr),
        use_alpha_expr_(use_alpha_expr),
        red_expr_(red_expr),
        green_expr_(green_expr),
        blue_expr_(blue_expr),
        alpha_expr_(alpha_expr)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle YCC", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      ColorChannels channels;
      channels.r = input[i].x;
      channels.g = input[i].y;
      channels.b = input[i].z;
      channels.a = input[i].w;
      rgb_to_hsv(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.v);
      rgb_to_hsl(channels.r, channels.g, channels.b, &channels.h, &channels.s, &channels.l);
      rgb_to_yuv(channels.r, channels.g, channels.b, &channels.y, &channels.yuv_u, &channels.yuv_v, BLI_YUV_ITU_BT709);
      float ycc_y, ycc_cb, ycc_cr;
      rgb_to_ycc(channels.r, channels.g, channels.b, &ycc_y, &ycc_cb, &ycc_cr, BLI_YCC_ITU_BT709);
      channels.cb = ycc_cb / 255.0f;
      channels.cr = ycc_cr / 255.0f;
      channels.z = 0.0f;
      channels.forward_u = 0.0f;
      channels.forward_v = 0.0f;
      channels.backward_u = 0.0f;
      channels.backward_v = 0.0f;

      float y = use_red_expr_ ? evaluate_expression(red_expr_, channels) :
                                get_channel_value(input[i], red_);
      float cb = use_green_expr_ ? evaluate_expression(green_expr_, channels) :
                                   get_channel_value(input[i], green_);
      float cr = use_blue_expr_ ? evaluate_expression(blue_expr_, channels) :
                                  get_channel_value(input[i], blue_);
      float a = use_alpha_expr_ ? evaluate_expression(alpha_expr_, channels) :
                                  get_channel_value(input[i], alpha_);

      y *= 255.0f;
      cb *= 255.0f;
      cr *= 255.0f;
      ycc_to_rgb(y, cb, cr, &output[i].x, &output[i].y, &output[i].z, ycc_mode_);
      output[i].w = a;
    });
  }
};

class ShuffleMotionFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;

 public:
  ShuffleMotionFunction(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
      : red_(red), green_(green), blue_(blue), alpha_(alpha)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle Motion", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      output[i] = float4(get_channel_value(input[i], red_),
                         get_channel_value(input[i], green_),
                         get_channel_value(input[i], blue_),
                         get_channel_value(input[i], alpha_));
    });
  }
};

class ShuffleDepthFunction : public mf::MultiFunction {
 private:
  uint8_t red_, green_, blue_, alpha_;

 public:
  ShuffleDepthFunction(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
      : red_(red), green_(green), blue_(blue), alpha_(alpha)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Shuffle Depth", signature};
      builder.single_input<float4>("Image");
      builder.single_output<float4>("Image");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4> &input = params.readonly_single_input<float4>(0, "Image");
    MutableSpan<float4> output = params.uninitialized_single_output<float4>(1, "Image");

    mask.foreach_index([&](const int64_t i) {
      output[i] = float4(get_channel_value(input[i], red_),
                         get_channel_value(input[i], green_),
                         get_channel_value(input[i], blue_),
                         get_channel_value(input[i], alpha_));
    });
  }
};

static void node_build_multi_function(blender::nodes::NodeMultiFunctionBuilder &builder)
{
  const NodeCMPShuffle &storage = node_storage(builder.node());

  switch (storage.mode) {
    case CMP_NODE_COMBSEP_COLOR_RGB:
      builder.construct_and_set_matching_fn<ShuffleRGBFunction>(storage.red,
                                                                 storage.green,
                                                                 storage.blue,
                                                                 storage.alpha,
                                                                 storage.use_red_expr,
                                                                 storage.use_green_expr,
                                                                 storage.use_blue_expr,
                                                                 storage.use_alpha_expr,
                                                                 storage.red_expr,
                                                                 storage.green_expr,
                                                                 storage.blue_expr,
                                                                 storage.alpha_expr);
      break;
    case CMP_NODE_COMBSEP_COLOR_HSV:
      builder.construct_and_set_matching_fn<ShuffleHSVFunction>(storage.red,
                                                                 storage.green,
                                                                 storage.blue,
                                                                 storage.alpha,
                                                                 storage.use_red_expr,
                                                                 storage.use_green_expr,
                                                                 storage.use_blue_expr,
                                                                 storage.use_alpha_expr,
                                                                 storage.red_expr,
                                                                 storage.green_expr,
                                                                 storage.blue_expr,
                                                                 storage.alpha_expr);
      break;
    case CMP_NODE_COMBSEP_COLOR_HSL:
      builder.construct_and_set_matching_fn<ShuffleHSLFunction>(storage.red,
                                                                 storage.green,
                                                                 storage.blue,
                                                                 storage.alpha,
                                                                 storage.use_red_expr,
                                                                 storage.use_green_expr,
                                                                 storage.use_blue_expr,
                                                                 storage.use_alpha_expr,
                                                                 storage.red_expr,
                                                                 storage.green_expr,
                                                                 storage.blue_expr,
                                                                 storage.alpha_expr);
      break;
    case CMP_NODE_COMBSEP_COLOR_YUV:
      builder.construct_and_set_matching_fn<ShuffleYUVFunction>(storage.red,
                                                                 storage.green,
                                                                 storage.blue,
                                                                 storage.alpha,
                                                                 storage.use_red_expr,
                                                                 storage.use_green_expr,
                                                                 storage.use_blue_expr,
                                                                 storage.use_alpha_expr,
                                                                 storage.red_expr,
                                                                 storage.green_expr,
                                                                 storage.blue_expr,
                                                                 storage.alpha_expr);
      break;
    case CMP_NODE_COMBSEP_COLOR_YCC:
      builder.construct_and_set_matching_fn<ShuffleYCCFunction>(storage.red,
                                                                 storage.green,
                                                                 storage.blue,
                                                                 storage.alpha,
                                                                 storage.ycc_mode,
                                                                 storage.use_red_expr,
                                                                 storage.use_green_expr,
                                                                 storage.use_blue_expr,
                                                                 storage.use_alpha_expr,
                                                                 storage.red_expr,
                                                                 storage.green_expr,
                                                                 storage.blue_expr,
                                                                 storage.alpha_expr);
      break;
  }
}

}  // namespace blender::nodes::node_composite_shuffle_cc

static void register_node_type_cmp_shuffle()
{
  namespace file_ns = blender::nodes::node_composite_shuffle_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeShuffle", CMP_NODE_SHUFFLE);
  ntype.ui_name = "Shuffle";
  ntype.ui_description = "Shuffle and remap RGBA channels";
  ntype.enum_name_legacy = "SHUFFLE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::cmp_node_shuffle_declare;
  ntype.initfunc = file_ns::node_cmp_shuffle_init;
  ntype.draw_buttons = file_ns::node_draw_buttons;
  blender::bke::node_type_storage(
      ntype, "NodeCMPShuffle", file_ns::node_storage_free, file_ns::node_storage_copy);
  ntype.gpu_fn = file_ns::node_gpu_material;
  ntype.build_multi_function = file_ns::node_build_multi_function;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_shuffle)