
static uniform float safe_rcp(uniform const float a)
{
  return a ? rcp_fast(a) : 0.0f;
}

float safe_rcp(const float a)
{
  return a ? rcp_fast(a) : 0.0f;
}

static uniform float square(const uniform float value)
{
    return value * value;
}

template<int Size>
uniform float<Size> square(const uniform float<Size> value)
{
    return value * value;
}

varying float square(const varying float value)
{
    return value * value;
}

template<int Size>
varying float<Size> square(const varying float<Size> value)
{
    return value * value;
}

export void distance_to_n_squared(const uniform float src_a_x[],
                                  const uniform float src_a_y[],
                                  const uniform float src_a_z[],
                                  const uniform float src_b_xyz[3],
                                  const uniform int count,
                                  float uniform dst[])
{
  const uniform float<3> b = {src_b_xyz[0], src_b_xyz[1], src_b_xyz[2]};
  const uniform int batch_count = (count / programCount) * programCount;
  for (uniform int index = 0; index < batch_count; index += programCount) {
    const varying float<3> a = {streaming_load(&src_a_x[index]),
                                streaming_load(&src_a_y[index]),
                                streaming_load(&src_a_z[index])};
    const varying float<3> batch = square<3>(a - b);
    dst[index + programIndex] = batch.x + batch.y + batch.z;
  }

  varying float<3> a;
  
  memcpy(&a.x, (uniform float * uniform)&src_a_x[batch_count], sizeof(uniform float) * (count - batch_count));
  memcpy(&a.y, (uniform float * uniform)&src_a_y[batch_count], sizeof(uniform float) * (count - batch_count));
  memcpy(&a.z, (uniform float * uniform)&src_a_z[batch_count], sizeof(uniform float) * (count - batch_count));
  
  const varying float<3> batch = square<3>(a - b);
  uniform float sufix_result[programCount];
  sufix_result[programIndex] = batch.x + batch.y + batch.z;
  memcpy((uniform float * uniform)&dst[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
}

enum FMMConstants {
    ChunkSize = programCount,
};

export uniform FMMConstants fmm_constants()
{
  return ChunkSize;
}

export void chunked_squared_distances_table(const uniform float row_x[][programCount],
                                            const uniform float row_y[][programCount],
                                            const uniform float row_z[][programCount],
                                            const uniform int row_count,
                                            const uniform float col_x[],
                                            const uniform float col_y[],
                                            const uniform float col_z[],
                                            const uniform int col_count,
                                            float uniform distances[][programCount])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    const varying float<3> row_xyz = {streaming_load(&row_x[row_index][0]),
                                      streaming_load(&row_y[row_index][0]),
                                      streaming_load(&row_z[row_index][0])};
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      uniform float<3> col_xyz = {streaming_load_uniform(&col_x[col_index]),
                                  streaming_load_uniform(&col_y[col_index]),
                                  streaming_load_uniform(&col_z[col_index])};
      const varying float<3> batch = square<3>(row_xyz - col_xyz);
      distances[row_index * col_count + col_index][programIndex] = batch.x + batch.y + batch.z;
    }
  }
}

export void squared_distances_table(const uniform float row_x[],
                                    const uniform float row_y[],
                                    const uniform float row_z[],
                                    const uniform int row_count,
                                    const uniform float col_x[],
                                    const uniform float col_y[],
                                    const uniform float col_z[],
                                    const uniform int col_count,
                                    float uniform distances[])
{
  assert(row_count < programCount);
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    const uniform float<3> row_xyz = {row_x[row_index], row_y[row_index], row_z[row_index]};

    const uniform int batch_col_count = (col_count / programCount) * programCount;
    for (uniform int col_index = 0; col_index < batch_col_count; col_index += programCount) {
      const varying float<3> col_xyz = {streaming_load(&col_x[col_index]),
                                        streaming_load(&col_y[col_index]),
                                        streaming_load(&col_z[col_index])};
      const varying float<3> batch = square<3>(row_xyz - col_xyz);
      distances[row_index * col_count + col_index + programIndex] = batch.x + batch.y + batch.z;
    }
    for (uniform int col_index = batch_col_count; col_index < col_count; col_index++) {
      const uniform float<3> col_xyz = {col_x[col_index], col_y[col_index], col_z[col_index]};
      const uniform float<3> batch = square<3>(row_xyz - col_xyz);
      distances[row_index * col_count + col_index] = batch.x + batch.y + batch.z;
    }
  }
}

