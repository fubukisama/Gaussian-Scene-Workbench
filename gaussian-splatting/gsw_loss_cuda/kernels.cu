/* SPDX-FileCopyrightText: 2024 Rahul Goel
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: MIT
 *
 * Derived anew from MIT fused-ssim ssim.cu at
 * 1272e21a282342e89537159e4bad508b19b34157 (not the private GPL prototype).
 * GSW additions: uint8 targets, combined L1/SSIM reduction and backward,
 * scalar gradient chain, reusable Torch workspace/current stream, validation.
 * Original Gaussian convolution and SSIM derivatives retain MIT attribution.
 */
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAException.h>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <torch/extension.h>
#include <cooperative_groups.h>
#include <algorithm>
#include <iostream>
#include <c10/cuda/CUDAGuard.h>

namespace cg = cooperative_groups;

#define G_00 0.001028380123898387f
#define G_01 0.0075987582094967365f
#define G_02 0.036000773310661316f
#define G_03 0.10936068743467331f
#define G_04 0.21300552785396576f
#define G_05 0.26601171493530273f
#define G_06 0.21300552785396576f
#define G_07 0.10936068743467331f
#define G_08 0.036000773310661316f
#define G_09 0.0075987582094967365f
#define G_10 0.001028380123898387f

// block size
#define BX 16
#define BY 16

// shared memory size
#define SX (BX + 10)
#define SSX (BX + 10)
#define SY (BY + 10)

// convolution scratchpad size
#define CX (BX)
#define CCX (BX + 10)
#define CY (BY + 10)


template <typename PixelT>
__device__ float sample_value(const PixelT* input, int index) {
  const float value = static_cast<float>(input[index]);
  // Preserve the normalization's FP32 rounding before subtraction. An FMA
  // contraction across this boundary would make identical pixels have L1>0.
  return std::is_same<PixelT, uint8_t>::value ? __fmul_rn(value, 1.0f / 255.0f) : value;
}

template <typename PixelT>
__device__ float get_pix_value(const PixelT* img, const int b, const int c, const int y, const int x, const int CH, const int H, const int W) {
  if (x >= W || y >= H || x < 0 || y < 0) {
    return 0.0f;
  } else {
    return sample_value(img, b * CH * H * W + c * H * W + y * W + x);
  }
}

template <typename PixelT>
__device__ void load_into_shared(float pixels[SY][SSX], const PixelT *inp, const int CH, const int H, const int W, const int i) {
  auto block = cg::this_thread_block();
  const int batch = block.group_index().z;
  const int start_y = block.group_index().y * BY;
  const int start_x = block.group_index().x * BX;

  const int cnt = SY * SX;
  const int num_blocks = (cnt + BX * BY - 1) / (BX * BY);
  for (int b = 0; b < num_blocks; ++b) {
    int tid = b * (BX * BY) + block.thread_rank();
    if (tid < cnt) {
      int local_y = tid / SX;
      int local_x = tid % SX;
      int y = start_y + local_y;
      int x = start_x + local_x;
      float one = get_pix_value(inp, batch, i, y - 5, x - 5, CH, H, W);
      pixels[local_y][local_x] = one;
    }
  }
}

// Original GSW scalar chain loader: avoids allocating an NCHW dL/dmap.
__device__ void load_loss_chain(float pixels[SY][SSX], const float* gradient,
                               float weight, int n, int ch, int h, int w) {
  auto block = cg::this_thread_block();
  const int start_y = block.group_index().y * BY;
  const int start_x = block.group_index().x * BX;
  const float scale = -weight * gradient[0] / (n * ch * h * w);
  for (int index = block.thread_rank(); index < SY * SX; index += BX * BY) {
    const int row = index / SX, column = index % SX;
    const int y = start_y + row - 5, x = start_x + column - 5;
    pixels[row][column] = y >= 0 && y < h && x >= 0 && x < w ? scale : 0;
  }
}

