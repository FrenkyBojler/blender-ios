/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Implements Gabor noise based on the paper:
 *
 *   Lagae, Ares, et al. "Procedural noise using sparse Gabor convolution." ACM Transactions on
 *   Graphics (TOG) 28.3f (2009): 1-10.
 *
 * But with the improvements from the paper:
 *
 *   Tavernier, Vincent, et al. "Making gabor noise fast and normalized." Eurographics 2019-40th
 *   Annual Conference of the European Association for Computer Graphics. 2019.
 *
 * And compute the Phase and Intensity of the Gabor based on the paper:
 *
 *   Tricard, Thibault, et al. "Procedural phasor noise." ACM Transactions on Graphics (TOG) 38.4f
 *   (2019): 1-13.
 */

#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_math_constants_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

#define SHD_GABOR_TYPE_2D 0.0f
#define SHD_GABOR_TYPE_3D 1.0f

/* The original Gabor noise paper specifies that the impulses count for each cell should be
 * computed by sampling a Poisson distribution whose mean is the impulse density. However,
 * Tavernier's paper showed that stratified Poisson point sampling is better assuming the weights
 * are sampled using a Bernoulli distribution, as shown in Figure (3). By stratified sampling, they
 * mean a constant number of impulses per cell, so the stratification is the grid itself in that
 * sense, as described in the supplementary material of the paper. */
#define IMPULSES_COUNT 8

/* Computes a 2D Gabor kernel based on Equation (6) in the original Gabor noise paper. Where the
 * frequency argument is the F_0 parameter and the orientation argument is the w_0 parameter. We
 * assume the Gaussian envelope has a unit magnitude, that is, K = 1. That is because we will
 * eventually normalize the final noise value to the unit range, so the multiplication by the
 * magnitude will be canceled by the normalization. Further, we also assume a unit Gaussian width,
 * that is, a = 1. That is because it does not provide much artistic control. It follows that the
 * Gaussian will be truncated at pi.
 *
 * To avoid the discontinuities caused by the aforementioned truncation, the Gaussian is windowed
 * using a Hann window, that is because contrary to the claim made in the original Gabor paper,
 * truncating the Gaussian produces significant artifacts especially when differentiated for bump
 * mapping. The Hann window is C1 continuous and has limited effect on the shape of the Gaussian,
 * so it felt like an appropriate choice.
 *
 * Finally, instead of computing the Gabor value directly, we instead use the complex phasor
 * formulation described in section 3.1.1 in Tricard's paper. That's done to be able to compute
 * the phase and intensity of the Gabor noise after summation based on equations (8) and (9). The
 * return value of the Gabor kernel function is then a complex number whose real value is the
 * value computed in the original Gabor noise paper, and whose imaginary part is the sine
 * counterpart of the real part, which is the only extra computation in the new formulation.
 *
 * Note that while the original Gabor noise paper uses the cosine part of the phasor, that is, the
 * real part of the phasor, we use the sine part instead, that is, the imaginary part of the
 * phasor, as suggested by Tavernier's paper in "Section 3.3. Instance stationarity and
 * normalization", to ensure a zero mean, which should help with normalization. */