export void chunked_zero_if_index_in_range(const uniform int row_indices[][programCount],
                                           const uniform int col_count,
                                           const uniform int from_bucket_to_sampler_offset,
                                           const uniform int row_count,
                                           uniform float rows_and_cols[][programCount])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    const varying int bucket_indices = row_indices[row_index][programIndex];
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      const varying bool mask = bucket_indices != col_index + from_bucket_to_sampler_offset;
      if (mask) {
        rows_and_cols[row_index * col_count + col_index][programIndex] = rows_and_cols[row_index * col_count + col_index][programIndex];
      } else {
        rows_and_cols[row_index * col_count + col_index][programIndex] = 0.0f;
      }
    }
  }
}

export void zero_if_index_in_range(const uniform int row_indices[],
                                   const uniform int col_count,
                                   const uniform int from_bucket_to_sampler_offset,
                                   const uniform int row_count,
                                   uniform float rows_and_cols[])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    const uniform int bucket_indices = row_indices[row_index];
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      const uniform bool mask = bucket_indices != col_index + from_bucket_to_sampler_offset;
      if (mask) {
        rows_and_cols[row_index * col_count + col_index] = rows_and_cols[row_index * col_count + col_index];
      } else {
        rows_and_cols[row_index * col_count + col_index] = 0.0f;
      }
    }
  }
}

export uniform int chunked_table_product_reduce(const uniform float rows_and_cols[][programCount],
                                                const uniform int col_count,
                                                const uniform float col_values[],
                                                const uniform int row_count,
                                                uniform float row_values[][programCount])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      row_values[row_index][programIndex] += streaming_load(&rows_and_cols[row_index * col_count + col_index][0]) * col_values[col_index];
    }
  }
}

export uniform int table_product_reduce(const uniform float rows_and_cols[],
                                        const uniform int col_count,
                                        const uniform float col_values[],
                                        const uniform int row_count,
                                        uniform float row_values[])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      row_values[row_index] += rows_and_cols[row_index * col_count + col_index] * col_values[col_index];
    }
  }
}

export void sqrt_n_add_single(float uniform values[], const uniform int count, const uniform float offset)
{
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        values[index + programIndex] = sqrt(streaming_load(&values[index])) + offset;
    }

    uniform float sufix[programCount];
    memcpy(&sufix[0], &values[batch_count], sizeof(uniform float) * (count - batch_count));
    
    sufix[programIndex] = sqrt(sufix[programIndex]) + offset;
    
    memcpy(&values[batch_count], sufix, sizeof(uniform float) * (count - batch_count));
}

export void gather_ints(
    uniform int src[],
    uniform int indices[],
    uniform int dst[],
    uniform int indices_start,
    uniform int indices_count)
{
  uniform int tr = 0;
  for (uniform int index = indices_start; index < indices_start + indices_count; index++) {
    assert(indices[index] >= 0);
    dst[tr] = src[indices[index]];
    tr++;
  }
  return;

    const uniform int start_round_up = ((indices_start + programCount - 1) / programCount) * programCount;
    const uniform int end_round_down = ((indices_start + indices_count) / programCount) * programCount;
    uniform int iter = 0;
    if (start_round_up >= end_round_down) {
      for (uniform int index = indices_start; index < indices_start + indices_count; index++) {
          dst[iter] = src[indices[index]];
          iter++;
      }
      return;
    }
    for (uniform int index = indices_start; index < start_round_up; index++) {
        dst[iter] = src[indices[index]];
        iter++;
    }
    for (uniform int index = start_round_up; index < end_round_down; index += programCount) {
        dst[iter + programIndex] = src[streaming_load(&indices[index])];
        iter += programCount;
    }
    for (uniform int index = end_round_down; index < indices_start + indices_count; index++) {
        dst[iter] = src[indices[index]];
        iter++;
    }
}