__device__ void multiply_shared_mem(float pix1[SY][SSX], float pix2[SY][SSX]) {
  auto block = cg::this_thread_block();
  const int cnt = SY * SX;
  const int num_blocks = (cnt + BX * BY - 1) / (BX * BY);
  for (int b = 0; b < num_blocks; ++b) {
    int tid = b * (BX * BY) + block.thread_rank();
    if (tid < cnt) {
      int local_y = tid / SX;
      int local_x = tid % SX;
      float one = pix1[local_y][local_x];
      float two = pix2[local_y][local_x];
      pix1[local_y][local_x] = one * two;
    }
  }
}

__device__ inline float do_sq(float val) {
  return val * val;
}

__device__ void
flush_conv_scratch(float buf[CY][CCX]) {
  auto block = cg::this_thread_block();
  const int cnt = CY * CX;
  const int num_blocks = (cnt + BX * BY - 1) / (BX * BY);
  for (int b = 0; b < num_blocks; ++b) {
    const int tid = b * (BX * BY) + block.thread_rank();
    if (tid < cnt) {
      const int local_y = tid / CX;
      const int local_x = tid % CX;
      buf[local_y][local_x] = 0.0f;
    }
  }
}

__device__ void do_separable_conv_x(float pixels[SY][SSX], float opt[CY][CCX], int H, int W, bool sq = false) {
  auto block = cg::this_thread_block();

  int local_y = block.thread_index().y;
  int local_x = block.thread_index().x + 5;
  float val = 0.0f;

  if (sq) {
    val += G_00 * do_sq(pixels[local_y][local_x - 5]);
    val += G_01 * do_sq(pixels[local_y][local_x - 4]);
    val += G_02 * do_sq(pixels[local_y][local_x - 3]);
    val += G_03 * do_sq(pixels[local_y][local_x - 2]);
    val += G_04 * do_sq(pixels[local_y][local_x - 1]);
    val += G_05 * do_sq(pixels[local_y][local_x    ]);
    val += G_06 * do_sq(pixels[local_y][local_x + 1]);
    val += G_07 * do_sq(pixels[local_y][local_x + 2]);
    val += G_08 * do_sq(pixels[local_y][local_x + 3]);
    val += G_09 * do_sq(pixels[local_y][local_x + 4]);
    val += G_10 * do_sq(pixels[local_y][local_x + 5]);
  } else {
    val += G_00 * pixels[local_y][local_x - 5];
    val += G_01 * pixels[local_y][local_x - 4];
    val += G_02 * pixels[local_y][local_x - 3];
    val += G_03 * pixels[local_y][local_x - 2];
    val += G_04 * pixels[local_y][local_x - 1];
    val += G_05 * pixels[local_y][local_x    ];
    val += G_06 * pixels[local_y][local_x + 1];
    val += G_07 * pixels[local_y][local_x + 2];
    val += G_08 * pixels[local_y][local_x + 3];
    val += G_09 * pixels[local_y][local_x + 4];
    val += G_10 * pixels[local_y][local_x + 5];
  }
  opt[local_y][local_x] = val;

  val = 0.0f;
  local_y = block.thread_index().y + BY;
  if (local_y < SY) {
    if (sq) {
      val += G_00 * do_sq(pixels[local_y][local_x - 5]);
      val += G_01 * do_sq(pixels[local_y][local_x - 4]);
      val += G_02 * do_sq(pixels[local_y][local_x - 3]);
      val += G_03 * do_sq(pixels[local_y][local_x - 2]);
      val += G_04 * do_sq(pixels[local_y][local_x - 1]);
      val += G_05 * do_sq(pixels[local_y][local_x    ]);
      val += G_06 * do_sq(pixels[local_y][local_x + 1]);
      val += G_07 * do_sq(pixels[local_y][local_x + 2]);
      val += G_08 * do_sq(pixels[local_y][local_x + 3]);
      val += G_09 * do_sq(pixels[local_y][local_x + 4]);
      val += G_10 * do_sq(pixels[local_y][local_x + 5]);
    } else {
      val += G_00 * pixels[local_y][local_x - 5];
      val += G_01 * pixels[local_y][local_x - 4];
      val += G_02 * pixels[local_y][local_x - 3];
      val += G_03 * pixels[local_y][local_x - 2];
      val += G_04 * pixels[local_y][local_x - 1];
      val += G_05 * pixels[local_y][local_x    ];
      val += G_06 * pixels[local_y][local_x + 1];
      val += G_07 * pixels[local_y][local_x + 2];
      val += G_08 * pixels[local_y][local_x + 3];
      val += G_09 * pixels[local_y][local_x + 4];
      val += G_10 * pixels[local_y][local_x + 5];
    }
    opt[local_y][local_x] = val;
  }
}

