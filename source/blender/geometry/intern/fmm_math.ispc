
export uniform float float_dot_product(const uniform float values[],
                                       const uniform float factors[],
                                       const uniform int count)
{
  float accumulator = 0.0f;
  foreach (i = 0 ... count) {
    accumulator += values[i] * factors[i];
  }
  return reduce_add(accumulator);
}

export void float3_dot_product(const uniform float values[][3],
                               const uniform float factors[],
                               const uniform int count,
                               float uniform result[3])
{
  float accumulator[3] = {0.0f, 0.0f, 0.0f};
  foreach (index = 0 ... count, axis = 0 ... 3) {
    accumulator[axis] += values[index][axis] * factors[index];
  }
  result[0] = reduce_add(accumulator[0]);
  result[1] = reduce_add(accumulator[1]);
  result[2] = reduce_add(accumulator[2]);
}

export void float_scatter_mul_add(const uniform float factors[],
                                  const uniform float value,
                                  const uniform int indices[],
                                  uniform float data[],
                                  const uniform int count)
{
  foreach (i = 0 ... count) {
    data[indices[i]] += value * factors[i];
  }
}

float safe_rcp(const float a)
{
  return a ? 1.0f / a : 0.0f;
}

export uniform float fixed_pow_0_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    values[i] = 1.0f;
  }
}

export uniform float fixed_pow_1_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    values[i] = safe_rcp(values[i]);
  }
}

export uniform float fixed_pow_2_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    values[i] = safe_rcp(values[i] * values[i]);
  }
}

export uniform float fixed_pow_3_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    values[i] = safe_rcp(values[i] * values[i] * values[i]);
  }
}

export uniform float fixed_pow_4_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    values[i] = safe_rcp(squared * squared);
  }
}

export uniform float fixed_pow_5_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    values[i] = safe_rcp(squared * squared * values[i]);
  }
}

export uniform float fixed_pow_6_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    values[i] = safe_rcp(squared * squared * squared);
  }
}

export uniform float fixed_pow_7_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    values[i] = safe_rcp(squared * squared * squared * values[i]);
  }
}

export uniform float fixed_pow_8_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    const float fourth_degree = squared * squared;
    values[i] = safe_rcp(fourth_degree * fourth_degree);
  }
}

export uniform float fixed_pow_9_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    const float fourth_degree = squared * squared;
    values[i] = safe_rcp(fourth_degree * fourth_degree * values[i]);
  }
}

export uniform float fixed_pow_10_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    const float fourth_degree = squared * squared;
    values[i] = safe_rcp(fourth_degree * fourth_degree * squared);
  }
}

export uniform float fixed_pow_11_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    const float fourth_degree = squared * squared;
    values[i] = safe_rcp(fourth_degree * fourth_degree * squared * values[i]);
  }
}

export uniform float fixed_pow_12_n(uniform float values[],
                                   const uniform int count)
{
  foreach (i = 0 ... count) {
    const float squared = values[i] * values[i];
    const float fourth_degree = squared * squared;
    values[i] = safe_rcp(fourth_degree * fourth_degree * fourth_degree);
  }
}