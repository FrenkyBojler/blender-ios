
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
        varying float<3> batch = {streaming_load(&positions_x[index]),
                                  streaming_load(&positions_y[index]),
                                  streaming_load(&positions_z[index])};
        batch -= targets;
        batch *= batch;
        distances[index + programIndex] = sqrt(batch.x + batch.y + batch.z) + offset;
    }

    uniform float sufix_x[programCount];
    uniform float sufix_y[programCount];
    uniform float sufix_z[programCount];

    memcpy(&sufix_x[0], (uniform float * uniform)&positions_x[batch_count], sizeof(uniform float) * (count - batch_count));
    memcpy(&sufix_y[0], (uniform float * uniform)&positions_y[batch_count], sizeof(uniform float) * (count - batch_count));
    memcpy(&sufix_z[0], (uniform float * uniform)&positions_z[batch_count], sizeof(uniform float) * (count - batch_count));

    uniform float sufix_result[programCount];

    varying float<3> sufix_batch = {sufix_x[programIndex], sufix_y[programIndex], sufix_z[programIndex]};
    sufix_batch -= targets;
    sufix_batch *= sufix_batch;
    sufix_result[programIndex] = sqrt(sufix_batch.x + sufix_batch.y + sufix_batch.z) + offset;

    memcpy((uniform float * uniform)&distances[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
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

export uniform int predicate_revers_indices_float_cmp(
    uniform int indices[],
    uniform float predicates[],
    const uniform int count,
    const uniform float min_predicate_value)
{
    uniform int front = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float predicate = streaming_load(&predicates[index]);
        const varying bool mask = predicate < min_predicate_value;
        indices[index + programIndex] = (front + exclusive_scan_add((int)mask)) * (int)mask;
        front += popcnt(packmask(mask));
    }
    for (uniform int index = batch_count; index < count; index++) {
        const uniform float predicate = predicates[index];
        const uniform bool mask = predicate < min_predicate_value;
        indices[index] = front * (int)mask;
        front += mask;
    }

    const uniform int total_front = front;

    if (total_front == count || total_front == 0) {
      return total_front;
    }

    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float predicate = streaming_load(&predicates[index]);
        const varying bool mask = !(predicate < min_predicate_value);
        const varying int old = streaming_load(&indices[index]);
        indices[index + programIndex] = old + (front + exclusive_scan_add((int)mask)) * (int)mask;
        front += popcnt(packmask(mask));
    }

    for (uniform int index = batch_count; index < count; index++) {
        const uniform float predicate = predicates[index];
        const uniform bool mask =!(predicate < min_predicate_value);
        const uniform int offset = front * (int)mask;
        indices[index] += front * (int)mask;
        front += mask;
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
        // prefetch_nt(&values[index + 100]);
        // prefetch_nt(&predicates[index + 100]);
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
        // prefetch_nt(&values[index + 100]);
        // prefetch_nt(&predicates[index + 100]);
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

struct IndicesStruct {
  int max_size_items_start;
  int max_size_items_end;
  int rest_size_start;
  int rest_size_end;
};

export uniform int gather_ints_buffer_segmented(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int count,
    uniform IndicesStruct * uniform more_or_equal,
    uniform IndicesStruct * uniform smaller)
{
  uniform int iter = 0;
    for (uniform int index = more_or_equal->max_size_items_start; index < more_or_equal->max_size_items_end; index++) {
        buffer[iter + programIndex] = streaming_load(&values[indices[index]]);
        iter += programCount;
    }

    for (uniform int index = more_or_equal->rest_size_start; index < more_or_equal->rest_size_end; index++) {
        buffer[iter] = values[indices[index]];
        iter++;
    }

    for (uniform int index = smaller->max_size_items_start; index < smaller->max_size_items_end; index++) {
        buffer[iter + programIndex] = streaming_load(&values[indices[index]]);
        iter += programCount;
    }

    for (uniform int index = smaller->rest_size_start; index < smaller->rest_size_end; index++) {
        buffer[iter] = values[indices[index]];
        iter++;
    }

    memcpy(&values[0], &buffer[0], sizeof(uniform int) * count);
}

enum ePredicateStatisics {
    epsNone,
    epsAll,
    epsMixed,
};

export uniform ePredicateStatisics float_more_then_single(
    uniform float values[],
    uniform float min_predicate_value,
    uniform int count)
{
    const uniform int batch_count = (count / programCount) * programCount;

    varying int total = 0;
    for (uniform int index = 0; index < batch_count / 2; index += programCount) {
        total += (int)(streaming_load(&values[index]) < min_predicate_value);
    }
    const uniform int half_total = reduce_add(total);
    if (half_total > 0 && half_total < batch_count / 2) {
        return epsMixed;
    }

    for (uniform int index = batch_count / 2; index < batch_count; index += programCount) {
        total += (int)(streaming_load(&values[index]) < min_predicate_value);
    }

    uniform int rest_total = 0;
    for (uniform int index = batch_count; index < count; index++) {
        rest_total += (int)(values[index] < min_predicate_value);
    }

    const uniform int full_total = reduce_add(total) + rest_total;
    if (full_total == 0) {
        return epsNone;
    }

    if (full_total == count) {
        return epsAll;
    }

    return epsMixed;
}

export uniform int float_compare_n_indices_segmented(
    uniform float predicates[],
    const uniform float min_predicate_value,
    uniform IndicesStruct * uniform more_or_equal,
    uniform IndicesStruct * uniform smaller,
    uniform int indices[],
    uniform int buffer[],
    const uniform int count)
{
    uniform int max_size_iter = 0;
    uniform int rest_items_iter = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float predicate = streaming_load(&predicates[index]);
        indices[max_size_iter] = index;

        uniform int total_rest = 0;
        if (predicate < min_predicate_value) {
            total_rest += packed_store_active(&buffer[rest_items_iter], index + programIndex);
        }

        const uniform bool full_batch = all(predicate < min_predicate_value);
        max_size_iter += (int)full_batch;
        rest_items_iter += ((int)!full_batch) * total_rest;
    }

    more_or_equal->max_size_items_start = 0;
    more_or_equal->max_size_items_end = max_size_iter;

    memcpy(&indices[0] + max_size_iter,
        &buffer[0],
        sizeof(uniform int) * (rest_items_iter));

    for (uniform int index = batch_count; index < count; index++) {
        const uniform float predicate = predicates[index];
        indices[max_size_iter + rest_items_iter] = index;
        rest_items_iter += (int)(predicate < min_predicate_value);
    }
    more_or_equal->rest_size_start = max_size_iter;
    more_or_equal->rest_size_end = max_size_iter + rest_items_iter;

    const uniform int total_more_or_equal = rest_items_iter + max_size_iter * programCount;

    {
        uniform int max_size_iter_back = max_size_iter + rest_items_iter;
        uniform int rest_items_iter_back = 0;
        const uniform int batch_count = (count / programCount) * programCount;
        for (uniform int index = 0; index < batch_count; index += programCount) {
            const varying float predicate = streaming_load(&predicates[index]);
            indices[max_size_iter_back] = index;

            uniform int total_rest = 0;
            if (!(predicate < min_predicate_value)) {
                total_rest += packed_store_active(&buffer[rest_items_iter_back], index + programIndex);
            }

            const uniform bool full_batch = all(!(predicate < min_predicate_value));
            max_size_iter_back += (int)full_batch;
            rest_items_iter_back += ((int)!full_batch) * total_rest;
        }

        smaller->max_size_items_start = max_size_iter + rest_items_iter;
        smaller->max_size_items_end = max_size_iter_back;

        memcpy(&indices[0] + max_size_iter_back,
            &buffer[0],
            sizeof(uniform int) * (rest_items_iter_back));

        for (uniform int index = batch_count; index < count; index++) {
            const uniform float predicate = predicates[index];
            indices[max_size_iter_back + rest_items_iter_back] = index;
            rest_items_iter_back += (int)(!(predicate < min_predicate_value));
        }
        smaller->rest_size_start = max_size_iter_back;
        smaller->rest_size_end = max_size_iter_back + rest_items_iter_back;
    }

    return total_more_or_equal;
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
//     assume((((uniform int64)(&values[0])) % (TARGET_WIDTH * sizeof(int))) == 0);
//     assume((((uniform int64)(&indices[0])) % (TARGET_WIDTH * sizeof(int))) == 0);
//     assume((((uniform int64)(&buffer[0])) % (TARGET_WIDTH * sizeof(int))) == 0);
// 
//     const uniform int front_self_range_count = max(0, (front_self_range / programCount) * programCount - programCount);
//     const uniform int batch_count = (count / programCount) * programCount;
// 
//     for (uniform int index = front_self_range_count; index < batch_count; index += programCount) {
//         const varying int index_value = streaming_load(&indices[index]);
//         const varying int value_value = values[index_value];
//         buffer[index + programIndex] = value_value;
//     }
// 
//     for (uniform int index = batch_count; index < count; index++) {
//         buffer[index] = values[indices[index]];
//     }
// 
//     for (uniform int index = 0; index < front_self_range_count; index += programCount) {
//         const varying int index_value = streaming_load(&indices[index]);
//         const varying int value_value = values[index_value];
//         values[index + programIndex] = value_value;
//     }
// 
//     memcpy(&values[0] + front_self_range_count, &buffer[0] + front_self_range_count, sizeof(uniform int) * (count - front_self_range_count));
// }



static void gather_ints(
    uniform int src[],
    uniform int indices[],
    uniform int dst[],
    uniform int indices_start,
    uniform int indices_count)
{
  uniform int tr = 0;
  for (uniform int index = indices_start; index < indices_start + indices_count; index++) {
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

static void scatter_ints(
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

    // uniform int tr1 = 0;
    // for (uniform int index = 0; index < mapping_total; index++) {
    //   buffer[tr1] = values[indices[index]];
    //   tr1++;
    // }
    // uniform int tr2 = mapping_half;
    // for (uniform int index = 0; index < mapping_half; index++) {
    //   values[indices[index]] = buffer[tr2];
    //   tr2++;
    // }
    // uniform int tr3 = 0;
    // for (uniform int index = mapping_half; index < mapping_total; index++) {
    //   values[indices[index]] = buffer[tr3];
    //   tr3++;
    // }
    // return;
    

    gather_ints(values, indices, buffer, 0, mapping_total);

    scatter_ints(buffer, indices, values, mapping_half, mapping_half);
    scatter_ints(buffer + mapping_half, indices, values, 0, mapping_half);
}

export void parition_as_gather_back(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int mapping_total)
{
    assert(mapping_total % 2 == 0);
    const uniform int mapping_half = mapping_total / 2;

    // uniform int tr1 = 0;
    // for (uniform int index = 0; index < mapping_total; index++) {
    //   buffer[tr1] = values[indices[index]];
    //   tr1++;
    // }
    // uniform int tr2 = mapping_half;
    // for (uniform int index = 0; index < mapping_half; index++) {
    //   values[indices[index]] = buffer[tr2];
    //   tr2++;
    // }
    // uniform int tr3 = 0;
    // for (uniform int index = mapping_half; index < mapping_total; index++) {
    //   values[indices[index]] = buffer[tr3];
    //   tr3++;
    // }
    // return;

    gather_ints(values, indices, buffer, 0, mapping_half);

    scatter_ints(buffer, indices, values, mapping_half, mapping_half);
}

// export void parition_as_gather_front_only(
//     uniform int values[],
//     uniform int indices[],
//     uniform int buffer[],
//     uniform int mapping_total)
// {
//     assert(mapping_total % 2 == 0);
//     const uniform int mapping_half = mapping_total / 2;
// 
//     gather_ints(values, indices, buffer, mapping_half, mapping_half);
//     scatter_ints(buffer, indices, values, 0, mapping_half);
// }

export uniform int float_more_then_single_count(
    uniform float values[],
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
             iter += packed_store_active(&indices[iter], index + programIndex);
        }
    }
    for (uniform int index = end_round_down; index < predicates_start + predicates_count; index++) {
        indices[iter] = index;
        iter += (int)(!(predicates[index] < min_predicate_value));
    }
    return iter;
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
             iter += packed_store_active(&indices[iter], index + programIndex);
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
    uniform float predicates[],
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
    
    
    
    const uniform int front_to_move = n_float_more_then_single(indices,
                                                               predicates,
                                                               min_predicate_value,
                                                               0,
                                                               total_front_size);
    const uniform int back_to_move = n_float_less_then_or_equal_single(indices + front_to_move,
                                                                       predicates,
                                                                       min_predicate_value,
                                                                       total_front_size,
                                                                       count - total_front_size);
    // print("%, %: (%, %);\n", count, total_front_size, front_to_move, back_to_move); // 6, 4: (2, 8);
    assert(front_to_move == back_to_move);
    return front_to_move + back_to_move;
}



export uniform int gather_ints_buffer(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int count,
    uniform int front_self_range)
{
    const uniform int front_self_range_count = max(0, (front_self_range / programCount) * programCount - programCount);
    const uniform int batch_count = (count / programCount) * programCount;

    for (uniform int index = front_self_range_count; index < batch_count; index += programCount) {
        buffer[index + programIndex] = values[streaming_load(&indices[index])];
    }

    for (uniform int index = batch_count; index < count; index++) {
        buffer[index] = values[indices[index]];
    }

    for (uniform int index = 0; index < front_self_range_count; index += programCount) {
        values[index + programIndex] = values[streaming_load(&indices[index])];
    }

    memcpy(&values[0] + front_self_range_count, &buffer[0] + front_self_range_count, sizeof(uniform int) * (count - front_self_range_count));
}

export uniform int scatter_ints_buffer(
    uniform int values[],
    uniform int indices[],
    uniform int buffer[],
    uniform int count,
    uniform int front_self_range)
{
    const uniform int batch_count = (count / programCount) * programCount;

    for (uniform int index = 0; index < batch_count; index += programCount) {
        buffer[streaming_load(&indices[index])] = streaming_load(&values[index]);
    }

    for (uniform int index = batch_count; index < count; index++) {
        buffer[indices[index]] = values[index];
    }

    memcpy(&values[0], &buffer[0], sizeof(uniform int) * count);
}

// export uniform int gather_ints_buffer(
//     uniform int values[],
//     uniform int indices[],
//     uniform int buffer[],
//     uniform int count,
//     uniform int front_self_range)
// {
//     const uniform int batch_count = (count / programCount) * programCount;
// 
//     // for (uniform int index = 0; index < count; index += programCount * 8) {
//     //     prefetch_l2(&values[index + programIndex]);
//     // }
// 
//     for (uniform int index = 0; index < batch_count; index += programCount) {
//         buffer[index + programIndex] = values[streaming_load(&indices[index])];
//     }
// 
//     for (uniform int index = batch_count; index < count; index++) {
//         buffer[index] = values[indices[index]];
//     }
// 
//     memcpy(&values[0], &buffer[0], sizeof(uniform int) * count);
// }

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
        const varying float value = values[index + programIndex];
        const varying float factor = factors[index + programIndex];
        // const varying float factor = streaming_load(&factors[index]);
        values[index + programIndex] = value + factor * other;
    }
    for (uniform int index = batch_count; index < count; index++) {
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
        // prefetch_nt(&values[index + 100]);
        values[index + programIndex] = safe_rcp(square(streaming_load(&values[index])));
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