export void scatter_ints(
    uniform int src[],
    uniform int indices[],
    uniform int dst[],
    uniform int indices_start,
    uniform int indices_count)
{
  uniform int tr = 0;
  for (uniform int index = indices_start; index < indices_start + indices_count; index++) {
    dst[indices[index]] = src[tr];
    tr++;
  }
  return;
  
    const uniform int start_round_up = ((indices_start + programCount - 1) / programCount) * programCount;
    const uniform int end_round_down = ((indices_start + indices_count) / programCount) * programCount;
    uniform int iter = 0;
    if (start_round_up >= end_round_down) {
      for (uniform int index = indices_start; index < indices_start + indices_count; index++) {
          dst[indices[index]] = src[iter];
          iter++;
      }
      return;
    }
    for (uniform int index = indices_start; index < start_round_up; index++) {
        dst[indices[index]] = src[iter];
        iter++;
    }
    for (uniform int index = start_round_up; index < end_round_down; index += programCount) {
        dst[streaming_load(&indices[index])] = src[iter + programIndex];
        iter += programCount;
    }
    for (uniform int index = end_round_down; index < indices_start + indices_count; index++) {
        dst[indices[index]] = src[iter];
        iter++;
    }
}

export void parition_as_gather(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int mapping_total)
{
    assert(mapping_total % 2 == 0);
    const uniform int mapping_half = mapping_total / 2;

    gather_ints(values, indices, buffer, 0, mapping_total);

    scatter_ints(buffer, indices, values, mapping_half, mapping_half);
    scatter_ints(buffer + mapping_half, indices, values, 0, mapping_half);
}

export void parition_as_gather_front(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    const uniform int mapping_total,
    const uniform int count,
    const uniform int total_front_size)
{
    assert(mapping_total % 2 == 0);
    const uniform int mapping_half = mapping_total / 2;

    gather_ints(values, indices, buffer, 0, mapping_half);
    scatter_ints(buffer, indices, values, mapping_half, mapping_half);
}

export uniform int count_floats_less_than(
    const uniform float values[],
    uniform float min_predicate_value,
    uniform int count)
{
    const uniform int batch_count = (count / programCount) * programCount;

    varying int batch_total = 0;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        batch_total += (int)(streaming_load(&values[index]) < min_predicate_value);
    }
    uniform int rest_total = 0;
    for (uniform int index = batch_count; index < count; index++) {
        rest_total += (int)(values[index] < min_predicate_value);
    }

    return reduce_add(batch_total) + rest_total;
}

static uniform int n_float_less_then_or_equal_single(uniform int indices[],
                                            uniform float predicates[],
                                            const uniform float min_predicate_value,
                                            const uniform int predicates_start,
                                            const uniform int predicates_count)
{
    assert(predicates_count > 0);
    const uniform int start_round_up = ((predicates_start + programCount - 1) / programCount) * programCount;
    const uniform int end_round_down = ((predicates_start + predicates_count) / programCount) * programCount;
    assert(predicates_count >= programCount || start_round_up >= end_round_down);
    uniform int iter = 0;
    if (start_round_up >= end_round_down) {
      for (uniform int index = predicates_start; index < predicates_start + predicates_count; index++) {
          indices[iter] = index;
          iter += (int)(!(predicates[index] < min_predicate_value));
      }
      return iter;
    }
    for (uniform int index = predicates_start; index < start_round_up; index++) {
        indices[iter] = index;
        iter += (int)(!(predicates[index] < min_predicate_value));
    }
    for (uniform int index = start_round_up; index < end_round_down; index += programCount) {
        if (!(streaming_load(&predicates[index]) < min_predicate_value)) {
             iter += packed_store_active2(&indices[iter], index + programIndex);
        }
    }
    for (uniform int index = end_round_down; index < predicates_start + predicates_count; index++) {
        indices[iter] = index;
        iter += (int)(!(predicates[index] < min_predicate_value));
    }
    return iter;
}

