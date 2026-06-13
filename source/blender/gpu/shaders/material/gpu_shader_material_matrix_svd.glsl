/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
 * Rotates rows p and q. */
float3x3 jacobi_rotate_left_3x3(float3x3 M, int p, int q, JacobiRotation j)
{
  float3 row_p = float3(M[0][p - 1], M[1][p - 1], M[2][p - 1]);
  float3 row_q = float3(M[0][q - 1], M[1][q - 1], M[2][q - 1]);
  float3 new_row_p = j.c * row_p + j.s * row_q;
  float3 new_row_q = -j.s * row_p + j.c * row_q;
  M[0][p - 1] = new_row_p[0];
  M[1][p - 1] = new_row_p[1];
  M[2][p - 1] = new_row_p[2];
  M[0][q - 1] = new_row_q[0];
  M[1][q - 1] = new_row_q[1];
  M[2][q - 1] = new_row_q[2];
  return M;
}

/* Apply right Jacobi rotation on 3x3 matrix : M = M * J.
 * Rotates columns p and q. */
float3x3 jacobi_rotate_right_3x3(float3x3 M, int p, int q, JacobiRotation j)
{
  float3 col_p = M[p - 1];
  float3 col_q = M[q - 1];
  M[p - 1] = j.c * col_p - j.s * col_q;
  M[q - 1] = j.s * col_p + j.c * col_q;
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

[[node]]
void matrix_svd(float4x4 matrix, out float4x4 U, out float3 S, out float4x4 V)
{
}
