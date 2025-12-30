/* Precompiled header for Eigen-based code paths.
 * Keep includes minimal and stable to maximize reuse.
 */

#pragma once

/* Core Eigen types and dense/cholesky decompositions are the main
 * compile-time cost centers highlighted by Build Insights.
 */
#include "Eigen/Core"
#include "Eigen/Cholesky"