float2 compute_2d_gabor_kernel(float2 position, float frequency, float orientation, out float2 derivative)
{
  float distance_squared = length_squared(position);
  float hann_window = 0.5f + 0.5f * cos(M_PI * distance_squared);
  float gaussian_envelop = exp(-M_PI * distance_squared);
  float windowed_gaussian_envelope = gaussian_envelop * hann_window;

  float2 frequency_vector = frequency * float2(cos(orientation), sin(orientation));
  float angle = 2.0f * M_PI * dot(position, frequency_vector);
  float cos_angle = cos(angle);
  float sin_angle = sin(angle);
  float2 phasor = float2(cos_angle, sin_angle);

  float2 kernel_value = windowed_gaussian_envelope * phasor;

  /* Compute derivative of envelope: envelope = gaussian * hann */
  /* ∇envelope = -π * p * exp(-πr²) * (sin(πr²) + 2 * hann) */
  float2 envelope_derivative = -M_PI * position * gaussian_envelop *
                               (sin(M_PI * distance_squared) + 2.0f * hann_window);

  /* Compute derivative of sin(θ) where θ = 2π * dot(p, f) */
  /* ∇sin(θ) = cos(θ) * ∇θ = cos(θ) * 2π * frequency_vector */
  float2 sin_derivative = cos_angle * 2.0f * M_PI * frequency_vector;

  /* Apply product rule: ∇(envelope * sin(θ)) = ∇envelope * sin(θ) + envelope * ∇sin(θ) */
  derivative = envelope_derivative * sin_angle + windowed_gaussian_envelope * sin_derivative;

  return kernel_value;
}

/* Computes the approximate standard deviation of the zero mean normal distribution representing
 * the amplitude distribution of the noise based on Equation (9) in the original Gabor noise paper.
 * For simplicity, the Hann window is ignored and the orientation is fixed since the variance is
 * orientation invariant. We start integrating the squared Gabor kernel with respect to x:
 *
 *   \int_{-\infty}^{-\infty} (e^{- \pi (x^2 + y^2)} cos(2 \pi f_0 x))^2 dx
 *
 * Which gives:
 *
 *  \frac{(e^{2 \pi f_0^2}-1) e^{-2 \pi y^2 - 2 pi f_0^2}}{2^\frac{3}{2}}
 *
 * Then we similarly integrate with respect to y to get:
 *
 *  \frac{1 - e^{-2 \pi f_0^2}}{4}
 *
 * Secondly, we note that the second moment of the weights distribution is 0.5 since it is a
 * fair Bernoulli distribution. So the final standard deviation expression is square root the
 * integral multiplied by the impulse density multiplied by the second moment.
 *
 * Note however that the integral is almost constant for all frequencies larger than one, and
 * converges to an upper limit as the frequency approaches infinity, so we replace the expression
 * with the following limit:
 *
 *  \lim_{x \to \infty} \frac{1 - e^{-2 \pi f_0^2}}{4}
 *
 * To get an approximation of 0.25. */
float compute_2d_gabor_standard_deviation()
{
  float integral_of_gabor_squared = 0.25f;
  float second_moment = 0.5f;
  return sqrt(IMPULSES_COUNT * second_moment * integral_of_gabor_squared);
}

/* Computes the Gabor noise value at the given position for the given cell. This is essentially the
 * sum in Equation (8) in the original Gabor noise paper, where we sum Gabor kernels sampled at a
 * random position with a random weight. The orientation of the kernel is constant for anisotropic
 * noise while it is random for isotropic noise. The original Gabor noise paper mentions that the
 * weights should be uniformly distributed in the [-1, 1] range, however, Tavernier's paper showed
 * that using a Bernoulli distribution yields better results, so that is what we do. */
float2 compute_2d_gabor_noise_cell(
    float2 cell, float2 position, float frequency, float isotropy, float base_orientation)

{
  float2 noise = float2(0.0f);
  for (int i = 0; i < IMPULSES_COUNT; ++i) {
    /* Compute unique seeds for each of the needed random variables. */
    float3 seed_for_orientation = float3(cell, i * 3);
    float3 seed_for_kernel_center = float3(cell, i * 3 + 1);
    float3 seed_for_weight = float3(cell, i * 3 + 2);

    /* For isotropic noise, add a random orientation amount, while for anisotropic noise, use the
     * base orientation. Linearly interpolate between the two cases using the isotropy factor. Note
     * that the random orientation range spans pi as opposed to two pi, that's because the Gabor
     * kernel is symmetric around pi. */
    float random_orientation = (hash_vec3_to_float(seed_for_orientation) - 0.5f) * M_PI;
    float orientation = base_orientation + random_orientation * isotropy;

    float2 kernel_center = hash_vec3_to_vec2(seed_for_kernel_center);
    float2 position_in_kernel_space = position - kernel_center;

    /* The kernel is windowed beyond the unit distance, so early exit with a zero for points that
     * are further than a unit radius. */
    if (length_squared(position_in_kernel_space) >= 1.0f) {
      continue;
    }

    /* We either add or subtract the Gabor kernel based on a Bernoulli distribution of equal
     * probability. */
        float weight = hash_vec3_to_float(seed_for_weight) < 0.5f ? -1.0f : 1.0f;

        float2 dummy_deriv;
        noise += weight * compute_2d_gabor_kernel(position_in_kernel_space, frequency, orientation, dummy_deriv);
  }
  return noise;
}