__device__ float do_separable_conv_y(float pixels[CY][CCX], int H, int W, bool sq = false) {
  auto block = cg::this_thread_block();
  int local_y = block.thread_index().y + 5;
  int local_x = block.thread_index().x + 5;
  float val = 0.0f;

  val += G_00 * pixels[local_y - 5][local_x];
  val += G_01 * pixels[local_y - 4][local_x];
  val += G_02 * pixels[local_y - 3][local_x];
  val += G_03 * pixels[local_y - 2][local_x];
  val += G_04 * pixels[local_y - 1][local_x];
  val += G_05 * pixels[local_y    ][local_x];
  val += G_06 * pixels[local_y + 1][local_x];
  val += G_07 * pixels[local_y + 2][local_x];
  val += G_08 * pixels[local_y + 3][local_x];
  val += G_09 * pixels[local_y + 4][local_x];
  val += G_10 * pixels[local_y + 5][local_x];

  return val;
}

template <typename TargetT>
__global__ void fusedssimCUDA(
  int H,
  int W,
  int CH,
  float C1,
  float C2,
  float* img1,
  const TargetT* img2,
  float* ssim_map,
  float* dm_dmu1,
  float* dm_dsigma1_sq,
  float* dm_dsigma12
)
{
  auto block = cg::this_thread_block();
  const int pix_y = block.group_index().y * BY + block.thread_index().y;
  const int pix_x = block.group_index().x * BX + block.thread_index().x;
  const int pix_id = pix_y * W + pix_x;
  const int num_pix = H * W;
  const int batch = block.group_index().z;

  // shared memory that will be used to load pixels temporarily
  __shared__ float buf1[SY][SSX];
  __shared__ float buf2[SY][SSX];
  __shared__ float buf3[CY][CCX];

  for (int i = 0; i < CH; ++i) {
    // load into shared
    load_into_shared(buf1, img1, CH, H, W, i);
    block.sync();

    // calculate mu1
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf1, buf3, H, W);
    block.sync();
    float mu1 = do_separable_conv_y(buf3, H, W);
    block.sync();

    // calculate sigma1_sq
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf1, buf3, H, W, true);
    block.sync();
    float sigma1_sq = do_separable_conv_y(buf3, H, W) - mu1 * mu1;
    block.sync();

    // calculate mu2
    load_into_shared(buf2, img2, CH, H, W, i);
    block.sync();
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf2, buf3, H, W);
    block.sync();
    float mu2 = do_separable_conv_y(buf3, H, W);
    block.sync();

    // calculate sigma2_sq
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf2, buf3, H, W, true);
    block.sync();
    float sigma2_sq = do_separable_conv_y(buf3, H, W) - mu2 * mu2;
    block.sync();

    // calculate sigma12
    multiply_shared_mem(buf1, buf2);
    block.sync();
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf1, buf3, H, W);
    block.sync();
    float sigma12 = do_separable_conv_y(buf3, H, W) - mu1 * mu2;
    block.sync();

    float mu1_sq = mu1 * mu1;
    float mu2_sq = mu2 * mu2;
    float mu1_mu2 = mu1 * mu2;
    float C = (2.0f * mu1_mu2 + C1);
    float D = (2.0f * sigma12 + C2);
    float A = (mu1_sq + mu2_sq + C1);
    float B = (sigma1_sq + sigma2_sq + C2);
    float m = (C * D) / (A * B);
    if (pix_x < W && pix_y < H) {
      const int global_idx = batch * CH * num_pix + i * num_pix + pix_id;
      ssim_map[global_idx] = m;

      if (dm_dmu1) {
        dm_dmu1[global_idx] = (
          (mu2 * 2.0f * D) / (A * B)
          -(mu2 * 2.0f * C) / (A * B)
          -(mu1 * 2.0f * C * D) / ( A * A * B)
          +(mu1 * 2.0f * C * D) / (A * B * B)
        );
        dm_dsigma1_sq[global_idx] = ((-C * D) / (A * B * B));
        dm_dsigma12[global_idx] = ((2 * C) / (A * B));
      }
    }
  }
}

