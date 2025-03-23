
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

float square(const float value)
{
    return value * value;
}

export uniform float float_dot_product(const uniform float values[],
                                       const uniform float factors[],
                                       const uniform int count)
{
  float accumulator = 0.0f;
  foreach (index = 0 ... count) {
    accumulator += values[index] * factors[index];
  }
  return reduce_add(accumulator);
}

export uniform float float_gather_dot_product(const uniform int indices[],
                                              const uniform float values[],
                                              const uniform float factors[],
                                              const uniform int count)
{
  float accumulator = 0.0f;
  foreach (index = 0 ... count) {
    accumulator += values[indices[index]] * factors[index];
  }
  return reduce_add(accumulator);
}

export void float3_dot_product(const uniform float values[][3],
                               const uniform float factors[],
                               const uniform int count,
                               float uniform result[3])
{
  float<3> accumulator = 0.0f;
  foreach (index = 0 ... count, axis = 0 ... 3) {
    accumulator[axis] += values[index][axis] * factors[index];
  }
  for (uniform int axis = 0; axis < 3; axis++) {
    result[axis] = reduce_add(accumulator[axis]);
  }
}

export void float3_gather_dot_product_(const uniform int indices[],
                                       const uniform float values[][3],
                                       const uniform float factors[],
                                       const uniform int count,
                                       float uniform result[3])
{
  float<3> accumulator = 0.0f;
  foreach (index = 0 ... count, axis = 0 ... 3) {
    accumulator[axis] += values[indices[index]][axis] * factors[index];
  }
  for (uniform int axis = 0; axis < 3; axis++) {
    result[axis] = reduce_add(accumulator[axis]);
  }
}

export void float3_gather_dot_product(const uniform int indices[],
                                      const uniform float values[][3],
                                      const uniform float factors[],
                                      const uniform int count,
                                      float uniform result[3])
{
  float<3> accumulator = {0.0f, 0.0f, 0.0f};
  foreach (index = 0 ... count, axis = 0 ... 3) {
    accumulator[axis] += values[indices[index]][axis] * factors[index];
  }
  for (uniform int axis = 0; axis < 3; axis++) {
    result[axis] = reduce_add(accumulator[axis]);
  }
}

// export void float_scatter_mul_add(const uniform float factors[],
//                                   const uniform float value,
//                                   const uniform int indices[],
//                                   uniform float data[],
//                                   const uniform int count)
// {
//   foreach (i = 0 ... count) {
//     data[indices[i]] += value * factors[i];
//     prefetchw_l1(&data[indices[min(i + 32, count - 1)]]);
//   }
// }

export void gather_distances(const uniform int indices[],
                             const uniform float positions[][3],
                             const uniform float target[3],
                             const uniform int count,
                             float uniform distances[],
                             const uniform float offset)
{
  for (uniform int index = 0; index < count; index++) {
    foreach (axis = 0 ... 3) {
      const float segment = positions[indices[index]][axis] - target[axis];
      distances[index] = sqrt(reduce_add(square(segment))) + offset;
    }
  }
}

// static void distances_exec(uniform float * uniform positions,
//                            const uniform float target[3],
//                            const uniform int count,
//                            uniform float * uniform distances,
//                            const uniform float offset)
// {
//     #pragma ignore warning(perf)
//     // const float targets = target[programIndex % 3];
//     const float targets = {target[0], target[1], target[2], 0.0f, target[0], target[1], target[2], 0.0f};
//     const uniform int triplet_size = programCount / 3;
// 
//     const uniform int chanked_count = (count / triplet_size) * triplet_size;
//     const uniform int chanked_limit = min(chanked_count, ((count * 3) - programCount) / 3) - triplet_size;
// 
//     for (uniform int index = 0; index < chanked_limit; index += triplet_size) {
//         float batch = positions[index * 3 + programIndex];
//         batch -= targets;
//         batch *= batch;
//         const float old = shift(batch, -2);
//         batch += shift(batch, -1) + old;
//         batch = sqrt(batch);
//         batch += offset;
//         #pragma unroll
//         for (uniform int item_index = 0; item_index < triplet_size; item_index++) {
//             distances[index + item_index] = extract(batch, item_index * 3);
//         }
//     }
//     const uniform float<3> target_ = {target[0], target[1], target[2]};
//     for (uniform int index = chanked_limit; index < count; index++) {
//         uniform float<3> batch = {positions[index * 3 + 0],
//                                   positions[index * 3 + 1],
//                                   positions[index * 3 + 2]};
//         batch -= target_;
//         batch *= batch;
//         batch[0] += batch[1] + batch[2];
//         batch[0] = sqrt(batch[0]);
//         batch[0] += offset;
//         distances[index] = batch[0];
//     }
// }
// 
// export void distances(uniform float positions[][3],
//                       const uniform float target[3],
//                       const uniform int count,
//                       float uniform distances[],
//                       const uniform float offset)
// {
//     distances_exec(&positions[0][0], target, count, &distances[0], offset);
// }