/* Computes the Gabor noise value by dividing the space into a grid and evaluating the Gabor noise
 * in the space of each cell of the 3x3 cell neighborhood. */
float2 compute_2d_gabor_noise(float2 coordinates,
                              float frequency,
                              float isotropy,
                              float base_orientation,
                              out float2 derivative)
{
  float2 cell_position = floor(coordinates);
  float2 local_position = coordinates - cell_position;

  float2 sum = float2(0.0f);
  float2 derivative_sum = float2(0.0f);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      float2 cell_offset = float2(i, j);

      float2 current_cell_position = cell_position + cell_offset;
      float2 position_in_cell_space = local_position - cell_offset;

      float2 cell_noise = float2(0.0f);
      float2 cell_derivative = float2(0.0f);
      for (int k = 0; k < IMPULSES_COUNT; ++k) {
        float3 seed_for_orientation = float3(current_cell_position, float(k * 3));
        float3 seed_for_kernel_center = float3(current_cell_position, float(k * 3 + 1));
        float3 seed_for_weight = float3(current_cell_position, float(k * 3 + 2));

        float random_orientation = (hash_vec3_to_float(seed_for_orientation) - 0.5f) * M_PI;
        float orientation = base_orientation + random_orientation * isotropy;

        float2 kernel_center = hash_vec3_to_vec2(seed_for_kernel_center);
        float2 position_in_kernel_space = position_in_cell_space - kernel_center;

        if (length_squared(position_in_kernel_space) >= 1.0f) {
          continue;
        }

        float weight = hash_vec3_to_float(seed_for_weight) < 0.5f ? -1.0f : 1.0f;

        float2 kernel_deriv;
        float2 kernel_value = compute_2d_gabor_kernel(
            position_in_kernel_space, frequency, orientation, kernel_deriv);
        cell_noise += weight * kernel_value;
        cell_derivative += weight * kernel_deriv;
      }
      sum += cell_noise;
      derivative_sum += cell_derivative;
    }
  }

  derivative = derivative_sum;
  return sum;
}

/* Identical to compute_2d_gabor_kernel, except it is evaluated in 3D space. Notice that Equation
 * (6) in the original Gabor noise paper computes the frequency vector using (cos(w_0), sin(w_0)),
 * which we also do in the 2D variant, however, for 3D, the orientation is already a unit frequency
 * vector, so we just need to scale it by the frequency value. */
float2 compute_3d_gabor_kernel(float3 position, float frequency, float3 orientation, out float3 derivative)
{
  float distance_squared = length_squared(position);
  float hann_window = 0.5f + 0.5f * cos(M_PI * distance_squared);
  float gaussian_envelop = exp(-M_PI * distance_squared);
  float windowed_gaussian_envelope = gaussian_envelop * hann_window;

  float3 frequency_vector = frequency * orientation;
  float angle = 2.0f * M_PI * dot(position, frequency_vector);
  float cos_angle = cos(angle);
  float sin_angle = sin(angle);
  float2 phasor = float2(cos_angle, sin_angle);

  float2 kernel_value = windowed_gaussian_envelope * phasor;

  /* Compute derivative of envelope */
  float3 envelope_derivative = -M_PI * position * gaussian_envelop *
                               (sin(M_PI * distance_squared) + 2.0f * hann_window);

  /* Compute derivative of sin(θ) where θ = 2π * dot(p, f) */
  float3 sin_derivative = cos_angle * 2.0f * M_PI * frequency_vector;

  /* Apply product rule: ∇(envelope * sin(θ)) = ∇envelope * sin(θ) + envelope * ∇sin(θ) */
  derivative = envelope_derivative * sin_angle + windowed_gaussian_envelope * sin_derivative;

  return kernel_value;
}

