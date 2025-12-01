/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* SPDX-License-Identifier: Zlib
 * Copyright 2006-2007 University of Dublin, Trinity College, All Rights Reserved.

/**
 * Dual Quaternion Skinning Library
 *
 * GPU implementation of dual quaternion skinning algorithm based on BLI_math_quaternion.hh
 */

#pragma once

/**
 * Structure matching blender's DQ format.
 */
struct DualQuat {
  float4 quat;
  float4 trans;
  float4x4 scale;
  float scale_weight;
  float quat_weight;
};

float4 quat_conjugate(float4 q)
{
  return float4(-q.xyz, q.w);
}

float4 quat_multiply(float4 q1, float4 q2)
{
  return float4(q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
                q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
                q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
                q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z);
}

void accumulate_dual_quat_raw(inout DualQuat sum, in DualQuat bone_dq, float weight)
{
  bool flipped = false;

  if (length(sum.quat) > 0.0 && dot(bone_dq.quat, sum.quat) < 0.0) {
    flipped = true;
    weight = -weight;
  }

  sum.quat.w += weight * bone_dq.quat.w;
  sum.quat.x += weight * bone_dq.quat.x;
  sum.quat.y += weight * bone_dq.quat.y;
  sum.quat.z += weight * bone_dq.quat.z;

  sum.trans.w += weight * bone_dq.trans.w;
  sum.trans.x += weight * bone_dq.trans.x;
  sum.trans.y += weight * bone_dq.trans.y;
  sum.trans.z += weight * bone_dq.trans.z;

  if (bone_dq.scale_weight > 0.0) {
    if (flipped) {
      /* Restore positive weight for scale interpolation. */
      weight = -weight;
    }

    float4x4 wmat = bone_dq.scale * weight;
    sum.scale += wmat;
    sum.scale_weight += weight;
  }
}

void accumulate_dual_quat_pivot(inout DualQuat sum,
                                in DualQuat bone_dq,
                                float3 pivot,
                                float weight)
{
  if (bone_dq.scale_weight > 0.0) {
    DualQuat mdq = bone_dq;

    /* Compute scale-induced translation at the pivot point. */
    float3 dst = (mdq.scale * float4(pivot, 1.0)).xyz;
    dst = dst - pivot;

    mdq.trans.w -= 0.5 * (mdq.quat.x * dst.x + mdq.quat.y * dst.y + mdq.quat.z * dst.z);
    mdq.trans.x += 0.5 * (mdq.quat.w * dst.x + mdq.quat.y * dst.z - mdq.quat.z * dst.y);
    mdq.trans.y += 0.5 * (mdq.quat.w * dst.y + mdq.quat.z * dst.x - mdq.quat.x * dst.z);
    mdq.trans.z += 0.5 * (mdq.quat.w * dst.z + mdq.quat.x * dst.y - mdq.quat.y * dst.x);

    mdq.scale[3][0] -= dst.x;
    mdq.scale[3][1] -= dst.y;
    mdq.scale[3][2] -= dst.z;

    accumulate_dual_quat_raw(sum, mdq, weight);
  }
  else {
    accumulate_dual_quat_raw(sum, bone_dq, weight);
  }
}

void accumulate_dual_quat(inout DualQuat sum, in DualQuat bone_dq, float weight)
{
  float3 pivot = float3(0.0);
  accumulate_dual_quat_pivot(sum, bone_dq, pivot, weight);
}

DualQuat normalize_dual_quat(DualQuat dq)
{
  if (dq.quat_weight == 0.0) {
    DualQuat identity;
    identity.quat = float4(0.0, 0.0, 0.0, 1.0);
    identity.trans = float4(0.0, 0.0, 0.0, 0.0);
    identity.scale = float4x4(1.0);
    identity.scale_weight = 0.0;
    identity.quat_weight = 1.0;
    return identity;
  }

  float scale = 1.0 / dq.quat_weight;

  DualQuat result = dq;
  result.quat *= scale;
  result.trans *= scale;

  if (result.scale_weight > 0.0) {
    /* Compensate for dual quaternions added without scale matrices. */
    float addweight = dq.quat_weight - result.scale_weight;
    if (addweight > 0.0) {
      result.scale[0][0] += addweight;
      result.scale[1][1] += addweight;
      result.scale[2][2] += addweight;
      result.scale[3][3] += addweight;
    }
    result.scale *= scale;
    result.scale_weight = 1.0;
  }
  result.quat_weight = 1.0;

  return result;
}

float3 transform_point_dual_quat(float3 point, DualQuat dq)
{
  float w = dq.quat.w, x = dq.quat.x, y = dq.quat.y, z = dq.quat.z;
  float t0 = dq.trans.w, t1 = dq.trans.x, t2 = dq.trans.y, t3 = dq.trans.z;

  float3x3 M;
  M[0][0] = w * w + x * x - y * y - z * z;
  M[1][0] = 2.0 * (x * y - w * z);
  M[2][0] = 2.0 * (x * z + w * y);

  M[0][1] = 2.0 * (x * y + w * z);
  M[1][1] = w * w + y * y - x * x - z * z;
  M[2][1] = 2.0 * (y * z - w * x);

  M[0][2] = 2.0 * (x * z - w * y);
  M[1][2] = 2.0 * (y * z + w * x);
  M[2][2] = w * w + z * z - x * x - y * y;

  float len2 = dot(dq.quat, dq.quat);
  if (len2 > 0.0) {
    len2 = 1.0 / len2;
  }

  /* Extract translation from dual quaternion. */
  float3 t;
  t[0] = 2.0 * (-t0 * x + w * t1 - t2 * z + y * t3);
  t[1] = 2.0 * (-t0 * y + t1 * z - x * t3 + w * t2);
  t[2] = 2.0 * (-t0 * z + x * t2 + w * t3 - t1 * y);

  float3 result = point;

  /* Apply scale transformation first. */
  if (dq.scale_weight != 0.0) {
    result = (dq.scale * float4(result, 1.0)).xyz;
  }

  /* Apply rotation and translation with quaternion normalization. */
  result = M * result;
  result[0] = (result[0] + t[0]) * len2;
  result[1] = (result[1] + t[1]) * len2;
  result[2] = (result[2] + t[2]) * len2;

  return result;
}

float3 transform_normal_dual_quat(float3 normal, DualQuat dq)
{
  float w = dq.quat.w, x = dq.quat.x, y = dq.quat.y, z = dq.quat.z;

  /* Build rotation matrix from quaternion */
  float3x3 M;
  M[0][0] = w * w + x * x - y * y - z * z;
  M[1][0] = 2.0 * (x * y - w * z);
  M[2][0] = 2.0 * (x * z + w * y);

  M[0][1] = 2.0 * (x * y + w * z);
  M[1][1] = w * w + y * y - x * x - z * z;
  M[2][1] = 2.0 * (y * z - w * x);

  M[0][2] = 2.0 * (x * z - w * y);
  M[1][2] = 2.0 * (y * z + w * x);
  M[2][2] = w * w + z * z - x * x - y * y;

  float len2 = dot(dq.quat, dq.quat);
  if (len2 > 0.0) {
    len2 = 1.0 / len2;
  }

  /* Apply scale first if present */
  float3x3 transform_matrix = M;
  if (dq.scale_weight != 0.0) {
    transform_matrix = M * float3x3(dq.scale);
  }

  float3x3 normal_matrix = transpose(inverse(transform_matrix));
  float3 result = normal_matrix * normal;

  result *= len2;

  return result;
}
