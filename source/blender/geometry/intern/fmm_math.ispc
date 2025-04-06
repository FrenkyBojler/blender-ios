
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

export void zero_if_in_range_n(const uniform int indices[],
                               uniform float values[],
                               const uniform int range_start,
                               const uniform int range_size,
                               const uniform int count)
{
    const uniform int range_end = range_start + range_size;

    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        varying int indices_batch = streaming_load(&indices[index]);
        varying float values_batch = streaming_load(&values[index]);
        const varying bool mask = indices_batch >= range_start && indices_batch < range_end;
        values[index + programIndex] = mask ? values_batch : 0.0f;
    }

    uniform int indices_sufix[programCount];
    uniform float values_sufix[programCount];

    memcpy(&indices_sufix[0], (uniform int * uniform)&indices[batch_count], sizeof(uniform int) * (count - batch_count));
    memcpy(&values_sufix[0], (uniform float * uniform)&values[batch_count], sizeof(uniform float) * (count - batch_count));

    uniform float sufix_result[programCount];

    const varying bool mask = indices_sufix[programIndex] >= range_start && indices_sufix[programIndex] < range_end;
    sufix_result[programIndex] = mask ? values_sufix[programIndex] : 0.0f;

    memcpy((uniform float * uniform)&values[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
}

export void zero_if_in_index_n(const uniform int indices[],
                               uniform float values[],
                               const uniform int index_value,
                               const uniform int count)
{
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        varying int indices_batch = streaming_load(&indices[index]);
        varying float values_batch = streaming_load(&values[index]);
        const varying bool mask = indices_batch != index_value;
        values[index + programIndex] = mask ? values_batch : 0.0f;
    }

    uniform int indices_sufix[programCount];
    uniform float values_sufix[programCount];

    memcpy(&indices_sufix[0], (uniform int * uniform)&indices[batch_count], sizeof(uniform int) * (count - batch_count));
    memcpy(&values_sufix[0], (uniform float * uniform)&values[batch_count], sizeof(uniform float) * (count - batch_count));

    uniform float sufix_result[programCount];

    const varying bool mask = indices_sufix[programIndex] != index_value;
    sufix_result[programIndex] = mask ? values_sufix[programIndex] : 0.0f;

    memcpy((uniform float * uniform)&values[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
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
                                  positions_z[index + programIndex]};
        batch -= targets;
        batch *= batch;
        distances[index + programIndex] = batch.x + batch.y + batch.z;
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
    sufix_result[programIndex] = sufix_batch.x + sufix_batch.y + sufix_batch.z;

    memcpy((uniform float * uniform)&distances[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
}

export void table_squared_distances(const uniform float row_x[],
                                    const uniform float row_y[],
                                    const uniform float row_z[],
                                    const uniform int row_count,
                                    const uniform float col_x[],
                                    const uniform float col_y[],
                                    const uniform float col_z[],
                                    const uniform int col_count,
                                    float uniform distances[])
{
  for (uniform int row_index = 0; row_index < row_count; row_index++) {
    for (uniform int col_index = 0; col_index < col_count; col_index++) {
      distances[row_index * col_count + col_index] = square(row_x[row_index] - col_x[col_index]) +
                                                     square(row_y[row_index] - col_x[col_index]) +
                                                     square(row_z[row_index] - col_x[col_index]);
    }
  }
}


/**
 * For 9 rows, columns and chunk size = 4:
 * A, B, C, D, E, F, G, H, I
 * 1, 2, 3, 4, 5, 6, 7, 8, 9
 *
 * A1, B1, C1, D1 | A2, B2, C2, D2 | A3, B3, C3, D3 | A4, B4, C4, D4 | A5, B5, C5, D5 | A6, B6, C6, D6 | A7, B7, C7, D7 | A8, B8, C8, D8 | A9, B9, C9, D9,
 * E1, F1, G1, H1 | E2, F2, G2, H2 | E3, F3, G3, H3 | E4, F4, G4, H4 | E5, F5, G5, H5 | E6, F6, G6, H6 | E7, F7, G7, H7 | E8, F8, G8, H8 | E9, F9, G9, H9,
 * I1, I2, I3, I4, I5, I6, I7, I8, I9
 */
export void chunked_squared_distances_table(const uniform float row_x[][16],
                                            const uniform float row_y[][16],
                                            const uniform float row_z[][16],
                                            const uniform int row_count,
                                            const uniform float col_x[][16],
                                            const uniform float col_y[][16],
                                            const uniform float col_z[][16],
                                            const uniform int col_count,
                                            float uniform distances[][16])
{
}

export void min_max_float3(const uniform float values[][3],
                           uniform float min_values[3],
                           uniform float max_values[3],
                           const uniform int count)
{
    const uniform float * uniform values_components = &values[0][0];

    const uniform int batch_size = programCount * 3;
    varying float<3> min_components;
    varying float<3> max_components;

    const uniform int batch_count = ((count * 3) / batch_size) * batch_size;
    for (uniform int index = 0; index < batch_count; index += batch_size) {
        const varying float<3> batch = {values_components[index + programCount * 0 + programIndex],
                                        values_components[index + programCount * 1 + programIndex],
                                        values_components[index + programCount * 2 + programIndex]};
        
        for (uniform int component_i = 0; component_i < 3; component_i++) {
            min_components[component_i] = min(min_components[component_i], batch[component_i]);
            max_components[component_i] = max(max_components[component_i], batch[component_i]);
        }
    }

    min_components[0] = min(min_components[0], shift(min_components[1], -((programCount * 1) % 3)));
    min_components[0] = min(min_components[0], shift(min_components[2], -((programCount * 2) % 3)));

    max_components[0] = max(max_components[0], shift(max_components[1], -((programCount * 1) % 3)));
    max_components[0] = max(max_components[0], shift(max_components[2], -((programCount * 2) % 3)));

    const uniform float limit_min = -FLT_MAX;
    const uniform float limit_max = FLT_MAX;

    for (uniform int component_i = 0; component_i < 3; component_i++) {
        const varying bool mask = programIndex % 3 == component_i;
        min_values[component_i] = reduce_min(min_components[component_i] * (float)(mask) + limit_max * (float)(!mask));
        max_values[component_i] = reduce_max(max_components[component_i] * (float)(mask) + limit_min * (float)(!mask));
    }

    for (uniform int index = batch_count; index < count; index++) {
        min_values[index % 3] = min(min_values[index % 3], values_components[index]);
        max_values[index % 3] = max(max_values[index % 3], values_components[index]);
    }
}

export void distances_split_(const uniform float positions_x[],
                            const uniform float positions_y[],
                            const uniform float positions_z[],
                            const uniform float target[3],
                            const uniform int count,
                            float uniform distances[],
                            const uniform float offset)
{
    const uniform float<3> targets = {target[0], target[1], target[2]};
    const uniform int batch_count = (count / (programCount * 4)) * (programCount * 4);
    for (uniform int index = 0; index < batch_count; index += (programCount * 4)) {
        varying float<3> batch1 = {streaming_load(&positions_x[index + programCount * 0]),
                                   streaming_load(&positions_y[index + programCount * 0]),
                                   streaming_load(&positions_z[index + programCount * 0])};
        varying float<3> batch2 = {streaming_load(&positions_x[index + programCount * 1]),
                                   streaming_load(&positions_y[index + programCount * 1]),
                                   streaming_load(&positions_z[index + programCount * 1])};
        varying float<3> batch3 = {streaming_load(&positions_x[index + programCount * 2]),
                                   streaming_load(&positions_y[index + programCount * 2]),
                                   streaming_load(&positions_z[index + programCount * 2])};
        varying float<3> batch4 = {streaming_load(&positions_x[index + programCount * 3]),
                                   streaming_load(&positions_y[index + programCount * 3]),
                                   streaming_load(&positions_z[index + programCount * 3])};
        batch1 -= targets;
        batch1 *= batch1;
        
        batch2 -= targets;
        batch2 *= batch2;
        
        batch3 -= targets;
        batch3 *= batch3;
        
        batch4 -= targets;
        batch4 *= batch4;
        distances[index + programCount * 0 + programIndex] = batch1.x + batch1.y + batch1.z;
        distances[index + programCount * 1 + programIndex] = batch2.x + batch2.y + batch2.z;
        distances[index + programCount * 2 + programIndex] = batch3.x + batch3.y + batch3.z;
        distances[index + programCount * 3 + programIndex] = batch4.x + batch4.y + batch4.z;
    }

    uniform float sufix_x[programCount * 4];
    uniform float sufix_y[programCount * 4];
    uniform float sufix_z[programCount * 4];

    memcpy(&sufix_x[0], (uniform float * uniform)&positions_x[batch_count], sizeof(uniform float) * (count - batch_count));
    memcpy(&sufix_y[0], (uniform float * uniform)&positions_y[batch_count], sizeof(uniform float) * (count - batch_count));
    memcpy(&sufix_z[0], (uniform float * uniform)&positions_z[batch_count], sizeof(uniform float) * (count - batch_count));

    uniform float sufix_result[programCount * 4];

    varying float<3> batch1 = {sufix_x[programCount * 0 + programIndex], sufix_y[programCount * 0 + programIndex], sufix_z[programCount * 0 + programIndex]};
    varying float<3> batch2 = {sufix_x[programCount * 1 + programIndex], sufix_y[programCount * 1 + programIndex], sufix_z[programCount * 1 + programIndex]};
    varying float<3> batch3 = {sufix_x[programCount * 2 + programIndex], sufix_y[programCount * 2 + programIndex], sufix_z[programCount * 2 + programIndex]};
    varying float<3> batch4 = {sufix_x[programCount * 3 + programIndex], sufix_y[programCount * 3 + programIndex], sufix_z[programCount * 3 + programIndex]};
    batch1 -= targets;
    batch1 *= batch1;
    
    batch2 -= targets;
    batch2 *= batch2;
    
    batch3 -= targets;
    batch3 *= batch3;
    
    batch4 -= targets;
    batch4 *= batch4;
    sufix_result[programCount * 0 + programIndex] = batch1.x + batch1.y + batch1.z;
    sufix_result[programCount * 1 + programIndex] = batch2.x + batch2.y + batch2.z;
    sufix_result[programCount * 2 + programIndex] = batch3.x + batch3.y + batch3.z;
    sufix_result[programCount * 3 + programIndex] = batch4.x + batch4.y + batch4.z;
    
    memcpy((uniform float * uniform)&distances[batch_count], &sufix_result[0], sizeof(uniform float) * (count - batch_count));
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
    memmove(values, values + total_front_size, (count - total_front_size) * sizeof(uniform float));
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

export uniform int count_float_less_than(
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

// export void replace_by_larger_if_smaller(
//     uniform float predicates[],
//     uniform int buffer[],
//     uniform float min_predicate_value,
//     uniform int count,
//     uniform int  )
// {
//   uniform int tr1 = 0;
//   for (uniform int index = 0; index < count - total_front_size; index++) {
//     if ((predicates[index] < min_predicate_value)) {
//       buffer[tr1] = index;
//       tr1++;
//     }
//   }
//   uniform int tr2 = 0;
//   for (uniform int index = count - total_front_size; index < count; index++) {
//     if (!(predicates[index] < min_predicate_value)) {
//       buffer[tr1 + tr2] = index;
//       tr2++;
//     }
//   }
//   assert(tr1 == tr2);
//   
//   for (uniform int index = 0; index < tr1; index++) {
//     predicates[buffer[index]] = predicates[buffer[tr1 + index]];
//   }
// }

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
        // const varying float factor = factors[index + programIndex];
        const varying float factor = streaming_load(&factors[index]);
        values[index + programIndex] = value + factor * other;
    }
    for (uniform int index = batch_count; index < count; index++) {
        const uniform float value = values[index];
        const uniform float factor = factors[index];
        values[index] = value + factor * other;
    }
    return buffer_index;
}

export uniform int mul_add_n(
    uniform float values[],
    uniform float factors[],
    uniform float others[],
    const uniform int count)
{
    uniform int buffer_index = 0;
    const uniform int batch_count = (count / programCount) * programCount;
    for (uniform int index = 0; index < batch_count; index += programCount) {
        const varying float value = values[index + programIndex];
        const varying float factor = factors[index + programIndex];
        const varying float other = others[index + programIndex];
        values[index + programIndex] = value + factor * other;
    }
    for (uniform int index = batch_count; index < count; index++) {
        const uniform float value = values[index];
        const uniform float factor = factors[index];
        const uniform float other = others[index];
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