/* Identical to compute_2d_gabor_standard_deviation except we do triple integration in 3D. The only
 * difference is the denominator in the integral expression, which is 2^{5 / 2} for the 3D case
 * instead of 4 for the 2D case. Similarly, the limit evaluates to 1 / (4 * sqrt(2)). */
float compute_3d_gabor_standard_deviation()
{
  float integral_of_gabor_squared = 1.0f / (4.0f * M_SQRT2);
  float second_moment = 0.5f;
  return sqrt(IMPULSES_COUNT * second_moment * integral_of_gabor_squared);
}

/* Computes the orientation of the Gabor kernel such that it is constant for anisotropic
 * noise while it is random for isotropic noise. We randomize in spherical coordinates for a
 * uniform distribution. */
float3 compute_3d_orientation(float3 orientation, float isotropy, float4 seed)
{
  /* Return the base orientation in case we are completely anisotropic. */
  if (isotropy == 0.0f) {
    return orientation;
  }

  /* Compute the orientation in spherical coordinates. */
  float inclination = acos(orientation.z);
  float azimuth = sign(orientation.y) * acos(orientation.x / length(orientation.xy));

  /* For isotropic noise, add a random orientation amount, while for anisotropic noise, use the
   * base orientation. Linearly interpolate between the two cases using the isotropy factor. Note
   * that the random orientation range is to pi as opposed to two pi, that's because the Gabor
   * kernel is symmetric around pi. */
  float2 random_angles = hash_vec4_to_vec2(seed) * M_PI;
  inclination += random_angles.x * isotropy;
  azimuth += random_angles.y * isotropy;

  /* Convert back to Cartesian coordinates, */
  return float3(
      sin(inclination) * cos(azimuth), sin(inclination) * sin(azimuth), cos(inclination));
}

float2 compute_3d_gabor_noise_cell(
    float3 cell, float3 position, float frequency, float isotropy, float3 base_orientation, out float3 derivative)

{
  float2 noise = float2(0.0f);
  float3 derivative_sum = float3(0.0f);
  for (int i = 0; i < IMPULSES_COUNT; ++i) {
    /* Compute unique seeds for each of the needed random variables. */
    float4 seed_for_orientation = float4(cell, i * 3);
    float4 seed_for_kernel_center = float4(cell, i * 3 + 1);
    float4 seed_for_weight = float4(cell, i * 3 + 2);

    float3 orientation = compute_3d_orientation(base_orientation, isotropy, seed_for_orientation);

    float3 kernel_center = hash_vec4_to_vec3(seed_for_kernel_center);
    float3 position_in_kernel_space = position - kernel_center;

    /* The kernel is windowed beyond the unit distance, so early exit with a zero for points that
     * are further than a unit radius. */
    if (length_squared(position_in_kernel_space) >= 1.0f) {
      continue;
    }

    /* We either add or subtract the Gabor kernel based on a Bernoulli distribution of equal
     * probability. */
    float weight = hash_vec4_to_float(seed_for_weight) < 0.5f ? -1.0f : 1.0f;

    float3 kernel_deriv;
    noise += weight * compute_3d_gabor_kernel(position_in_kernel_space, frequency, orientation, kernel_deriv);
    derivative_sum += weight * kernel_deriv;
  }
  derivative = derivative_sum;
  return noise;
}