template <typename TargetT>
__global__ void fusedssim_backwardCUDA(
  int H,
  int W,
  int CH,
  float C1,
  float C2,
  float* img1,
  const TargetT* img2,
  const float *upstream_gradient,
  float *dL_dimg1,
  float* dm_dmu1,
  float* dm_dsigma1_sq,
  float* dm_dsigma12,
  float ssim_weight,
  int batches
)
{
  auto block = cg::this_thread_block();
  const int pix_y = block.group_index().y * BY + block.thread_index().y;
  const int pix_x = block.group_index().x * BX + block.thread_index().x;
  const int pix_id = pix_y * W + pix_x;
  const int num_pix = H * W;
  const int batch = block.group_index().z;

  // shared memory that will be used to load pixels temporarily
  __shared__ float buf1[SY][SSX];
  __shared__ float buf2[SY][SSX];
  __shared__ float buf3[CY][CCX];

  for (int i = 0; i < CH; ++i) {
    float dL_dpix = 0.0f;
    float tmp = 0.0f;
    float pix1 = get_pix_value(img1, batch, i, pix_y, pix_x, CH, H, W);
    float pix2 = get_pix_value(img2, batch, i, pix_y, pix_x, CH, H, W);
    load_loss_chain(buf1, upstream_gradient, ssim_weight, batches, CH, H, W);

    // gradient from mu1
    load_into_shared(buf2, dm_dmu1, CH, H, W, i);
    block.sync();
    multiply_shared_mem(buf2, buf1);
    block.sync();
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf2, buf3, H, W);
    block.sync();
    tmp = do_separable_conv_y(buf3, H, W);
    block.sync();
    dL_dpix += tmp;

    // gradient from sigma1_sq
    load_into_shared(buf2, dm_dsigma1_sq, CH, H, W, i);
    block.sync();
    multiply_shared_mem(buf2, buf1);
    block.sync();
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf2, buf3, H, W);
    block.sync();
    tmp = pix1 * 2.0f * do_separable_conv_y(buf3, H, W);
    block.sync();
    dL_dpix += tmp;

    // gradient from sigma12
    load_into_shared(buf2, dm_dsigma12, CH, H, W, i);
    block.sync();
    multiply_shared_mem(buf2, buf1);
    block.sync();
    flush_conv_scratch(buf3);
    block.sync();
    do_separable_conv_x(buf2, buf3, H, W);
    block.sync();
    tmp = pix2 * do_separable_conv_y(buf3, H, W);
    block.sync();
    dL_dpix += tmp;

    if (pix_x < W && pix_y < H) {
      const int global_idx = batch * CH * num_pix + i * num_pix + pix_id;
      const float sign = pix1 > pix2 ? 1.0f : (pix1 < pix2 ? -1.0f : 0.0f);
      dL_dimg1[global_idx] = dL_dpix +
          (1 - ssim_weight) * sign * upstream_gradient[0] / (batches * CH * H * W);
    }
  }
}