export void distances(const uniform float positions[][3],
                      const uniform float target[3],
                      const uniform int count,
                      float uniform distances[],
                      const uniform float offset)
{
    const uniform float<3> targets = {target[0], target[1], target[2]};
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        varying float<3> batch;
        aos_to_soa3(&positions[index][0], &batch[0], &batch[1], &batch[2]);
        batch -= targets;
        batch *= batch;
        distances[index + programIndex] = sqrt(batch.x + batch.y + batch.z) + offset;
    }
    for (uniform int index = batch_count; index < count; index++) {
        uniform float<3> batch = {positions[index][0], positions[index][1], positions[index][2]};
        batch -= targets;
        batch *= batch;
        distances[index] = sqrt(batch.x + batch.y + batch.z) + offset;
    }
}

export void distances_split(const uniform float positions_x[],
                            const uniform float positions_y[],
                            const uniform float positions_z[],
                            const uniform float target[3],
                            const uniform int count,
                            float uniform distances[],
                            const uniform float offset)
{
    const uniform float<3> targets = {target[0], target[1], target[2]};
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&positions_x[index + 100]);
        prefetch_nt(&positions_y[index + 100]);
        prefetch_nt(&positions_z[index + 100]);
        varying float<3> batch = {positions_x[index + programIndex],
                                  positions_y[index + programIndex],
                                  positions_z[index + programIndex]};
        batch -= targets;
        batch *= batch;
        distances[index + programIndex] = sqrt(batch.x + batch.y + batch.z) + offset;
    }
    for (uniform int index = batch_count; index < count; index++) {
        uniform float<3> batch = {positions_x[index], positions_y[index], positions_z[index]};
        batch -= targets;
        batch *= batch;
        distances[index] = sqrt(batch.x + batch.y + batch.z) + offset;
    }
}

export uniform int predicate_indices_float_cmp(
    uniform int indices[],
    uniform float predicates[],
    const uniform int count,
    const uniform float min_predicate_value)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float predicate = streaming_load(&predicates[index]);
        if (predicate < min_predicate_value) {
             buffer_index += packed_store_active(&indices[buffer_index], index + programIndex);
        }
    }
    for (uniform int index = batch_count; index < count; index++) {
        const uniform float predicate = predicates[index];
        if (predicate < min_predicate_value) {
            indices[buffer_index] = index;
            buffer_index++;
        }
    }
    const uniform int total_front = buffer_index;
    if (total_front == count) {
      return total_front;
    }
    if (total_front == 0) {
      return total_front;
    }
    
    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float predicate = streaming_load(&predicates[index]);
        if (!(predicate < min_predicate_value)) {
             buffer_index += packed_store_active(&indices[buffer_index], index + programIndex);
        }
    }
    for (uniform int index = batch_count; index < count; index++) {
        const uniform float predicate = predicates[index];
        if (!(predicate < min_predicate_value)) {
            indices[buffer_index] = index;
            buffer_index++;
        }
    }

    return total_front;
}

