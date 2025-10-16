/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "RNA_types.hh"

#include "IMB_colormanagement.hh"

#include "COM_algorithm_parallel_reduction.hh"
#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_levels_cc {

enum class DataType : uint8_t {
  Float = 0,
  Color = 1,
};

static const EnumPropertyItem data_type_items[] = {
    {int(DataType::Float), "FLOAT", 0, N_("Float"), N_("The input is a float")},
    {int(DataType::Color), "COLOR", 0, N_("Color"), N_("The input is a color")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Image", "Float Image")
      .default_value(0.0f)
      .structure_type(StructureType::Dynamic)
      .usage_by_single_menu(int(DataType::Float))
      .hide_value();
  b.add_input<decl::Color>("Image", "Color Image")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .structure_type(StructureType::Dynamic)
      .usage_by_single_menu(int(DataType::Color))
      .hide_value();
  b.add_input<decl::Menu>("Data Type")
      .default_value(DataType::Float)
      .static_items(data_type_items)
      .optional_label();

  b.add_output<decl::Float>("Minimum", "Float Minimum").usage_by_single_menu(int(DataType::Float));
  b.add_output<decl::Float>("Maximum", "Float Maximum").usage_by_single_menu(int(DataType::Float));
  b.add_output<decl::Float>("Sum", "Float Sum").usage_by_single_menu(int(DataType::Float));
  b.add_output<decl::Float>("Mean", "Float Mean").usage_by_single_menu(int(DataType::Float));
  b.add_output<decl::Float>("Standard Deviation", "Float Standard Deviation")
      .usage_by_single_menu(int(DataType::Float));

  b.add_output<decl::Color>("Minimum", "Color Minimum").usage_by_single_menu(int(DataType::Color));
  b.add_output<decl::Color>("Maximum", "Color Maximum").usage_by_single_menu(int(DataType::Color));
  b.add_output<decl::Color>("Sum", "Color Sum").usage_by_single_menu(int(DataType::Color));
  b.add_output<decl::Color>("Mean", "Color Mean").usage_by_single_menu(int(DataType::Color));
  b.add_output<decl::Color>("Standard Deviation", "Color Standard Deviation")
      .usage_by_single_menu(int(DataType::Color));
}

using namespace blender::compositor;

class LevelsOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    switch (this->get_data_type()) {
      case DataType::Float:
        this->execute_float();
        break;
      case DataType::Color:
        this->execute_color();
        break;
    }
  }

  void execute_float()
  {
    const Result &input = this->get_input("Float Image");
    if (input.is_single_value()) {
      this->execute_single_value_float();
      return;
    }

    Result &minimum_output = this->get_result("Float Minimum");
    if (minimum_output.should_compute()) {
      const float minimum = minimum_float(this->context(), input);
      minimum_output.allocate_single_value();
      minimum_output.set_single_value(minimum);
    }

    Result &maximum_output = this->get_result("Float Maximum");
    if (maximum_output.should_compute()) {
      const float maximum = maximum_float(this->context(), input);
      maximum_output.allocate_single_value();
      maximum_output.set_single_value(maximum);
    }

    Result &sum_output = this->get_result("Float Sum");
    Result &mean_output = this->get_result("Float Mean");
    Result &standard_deviation_output = this->get_result("Float Standard Deviation");
    if (sum_output.should_compute() || mean_output.should_compute() ||
        standard_deviation_output.should_compute())
    {
      const float sum = sum_float(this->context(), input);
      if (sum_output.should_compute()) {
        sum_output.allocate_single_value();
        sum_output.set_single_value(sum);
      }

      if (mean_output.should_compute()) {
        const float mean = sum / math::reduce_mul(input.domain().size);
        mean_output.allocate_single_value();
        mean_output.set_single_value(mean);
      }

      if (standard_deviation_output.should_compute()) {
        const float mean = sum / math::reduce_mul(input.domain().size);
        const float sum_of_squared_difference_to_mean = sum_squared_difference_float(
            this->context(), input, mean);
        const float mean_squared_difference_to_mean = sum_of_squared_difference_to_mean /
                                                      math::reduce_mul(input.domain().size);
        const float standard_deviation = math::sqrt(mean_squared_difference_to_mean);
        standard_deviation_output.allocate_single_value();
        standard_deviation_output.set_single_value(standard_deviation);
      }
    }
  }

  void execute_single_value_float()
  {
    const Result &input = this->get_input("Float Image");
    Result &minimum_output = this->get_result("Float Minimum");
    Result &maximum_output = this->get_result("Float Maximum");
    Result &sum_output = this->get_result("Float Sum");
    Result &mean_output = this->get_result("Float Mean");
    Result &standard_deviation_output = this->get_result("Float Standard Deviation");

    if (minimum_output.should_compute()) {
      minimum_output.share_data(input);
    }

    if (maximum_output.should_compute()) {
      maximum_output.share_data(input);
    }

    if (sum_output.should_compute()) {
      sum_output.share_data(input);
    }

    if (mean_output.should_compute()) {
      mean_output.share_data(input);
    }

    if (standard_deviation_output.should_compute()) {
      standard_deviation_output.allocate_single_value();
      standard_deviation_output.set_single_value(0.0f);
    }
  }

  void execute_color()
  {
    const Result &input = this->get_input("Color Image");
    if (input.is_single_value()) {
      this->execute_single_value_color();
      return;
    }

    Result &minimum_output = this->get_result("Color Minimum");
    if (minimum_output.should_compute()) {
      const float4 minimum = minimum_color(this->context(), input);
      minimum_output.allocate_single_value();
      minimum_output.set_single_value(minimum);
    }

    Result &maximum_output = this->get_result("Color Maximum");
    if (maximum_output.should_compute()) {
      const float4 maximum = maximum_color(this->context(), input);
      maximum_output.allocate_single_value();
      maximum_output.set_single_value(maximum);
    }

    Result &sum_output = this->get_result("Color Sum");
    Result &mean_output = this->get_result("Color Mean");
    Result &standard_deviation_output = this->get_result("Color Standard Deviation");
    if (sum_output.should_compute() || mean_output.should_compute() ||
        standard_deviation_output.should_compute())
    {
      const float4 sum = sum_color(this->context(), input);
      if (sum_output.should_compute()) {
        sum_output.allocate_single_value();
        sum_output.set_single_value(sum);
      }

      if (mean_output.should_compute()) {
        const float4 mean = sum / math::reduce_mul(input.domain().size);
        mean_output.allocate_single_value();
        mean_output.set_single_value(mean);
      }

      if (standard_deviation_output.should_compute()) {
        const float4 mean = sum / math::reduce_mul(input.domain().size);
        const float4 sum_of_squared_difference_to_mean = sum_squared_difference_color(
            this->context(), input, mean);
        const float4 mean_squared_difference_to_mean = sum_of_squared_difference_to_mean /
                                                       math::reduce_mul(input.domain().size);
        const float4 standard_deviation = math::sqrt(mean_squared_difference_to_mean);
        standard_deviation_output.allocate_single_value();
        standard_deviation_output.set_single_value(standard_deviation);
      }
    }
  }

  void execute_single_value_color()
  {
    const Result &input = this->get_input("Color Image");
    Result &minimum_output = this->get_result("Color Minimum");
    Result &maximum_output = this->get_result("Color Maximum");
    Result &sum_output = this->get_result("Color Sum");
    Result &mean_output = this->get_result("Color Mean");
    Result &standard_deviation_output = this->get_result("Color Standard Deviation");

    if (minimum_output.should_compute()) {
      minimum_output.share_data(input);
    }

    if (maximum_output.should_compute()) {
      maximum_output.share_data(input);
    }

    if (sum_output.should_compute()) {
      sum_output.share_data(input);
    }

    if (mean_output.should_compute()) {
      mean_output.share_data(input);
    }

    if (standard_deviation_output.should_compute()) {
      standard_deviation_output.allocate_single_value();
      standard_deviation_output.set_single_value(float4(0.0f));
    }
  }

  DataType get_data_type()
  {
    const Result &input = this->get_input("Data Type");
    const MenuValue default_menu_value = MenuValue(DataType::Float);
    const MenuValue menu_value = input.get_single_value_default(default_menu_value);
    return static_cast<DataType>(menu_value.value);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new LevelsOperation(context, node);
}

static void register_node()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeLevels", CMP_NODE_VIEW_LEVELS);
  ntype.ui_name = "Levels";
  ntype.ui_description = "Compute average and standard deviation of pixel values";
  ntype.enum_name_legacy = "LEVELS";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node)

}  // namespace blender::nodes::node_composite_levels_cc