/* Identical to compute_2d_gabor_noise but works in the 3D neighborhood of the noise. */
float2 compute_3d_gabor_noise(float3 coordinates,
                              float frequency,
                              float isotropy,
                              float3 base_orientation,
                              out float3 derivative)
{
  float3 cell_position = floor(coordinates);
  float3 local_position = coordinates - cell_position;

  float2 sum = float2(0.0f);
  float3 derivative_sum = float3(0.0f);
  for (int k = -1; k <= 1; k++) {
    for (int j = -1; j <= 1; j++) {
      for (int i = -1; i <= 1; i++) {
        float3 cell_offset = float3(i, j, k);
        float3 current_cell_position = cell_position + cell_offset;
        float3 position_in_cell_space = local_position - cell_offset;
        float3 cell_derivative;
        sum += compute_3d_gabor_noise_cell(
            current_cell_position, position_in_cell_space, frequency, isotropy, base_orientation, cell_derivative);
        derivative_sum += cell_derivative;
      }
    }
  }

  derivative = derivative_sum;
  return sum;
}

[[node]]
void node_tex_gabor(float3 coordinates,
                    float scale,
                    float frequency,
                    float anisotropy,
                    float orientation_2d,
                    float3 orientation_3d,
                    float type,
                    float &output_value,
                    float &output_phase,
                    float &output_intensity,
                    float3 &output_normal)
{
  float3 scaled_coordinates = coordinates * scale;
  float isotropy = 1.0f - clamp(anisotropy, 0.0f, 1.0f);
  frequency = max(0.001f, frequency);

  float2 phasor = float2(0.0f);
  float3 derivative = float3(0.0f);
  float standard_deviation = 1.0f;
  if (type == SHD_GABOR_TYPE_2D) {
    float2 noise_derivative;
    phasor = compute_2d_gabor_noise(
        scaled_coordinates.xy, frequency, isotropy, orientation_2d, noise_derivative);
    /* Extend 2D derivative to 3D: invert X and Y (Blender convention), set Z=1 for upward normal */
    derivative = float3(-noise_derivative.x * scale, -noise_derivative.y * scale, 1.0f);
    standard_deviation = compute_2d_gabor_standard_deviation();
  }
  else if (type == SHD_GABOR_TYPE_3D) {
    float3 orientation = normalize(orientation_3d);
    phasor = compute_3d_gabor_noise(
        scaled_coordinates, frequency, isotropy, orientation, derivative);
    /* Invert X and Y (Blender convention) and scale by the scale factor */
    derivative = float3(-derivative.x * scale, -derivative.y * scale, derivative.z * scale);
    standard_deviation = compute_3d_gabor_standard_deviation();
  }

  /* Normalize the noise by dividing by six times the standard deviation, which was determined
   * empirically. */
  float normalization_factor = 6.0f * standard_deviation;

  /* As discussed in compute_2d_gabor_kernel, we use the imaginary part of the phasor as the Gabor
   * value. But remap to [0, 1] from [-1, 1]. */
  output_value = (phasor.y / normalization_factor) * 0.5f + 0.5f;

  /* Compute the phase based on equation (9) in Tricard's paper. But remap the phase into the
   * [0, 1] range. */
  output_phase = (atan2(phasor.y, phasor.x) + M_PI) / (2.0f * M_PI);

  /* Compute the intensity based on equation (8) in Tricard's paper. */
  output_intensity = length(phasor) / normalization_factor;

  /* Normalize the derivatives. Keep normals in the same space as input coordinates.
   * The derivative is of phasor.y, so we divide by normalization_factor first.
   * For 2D, we keep Z=1 constant (not divided by normalization_factor) to ensure upward normal. */
  float3 normal;
  if (type == SHD_GABOR_TYPE_2D) {
    normal = float3(derivative.x / normalization_factor, 
                    derivative.y / normalization_factor, 
                    derivative.z);
  }
  else {
    normal = derivative / normalization_factor;
  }
  output_normal = normalize(normal);
}
