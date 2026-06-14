/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_matrix_construct_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* Jacobi rotations are 2x2 matrices, with the form [[c, s], [-s, c]]
 * where c = cos(theta), s = sin(theta), theta = angle of rotation.
 * Hold only the cos and sin values for easier storage. */
struct JacobiRotation {
  float c;
  float s;
};

/* Apply left Jacobi rotation on 2x2 matrix : M = J * M. */
float2x2 jacobi_rotate_left_2x2(float2x2 M, JacobiRotation j)
{
  float2 row_0 = float2(M[0][0], M[1][0]);
  float2 row_1 = float2(M[0][1], M[1][1]);
  float2 new_row_0 = j.c * row_0 + j.s * row_1;
  float2 new_row_1 = -j.s * row_0 + j.c * row_1;
  M[0][0] = new_row_0[0];
  M[1][0] = new_row_0[1];
  M[0][1] = new_row_1[0];
  M[1][1] = new_row_1[1];
  return M;
}

/* Apply left Jacobi rotation on 3x3 matrix : M = J * M.
 * Rotates rows p and q (0-based, matching Eigen). */
float3x3 jacobi_rotate_left_3x3(float3x3 M, int p, int q, JacobiRotation j)
{
  float3 row_p = float3(M[0][p], M[1][p], M[2][p]);
  float3 row_q = float3(M[0][q], M[1][q], M[2][q]);
  float3 new_row_p = j.c * row_p + j.s * row_q;
  float3 new_row_q = -j.s * row_p + j.c * row_q;
  M[0][p] = new_row_p[0];
  M[1][p] = new_row_p[1];
  M[2][p] = new_row_p[2];
  M[0][q] = new_row_q[0];
  M[1][q] = new_row_q[1];
  M[2][q] = new_row_q[2];
  return M;
}

/* Apply right Jacobi rotation on 3x3 matrix : M = M * J.
 * Rotates columns p and q (0-based, matching Eigen). */
float3x3 jacobi_rotate_right_3x3(float3x3 M, int p, int q, JacobiRotation j)
{
  float3 col_p = M[p];
  float3 col_q = M[q];
  M[p] = j.c * col_p - j.s * col_q;
  M[q] = j.s * col_p + j.c * col_q;
  return M;
}

JacobiRotation jacobi_transpose(JacobiRotation j)
{
  JacobiRotation jt;
  jt.c = j.c;
  jt.s = -j.s;
  return jt;
}

JacobiRotation jacobi_multiply(JacobiRotation a, JacobiRotation b)
{
  JacobiRotation j;
  j.c = a.c * b.c - a.s * b.s;
  j.s = a.c * b.s + a.s * b.c;
  return j;
}

float max_element_from_3x3(float3x3 M)
{
  return max(
      max(max(abs(M[0][0]), abs(M[0][1])), max(abs(M[0][2]), abs(M[1][0]))),
      max(max(abs(M[1][1]), abs(M[1][2])), max(abs(M[2][0]), max(abs(M[2][1]), abs(M[2][2])))));
}

void jacobi_svd_3x3(float3x3 A, out float3x3 U, out float3 S, out float3x3 V)
{
  const float precision = 2.0f * FLT_EPSILON;
  const float consider_as_zero = FLT_MIN;

  /* Normalize matrix for numerical stability. */
  float scale = max_element_from_3x3(A);
  if (scale == 0.0f) {
    scale = 1.0f;
  }

  A = A / scale;
  U = mat3x3_identity();
  V = mat3x3_identity();

  float max_diag_entry = max(max(abs(A[0][0]), abs(A[1][1])), abs(A[2][2]));

  /* The main Jacobi SVD iteration. Sweep until all off-diagonal entries are below threshold. */
  bool finished = false;
  while (!finished) {
    finished = true;

    for (int p = 1; p < 3; p++) {
      for (int q = 0; q < p; q++) {

        /* Skip pairs already converged. */
        float threshold = max(consider_as_zero, precision * max_diag_entry);
        if (abs(A[q][p]) > threshold || abs(A[p][q]) > threshold) {
          finished = false;

          /* SVD of the 2x2 block at indices (p, q). */
          JacobiRotation j_left;
          JacobiRotation j_right;
          jacobi_svd_2x2(A, p, q, j_left, j_right);

          /* Accumulate rotations. */
          A = jacobi_rotate_left_3x3(A, p, q, j_left);
          U = jacobi_rotate_right_3x3(U, p, q, jacobi_transpose(j_left));

          A = jacobi_rotate_right_3x3(A, p, q, j_right);
          V = jacobi_rotate_right_3x3(V, p, q, j_right);

          max_diag_entry = max(max_diag_entry, max(abs(A[p][p]), abs(A[q][q])));
        }
      }
    }
  }

  /* A is now diagonal. Extract singular values, force S >= 0, update sign into U. */
  S = float3(0.0f);
  for (int i = 0; i < 3; i++) {
    float a = A[i][i];
    S[i] = abs(a);
    if (a < 0.0f) {
      U[i] = -U[i];
    }
  }

  /* Revert to original scale. */
  S = S * scale;

  /* Sort singular values descending & update U,V columns accordingly. */
  for (int i = 0; i < 3; i++) {
    float max_val = 0.0f;
    int pos = i;
    for (int j = i; j < 3; j++) {
      float val = S[j];
      if (val > max_val) {
        max_val = val;
        pos = j;
      }
    }
    if (max_val == 0.0f) {
      break;
    }
    if (pos != i) {
      float tmp_s = S[i];
      S[i] = S[pos];
      S[pos] = tmp_s;

      float3 tmp_u = U[i];
      U[i] = U[pos];
      U[pos] = tmp_u;

      float3 tmp_v = V[i];
      V[i] = V[pos];
      V[pos] = tmp_v;
    }
  }
}

[[node]]
void matrix_svd(float4x4 matrix, out float4x4 U, out float3 S, out float4x4 V)
{
  float3x3 A = to_float3x3(matrix);
  float3x3 U3;
  float3x3 V3;
  jacobi_svd_3x3(A, U3, S, V3);
  U = to_float4x4(U3);
  V = to_float4x4(V3);
}