export uniform int partition_int_compare_float(
    uniform int values[],
    uniform int buffer[],
    const uniform float predicates[],
    const uniform int count,
    const uniform float min_predicate_value)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const varying int value = streaming_load(&values[index]);
        const varying float predicate = streaming_load(&predicates[index]);
        if (predicate < min_predicate_value) {
             buffer_index += packed_store_active(&buffer[buffer_index], value);
        }
    }
    for (uniform int index = batch_count; index < count; index++) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const uniform int value = values[index];
        const uniform float predicate = predicates[index];
        if (predicate < min_predicate_value) {
            buffer[buffer_index] = value;
            buffer_index++;
        }
    }
    const uniform int total_front = buffer_index;
    if (total_front == 0 || total_front == count) {
      return total_front;
    }
    
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const varying int value = streaming_load(&values[index]);
        const varying float predicate = streaming_load(&predicates[index]);
        if (!(predicate < min_predicate_value)) {
            buffer_index += packed_store_active(&buffer[buffer_index], value);
        }
    }
    for (uniform int index = batch_count; index < count; index++) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const uniform int value = values[index];
        const uniform float predicate = predicates[index];
        if (!(predicate < min_predicate_value)) {
            buffer[buffer_index] = value;
            buffer_index++;
        }
    }
    
    memcpy(&values[0], &buffer[0], sizeof(uniform int) * count);
    return total_front;
}

// export uniform int gather_ints_buffer(
//     uniform int values[],
//     uniform int indices[],
//     uniform int buffer[],
//     uniform int count,
//     uniform int front_self_range)
// {
//     foreach(i = front_self_range ... count) {
//         buffer[i] = values[indices[i]];
//     }
// 
//     foreach(i = 0 ... front_self_range) {
//         values[i] = values[indices[i]];
//     }
// 
//     memcpy(&values[0] + front_self_range, &buffer[0] + front_self_range, sizeof(uniform int) * (count - front_self_range));
// }

// export uniform int gather_ints_buffer(
//     uniform int values[],
//     uniform int indices[],
//     uniform int buffer[],
//     uniform int count,
//     uniform int front_self_range)
// {
//     const uniform int front_self_range_count = max(0, (front_self_range / programCount) * programCount - programCount);
//     const uniform int batch_count = (count / programCount) * programCount;
// 
//     for (uniform int index = front_self_range_count; index < batch_count; index += programCount) {
//         const varying int index_value = streaming_load(&indices[index]);
//         const varying int value_value = values[index_value];
//         streaming_store(&buffer[index], value_value);
//     }
// 
//     for (uniform int index = batch_count; index < count; index++) {
//         buffer[index] = values[indices[index]];
//     }
// 
//     for (uniform int index = 0; index < front_self_range_count; index += programCount) {
//         const varying int index_value = streaming_load(&indices[index]);
//         const varying int value_value = values[index_value];
//         streaming_store(&values[index], value_value);
//     }
// 
//     memcpy(&values[0] + front_self_range_count, &buffer[0] + front_self_range_count, sizeof(uniform int) * (count - front_self_range_count));
// }

// export uniform int gather_ints_buffer(
//     uniform int values[],
//     uniform int indices[],
//     uniform int buffer[],
//     uniform int count,
//     uniform int front_self_range)
// {
//     const uniform int front_self_range_count = max(0, (front_self_range / programCount) * programCount - programCount);
//     const uniform int batch_count = (count / programCount) * programCount;
// 
//     for (uniform int index = front_self_range_count; index < batch_count; index += programCount) {
//         buffer[index + programIndex] = values[indices[index + programIndex]];
//     }
// 
//     for (uniform int index = batch_count; index < count; index++) {
//         buffer[index] = values[indices[index]];
//     }
// 
//     for (uniform int index = 0; index < front_self_range_count; index += programCount) {
//         values[index + programIndex] = values[indices[index + programIndex]];
//     }
// 
//     memcpy(&values[0] + front_self_range_count, &buffer[0] + front_self_range_count, sizeof(uniform int) * (count - front_self_range_count));
// }

export uniform int gather_ints_buffer(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int count,
    uniform int front_self_range)
{
    const uniform int batch_count = (count / programCount) * programCount;

    for (uniform int index = 0; index < batch_count; index += programCount) {
        buffer[index + programIndex] = values[indices[index + programIndex]];
    }

    for (uniform int index = batch_count; index < count; index++) {
        buffer[index] = values[indices[index]];
    }

    memcpy(&values[0], &buffer[0], sizeof(uniform int) * count);
}