namespace {
__device__ float warp_sum(float value) {
  for (int offset = 16; offset > 0; offset /= 2)
    value += __shfl_down_sync(0xffffffff, value, offset);
  return value;
}

// NCHW L1 and channel-averaged SSIM are reduced together, without creating
// abs(image-target) or a separate full-resolution L1-gradient tensor.
template <typename TargetT>
__global__ void combined_mean(const float* image, const TargetT* target,
    const float* ssim_map, float weight, float* partials,
    int samples, int plane_pixels, int channels) {
  float l1_sum = 0, loss_sum = 0;
  for (int pixel = blockIdx.x * blockDim.x + threadIdx.x; pixel < samples;
       pixel += blockDim.x * gridDim.x) {
    const int batch = pixel / plane_pixels;
    const int location = pixel % plane_pixels;
    float l1 = 0, similarity = 0;
    for (int channel = 0; channel < channels; ++channel) {
      const int index = (batch * channels + channel) * plane_pixels + location;
      l1 += fabsf(image[index] - sample_value(target, index));
      similarity += ssim_map[index];
    }
    l1 /= channels;
    l1_sum += l1;
    loss_sum += (1 - weight) * l1 + weight * (1 - similarity / channels);
  }
  l1_sum = warp_sum(l1_sum);
  loss_sum = warp_sum(loss_sum);
  __shared__ float values[2][8];
  const int lane = threadIdx.x % 32, warp = threadIdx.x / 32;
  if (lane == 0) { values[0][warp] = loss_sum; values[1][warp] = l1_sum; }
  __syncthreads();
  if (warp == 0) {
    loss_sum = warp_sum(lane < 8 ? values[0][lane] : 0);
    l1_sum = warp_sum(lane < 8 ? values[1][lane] : 0);
    if (lane == 0) { partials[blockIdx.x * 2] = loss_sum; partials[blockIdx.x * 2 + 1] = l1_sum; }
  }
}

__global__ void finish_mean(const float* partials, float* scalars, int blocks, int samples) {
  float loss = 0, l1 = 0;
  for (int block = threadIdx.x; block < blocks; block += blockDim.x) {
    loss += partials[block * 2]; l1 += partials[block * 2 + 1];
  }
  loss = warp_sum(loss); l1 = warp_sum(l1);
  __shared__ float values[2][8];
  const int lane = threadIdx.x % 32, warp = threadIdx.x / 32;
  if (lane == 0) { values[0][warp] = loss; values[1][warp] = l1; }
  __syncthreads();
  if (warp == 0) {
    loss = warp_sum(lane < 8 ? values[0][lane] : 0);
    l1 = warp_sum(lane < 8 ? values[1][lane] : 0);
    if (lane == 0) { scalars[0] = loss / samples; scalars[1] = l1 / samples; }
  }
}

void validate(const torch::Tensor& image, const torch::Tensor& target,
              double weight, const std::vector<torch::Tensor>& buffers) {
  TORCH_CHECK(image.is_cuda() && image.scalar_type() == torch::kFloat32 &&
              image.is_contiguous() && image.dim() == 4 && image.numel() > 0,
              "fused_loss_prediction_contract");
  TORCH_CHECK(target.is_cuda() && target.device() == image.device() &&
              target.is_contiguous() && target.sizes() == image.sizes() &&
              (target.scalar_type() == torch::kFloat32 || target.scalar_type() == torch::kUInt8),
              "fused_loss_target_contract");
  TORCH_CHECK(std::isfinite(weight) && weight >= 0 && weight <= 1, "fused_loss_weight_contract");
  TORCH_CHECK(buffers.size() == 7, "fused_loss_workspace_contract");
  for (const auto& buffer : buffers)
    TORCH_CHECK(buffer.is_cuda() && buffer.device() == image.device() &&
                buffer.is_contiguous() && buffer.scalar_type() == torch::kFloat32,
                "fused_loss_workspace_tensor_contract");
  TORCH_CHECK(image.numel() < 2147483647, "fused_loss_image_too_large");
}
} // namespace

