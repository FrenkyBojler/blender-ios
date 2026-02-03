/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Linear Blend Skinning.
 */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_skinning_linear)

/**
 * Maps a normalized position along the bone (0.0 to 1.0) to a specific
 * segment index and a blend factor for interpolation between segments.
 */
void bbone_deform_clamp_segment_index(float head_tail,
                                      int segments,
                                      out int r_index,
                                      out float r_blend)
{
  float pre_blend = clamp(head_tail, 0.0f, 1.0f) * float(segments);

  /* Determine the base segment index */
  int index = int(floor(pre_blend));
  index = clamp(index, 0, segments - 1);

  /* calculate blend factor (fractional part) for the next segment */
  r_index = index;
  r_blend = pre_blend - float(index);
}

/**
 * Applies the transform of a specific B-Bone segment.
 */
void accumulate_bbone(
    int offset, float3 P_armspace, float4 T_rest, float weight, int seg_index, inout float3 P_accum, inout float3 T_accum)
{
  float4x4 pose_mat = bonemat_buf[offset + seg_index];
  float3x3 pose_mat3 = float3x3(pose_mat);
  float3x3 pose_mat3_inv_transpose = transpose(inverse(pose_mat3));

  float3 P_transformed = (pose_mat * float4(P_armspace, 1.0f)).xyz;

  float3 T_eval = T_rest.xyz * pose_mat3_inv_transpose;
  T_accum += T_eval * weight;

  /* Accumulate the difference from the rest position */
  P_accum += weight * (P_transformed - P_armspace);
}

/**
 * Calculates deformation for Bendy Bones
 * this finds where the vertex lies along the bone's length to
 * interpolate between the correct segments.
 */
void b_bone_deform(int offset,
                   float4x4 inv_arm_mat,
                   float bone_length,
                   int segments,
                   float3 P_armspace,
                   float4 T_rest,
                   float weight,
                   inout float3 P_accum,
                   inout float3 T_accum)
{
  /* Handle degenerate bones */
  if (bone_length < 0.0001f) {
    accumulate_bbone(offset, P_armspace, T_rest, weight, 0, P_accum, T_accum);
    return;
  }

  /* We only need the Y-coordinate (length axis) in local space to find position along the bone */
  float y = inv_arm_mat[0][1] * P_armspace.x + inv_arm_mat[1][1] * P_armspace.y + inv_arm_mat[2][1] * P_armspace.z +
            inv_arm_mat[3][1];

  float head_tail = y / bone_length;

  int index;
  float blend;
  bbone_deform_clamp_segment_index(head_tail, segments, index, blend);

  float weight_a = weight * (1.0f - blend);
  float weight_b = weight * blend;

  accumulate_bbone(offset, P_armspace, T_rest, weight_a, index, P_accum, T_accum);
  accumulate_bbone(offset, P_armspace, T_rest, weight_b, index + 1, P_accum, T_accum);
}

/* regular skinning */
void accumulate_simple(
    int offset, float3 P_armspace, float4 T_rest, float weight, inout float3 P_accum, inout float3 T_accum)
{
  float4x4 pose_mat = bonemat_buf[offset];
  float3x3 pose_mat3 = float3x3(pose_mat);
  float3x3 pose_mat3_inv_transpose = transpose(inverse(pose_mat3));

  float3 P_transformed = (pose_mat * float4(P_armspace, 1.0f)).xyz;

  float3 T_eval = T_rest.xyz * pose_mat3_inv_transpose;
  T_accum += T_eval * weight;

  P_accum += weight * (P_transformed - P_armspace);
}

void main()
{
  uint gid = gl_GlobalInvocationID.x;
  if (gid >= uint(vertex_count)) {
    return;
  }

  ArmatureSpace armspace = armspace_buf;

  /* Rest Position */
  float3 P_rest = pos_buf[gid].xyz;
  float4 T_rest = tan_buf[gid];

  /* Transform to Armature Space */
  float3 P_armspace = (armspace.TargetToSpace * float4(P_rest, 1.0f)).xyz;

  float3 P_accum = float3(0.0f);
  float3 T_accum = float3(0.0f);
  float total_weight = 0.0f;

  uint base_idx = gid * uint(influence_count);

  for (int k = 0; k < influence_count; ++k) {
    uint idx = base_idx + uint(k);
    uint bi = indices_buf[idx];
    float weight = weights_buf[idx];

    /* Skip invalid bones or zero weights */
    if (bi == 0xFFFFFFFFu || weight <= 0.0f) {
      continue;
    }

    int bone_idx = int(bi);
    BoneData bonedata = Bonedata_buf[bone_idx];
    int offset = int(bonedata.offsets);
    int segments = int(bonedata.segments);

    if (segments > 1) {
      b_bone_deform(offset,
                    bonedata.inverse_arm,
                    bonedata.lengths,
                    segments,
                    P_armspace,
                    T_rest,
                    weight,
                    P_accum,
                    T_accum);
    }
    else {
      accumulate_simple(offset, P_armspace, T_rest, weight, P_accum, T_accum);
    }

    total_weight += weight;
  }

  float3 P_arm_final = P_armspace + P_accum * (1.0f / max(total_weight, 1e-4f));
  float4 P_final = armspace.ArmatureToSpace * float4(P_arm_final, 1.0f);
  float3 T_final = normalize(T_rest.xyz + T_accum);

  out_skinned_pos[gid] = float4(P_final.xyz, 1.0f);
  out_skinned_tan[gid] = float4(T_final, T_rest);
}
