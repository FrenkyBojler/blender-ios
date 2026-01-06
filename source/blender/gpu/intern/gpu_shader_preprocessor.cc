/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "shader_tool/intermediate.hh"

#include "gpu_shader_private.hh"

namespace blender::gpu {

std::string Shader::run_preprocessor(StringRef source)
{
  using namespace shader::parser;
  report_callback report = [](int, int, std::string, const char *) {};
  IntermediateForm parser(source, report, ParserStage::MergeTokens);

  parser.print_stats();

  return parser.result_get();
}

}  // namespace blender::gpu