static uniform int n_float_more_then_single(uniform int indices[],
                                                     uniform float predicates[],
                                                     const uniform float min_predicate_value,
                                                     const uniform int predicates_start,
                                                     const uniform int predicates_count)
{
    assert(predicates_count > 0);
    const uniform int start_round_up = ((predicates_start + programCount - 1) / programCount) * programCount;
    const uniform int end_round_down = ((predicates_start + predicates_count) / programCount) * programCount;
    assert(predicates_count >= programCount || start_round_up >= end_round_down);
    uniform int iter = 0;
    if (start_round_up >= end_round_down) {
      for (uniform int index = predicates_start; index < predicates_start + predicates_count; index++) {
          indices[iter] = index;
          iter += (int)((predicates[index] < min_predicate_value));
      }
      return iter;
    }
    for (uniform int index = predicates_start; index < start_round_up; index++) {
        indices[iter] = index;
        iter += (int)((predicates[index] < min_predicate_value));
    }
    for (uniform int index = start_round_up; index < end_round_down; index += programCount) {
        if ((streaming_load(&predicates[index]) < min_predicate_value)) {
             iter += packed_store_active2(&indices[iter], index + programIndex);
        }
    }
    for (uniform int index = end_round_down; index < predicates_start + predicates_count; index++) {
        indices[iter] = index;
        iter += (int)((predicates[index] < min_predicate_value));
    }
    return iter;
}

export uniform int predicate_partition_indices_float_cmp(
    uniform int indices[],
    const uniform float predicates[],
    const uniform int count,
    const uniform float min_predicate_value,
    const uniform int total_front_size)
{
    assert(total_front_size > 0);
    assert(total_front_size < count);
    
    // uniform int tr1 = 0;
    // for (uniform int index = 0; index < total_front_size; index++) {
    //   if (!(predicates[index] < min_predicate_value)) {
    //     indices[tr1] = index;
    //     tr1++;
    //   }
    // }
    // uniform int tr2 = 0;
    // for (uniform int index = total_front_size; index < count; index++) {
    //   if ((predicates[index] < min_predicate_value)) {
    //     indices[tr1 + tr2] = index;
    //     tr2++;
    //   }
    // }
    // assert(tr1 == tr2);
    // return tr1 + tr2;
    
    
    
    const uniform int front_to_move = n_float_less_then_or_equal_single(indices,
                                                                        predicates,
                                                                        min_predicate_value,
                                                                        0,
                                                                        total_front_size);
    const uniform int back_to_move = n_float_more_then_single(indices + front_to_move,
                                                              predicates,
                                                              min_predicate_value,
                                                              total_front_size,
                                                              count - total_front_size);
    // print("%, %: (%, %);\n", count, total_front_size, front_to_move, back_to_move); // 6, 4: (2, 8);
    assert(front_to_move == back_to_move);
    return front_to_move + back_to_move;
}

export uniform int mul_n_add_to(uniform float dst[],
                                const uniform float src[],
                                const uniform float value,
                                const uniform int count)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        dst[index + programIndex] = dst[index + programIndex] + value * streaming_load(&src[index]);
    }
    for (uniform int index = batch_count; index < count; index++) {
        dst[index] = dst[index] + value * src[index];
    }
    return buffer_index;
}

export void split_float3_to_3_float(
    const uniform float xyz_values[][3],
    uniform float x_components[],
    uniform float y_components[],
    uniform float z_components[],
    uniform int count)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&xyz_values[index + 100]);
        varying float<3> value;
        aos_to_soa3(&xyz_values[index][0], &value.x, &value.y, &value.z);
        streaming_store(&x_components[index], value.x);
        streaming_store(&y_components[index], value.y);
        streaming_store(&z_components[index], value.z);
    }
    for (uniform int index = batch_count; index < count; index++) {
        x_components[index] = xyz_values[index][0];
        y_components[index] = xyz_values[index][1];
        z_components[index] = xyz_values[index][2];
    }
}