export uniform int zip_if_larger_or_equal(
    uniform int values[],
    const uniform float predicates[],
    const uniform int count,
    const uniform float min_predicate_value)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const varying int value = streaming_load(&values[index]);
        const varying float predicate = streaming_load(&predicates[index]);
        if (predicate >= min_predicate_value) {
             buffer_index += packed_store_active(&values[buffer_index], value);
        }
    }
    for (uniform int index = batch_count; index < count; index++) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&predicates[index + 100]);
        const uniform int value = values[index];
        const uniform float predicate = predicates[index];
        if (predicate >= min_predicate_value) {
            values[buffer_index] = value;
            buffer_index++;
        }
    }

    return buffer_index;
}

export uniform int one_mul_add_n(
    uniform float values[],
    uniform float factors[],
    const uniform float other,
    const uniform int count)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&factors[index + 100]);
        const varying float value = values[index + programIndex];
        const varying float factor = factors[index + programIndex];
        values[index + programIndex] = value + factor * other;
    }
    for (uniform int index = batch_count; index < count; index++) {
        prefetch_nt(&values[index + 100]);
        prefetch_nt(&factors[index + 100]);
        const uniform float value = values[index];
        const uniform float factor = factors[index];
        values[index] = value + factor * other;
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

// export void distances(const uniform float positions[][3],
//                       const uniform float target[3],
//                       const uniform int count,
//                       float uniform distances[],
//                       const uniform float offset)
// {
//   for (uniform int index = 0; index < count; index++) {
//     foreach (axis = 0 ... 3) {
//         const float segment = positions[index][axis] - target[axis];
//         distances[index] = sqrt(reduce_add(square(segment))) + offset;
//     }
//   }
// }

// export void distances(const uniform float positions[][3],
//                       const uniform float target[3],
//                       const uniform int count,
//                       float uniform distances[],
//                       const uniform float offset)
// {
//   for (uniform int index = 0; index < count; index++) {
//     foreach (axis = 0 ... 3) {
//         const float segment = positions[index][axis] - target[axis];
//         distances[index] = reduce_add(segment * segment);
//     }
//   }
// 
//   foreach (index = 0 ... count) {
//     distances[index] = sqrt(distances[index]) + offset;
//   }

export uniform float fixed_safe_0_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    values[i] = 1.0f;
  }
}

export uniform float fixed_safe_1_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(values[i]);
  }
}

/*
export uniform float fixed_safe_2_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(values[i]));
  }
}
*/

export uniform float fixed_safe_2_rpow_n(uniform float values[],
                                         const uniform int count)
{
    uniform int index = 0;
    for (; index < count - programCount; index += programCount) {
        prefetch_nt(&values[index + 100]);
        streaming_store(&values[index], safe_rcp(square(streaming_load(&values[index]))));
    }
    for (; index < count; index++) {
        values[index] = safe_rcp(square(values[index]));
    }
}

export uniform float fixed_safe_3_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(values[i]) * values[i]);
  }
}

export uniform float fixed_safe_4_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(square(values[i])));
  }
}

export uniform float fixed_safe_5_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(square(values[i])) * values[i]);
  }
}

export uniform float fixed_safe_6_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    const float squared = square(values[i]);
    values[i] = safe_rcp(square(squared) * squared);
  }
}

export uniform float fixed_safe_7_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    const float squared = square(values[i]);
    values[i] = safe_rcp(square(squared) * squared * values[i]);
  }
}

export uniform float fixed_safe_8_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(square(square(values[i]))));
  }
}

export uniform float fixed_safe_9_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    values[i] = safe_rcp(square(square(square(values[i]))) * values[i]);
  }
}

export uniform float fixed_safe_10_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    const float squared = square(values[i]);
    values[i] = safe_rcp(square(square(squared)) * squared);
  }
}

export uniform float fixed_safe_11_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    const float squared = square(values[i]);
    values[i] = safe_rcp(square(square(squared)) * squared * values[i]);
  }
}

export uniform float fixed_safe_12_rpow_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    prefetch_nt(&values[i + 100]);
    const float fourth_degree = square(square(values[i]));
    values[i] = safe_rcp(square(fourth_degree) * fourth_degree);
  }
}