torch::Tensor fused_loss_forward(torch::Tensor image, torch::Tensor target, double weight,
                                 std::vector<torch::Tensor> buffers) {
  validate(image, target, weight, buffers);
  const c10::cuda::CUDAGuard guard(image.device());
  const int n = image.size(0), c = image.size(1), h = image.size(2), w = image.size(3);
  const int samples = n * h * w, blocks = std::min((samples + 255) / 256, 1024);
  for (int index : {0, 1, 2, 6})
    TORCH_CHECK(buffers[index].numel() >= image.numel(), "fused_loss_workspace_too_small");
  TORCH_CHECK(buffers[3].numel() >= image.numel() && buffers[4].numel() >= blocks * 2 &&
              buffers[5].numel() >= 2, "fused_loss_reduction_workspace_too_small");
  const dim3 grid((w + BX - 1) / BX, (h + BY - 1) / BY, n);
  const dim3 block(BX, BY);
  const auto stream = at::cuda::getCurrentCUDAStream(image.get_device());
  if (target.scalar_type() == torch::kUInt8) {
    fusedssimCUDA<uint8_t><<<grid, block, 0, stream>>>(
        h, w, c, .0001f, .0009f, image.data_ptr<float>(), target.data_ptr<uint8_t>(),
        buffers[3].data_ptr<float>(), buffers[0].data_ptr<float>(),
        buffers[1].data_ptr<float>(), buffers[2].data_ptr<float>());
    combined_mean<uint8_t><<<blocks, 256, 0, stream>>>(image.data_ptr<float>(),
        target.data_ptr<uint8_t>(), buffers[3].data_ptr<float>(), weight,
        buffers[4].data_ptr<float>(), samples, h * w, c);
  } else {
    fusedssimCUDA<float><<<grid, block, 0, stream>>>(
        h, w, c, .0001f, .0009f, image.data_ptr<float>(), target.data_ptr<float>(),
        buffers[3].data_ptr<float>(), buffers[0].data_ptr<float>(),
        buffers[1].data_ptr<float>(), buffers[2].data_ptr<float>());
    combined_mean<float><<<blocks, 256, 0, stream>>>(image.data_ptr<float>(),
        target.data_ptr<float>(), buffers[3].data_ptr<float>(), weight,
        buffers[4].data_ptr<float>(), samples, h * w, c);
  }
  finish_mean<<<1, 256, 0, stream>>>(buffers[4].data_ptr<float>(), buffers[5].data_ptr<float>(), blocks, samples);
  C10_CUDA_KERNEL_LAUNCH_CHECK();
  return buffers[5];
}

torch::Tensor fused_loss_backward(torch::Tensor image, torch::Tensor target, double weight,
                                  torch::Tensor upstream_gradient,
                                  std::vector<torch::Tensor> buffers) {
  validate(image, target, weight, buffers);
  TORCH_CHECK(upstream_gradient.is_cuda() && upstream_gradient.device() == image.device() &&
              upstream_gradient.scalar_type() == torch::kFloat32 &&
              upstream_gradient.is_contiguous() && upstream_gradient.numel() == 1,
              "fused_loss_upstream_gradient_contract");
  const c10::cuda::CUDAGuard guard(image.device());
  const int n = image.size(0), c = image.size(1), h = image.size(2), w = image.size(3);
  const dim3 grid((w + BX - 1) / BX, (h + BY - 1) / BY, n);
  const dim3 block(BX, BY);
  const auto stream = at::cuda::getCurrentCUDAStream(image.get_device());
  if (target.scalar_type() == torch::kUInt8)
    fusedssim_backwardCUDA<uint8_t><<<grid, block, 0, stream>>>(
        h, w, c, .0001f, .0009f, image.data_ptr<float>(), target.data_ptr<uint8_t>(),
        upstream_gradient.data_ptr<float>(), buffers[6].data_ptr<float>(),
        buffers[0].data_ptr<float>(), buffers[1].data_ptr<float>(), buffers[2].data_ptr<float>(),
        weight, n);
  else
    fusedssim_backwardCUDA<float><<<grid, block, 0, stream>>>(
        h, w, c, .0001f, .0009f, image.data_ptr<float>(), target.data_ptr<float>(),
        upstream_gradient.data_ptr<float>(), buffers[6].data_ptr<float>(),
        buffers[0].data_ptr<float>(), buffers[1].data_ptr<float>(), buffers[2].data_ptr<float>(),
        weight, n);
  C10_CUDA_KERNEL_LAUNCH_CHECK();
  return buffers[6].view_as(image);
}