export uniform float safe_0_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    values[i] = rsqrt_fast(values[i]);
  }
}

export uniform float safe_1_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    values[i] = safe_rcp(values[i]);
  }
}

export uniform float safe_1_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = r_value * sqrt(r_value);
  }
}

export uniform float safe_2_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(r_value);
  }
}

export uniform float safe_2_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(r_value) * sqrt(r_value);
  }
}

export uniform float safe_3_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(r_value) * r_value;
  }
}

export uniform float safe_3_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(r_value) * r_value * sqrt(r_value);
  }
}

export uniform float safe_4_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(r_value));
  }
}

export uniform float safe_4_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(r_value)) * sqrt(r_value);
  }
}

export uniform float safe_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(r_value)) * r_value;
  }
}

export uniform float safe_5_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(r_value)) * r_value * sqrt(r_value);
  }
}

export uniform float safe_6_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared;
  }
}

export uniform float safe_6_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * sqrt(r_value);
  }
}

export uniform float safe_7_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * r_value;
  }
}

export uniform float safe_7_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * r_value * sqrt(r_value);
  }
}

export uniform float safe_8_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(square(r_value)));
  }
}

export uniform float safe_8_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(square(r_value))) * sqrt(r_value);
  }
}

export uniform float safe_9_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(square(r_value))) * r_value;
  }
}

export uniform float safe_9_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    values[i] = square(square(square(r_value))) * r_value * sqrt(r_value);
  }
}

export uniform float safe_10_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(square(squared)) * squared;
  }
}

export uniform float safe_10_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = safe_rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(square(squared)) * squared * sqrt(r_value);
  }
}

export uniform float unsafe_0_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    values[i] = rsqrt_fast(values[i]);
  }
}

export uniform float unsafe_1_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    values[i] = rcp(values[i]);
  }
}

export uniform float unsafe_1_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = r_value * sqrt(r_value);
  }
}

export uniform float unsafe_2_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(r_value);
  }
}

export uniform float unsafe_2_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(r_value) * sqrt(r_value);
  }
}

export uniform float unsafe_3_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(r_value) * r_value;
  }
}

export uniform float unsafe_3_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(r_value) * r_value * sqrt(r_value);
  }
}

export uniform float unsafe_4_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(r_value));
  }
}

export uniform float unsafe_4_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(r_value)) * sqrt(r_value);
  }
}

export uniform float unsafe_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(r_value)) * r_value;
  }
}

export uniform float unsafe_5_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(r_value)) * r_value * sqrt(r_value);
  }
}

export uniform float unsafe_6_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared;
  }
}

export uniform float unsafe_6_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * sqrt(r_value);
  }
}

export uniform float unsafe_7_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * r_value;
  }
}

export uniform float unsafe_7_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(squared) * squared * r_value * sqrt(r_value);
  }
}

export uniform float unsafe_8_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(square(r_value)));
  }
}

export uniform float unsafe_8_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(square(r_value))) * sqrt(r_value);
  }
}

export uniform float unsafe_9_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(square(r_value))) * r_value;
  }
}

export uniform float unsafe_9_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    values[i] = square(square(square(r_value))) * r_value * sqrt(r_value);
  }
}

export uniform float unsafe_10_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(square(squared)) * squared;
  }
}

export uniform float unsafe_10_5_rpow_n(uniform float values[], const uniform int count)
{
  assume(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  assert(((uniform uint64)((void*)values) & (32 * TARGET_WIDTH)-1) == 0);
  foreach (i = 0 ... count) {
    const float r_value = rcp(values[i]);
    const float squared = square(r_value);
    values[i] = square(square(squared)) * squared * sqrt(r_value);
  }
}
