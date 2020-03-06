/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#define EIGEN_USE_GPU

#include <stdio.h>

#include "third_party/eigen3/Eigen/Core"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/kernels/relu_op_functor.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"
#include "tensorflow/core/util/gpu_launch_config.h"

#if TENSORFLOW_USE_ROCM
#include "rocm/include/hip/hip_fp16.h"
typedef __half2 half2;
#endif

namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

namespace functor {

#if GOOGLE_CUDA
// TODO(rocm): disabling this code on the ROCm platform since the references
// to `half2` are leading to compile errors.

// This kernel computes ReluGrad by processing one half2, two fp16, at a time.
// It effectively does: backdrops = (feature > 0) ? gradient : 0
// It also tries to use native half2 primitives as much as possible.
__global__ void ReluGradHalfKernel(const Eigen::half* gradient,
                                   const Eigen::half* feature,
                                   Eigen::half* backprop, int32 count) {
  int32 half2_count = count >> 1;
  int32 index = blockIdx.x * blockDim.x + threadIdx.x;
  const int32 total_device_threads = gridDim.x * blockDim.x;

  while (index < half2_count) {
    // The fast branch.
    // One half2, two fp16, is fetched and processed at a time.
    half2 gradient_h2 = reinterpret_cast<const half2*>(gradient)[index];
    half2 feature_h2 = reinterpret_cast<const half2*>(feature)[index];
    half2* p_backprop_h2 = reinterpret_cast<half2*>(backprop) + index;

#if __CUDA_ARCH__ >= 530
    // Fast path, when half2 primitives are available.
    const half2 kZeroH2 = __float2half2_rn(0.f);
    // mask = (feature > 0)
    half2 mask_h2 = __hgt2(feature_h2, kZeroH2);
    // backprop = mask * gradient
    half2 backprop_h2 = __hmul2(mask_h2, gradient_h2);
#else
    // Fall back: convert half2 to float2 for processing.
    float2 feature_f2 = __half22float2(feature_h2);
    float2 gradient_f2 = __half22float2(gradient_h2);
    float2 backprop_f2 = make_float2((feature_f2.x > 0) ? gradient_f2.x : 0,
                                     (feature_f2.y > 0) ? gradient_f2.y : 0);
    // Convert back to half2.
    half2 backprop_h2 = __float22half2_rn(backprop_f2);
#endif

    // Write back the result.
    *p_backprop_h2 = backprop_h2;

    index += total_device_threads;
  }

  if ((count & 0x1) == 1 && index == half2_count) {
    // If the total number of the elements is odd, process the last element.
    Eigen::half grad_h = gradient[count - 1];
    Eigen::half feature_h = feature[count - 1];

    float grad_f = static_cast<float>(grad_h);
    float feature_f = static_cast<float>(feature_h);
    float backprop_f = (feature_f > 0) ? grad_f : 0;

    Eigen::half backprop_h(backprop_f);
    backprop[count - 1] = backprop_h;
  }
}

template <typename Device>
struct ReluGrad<Device, Eigen::half> {
  // Computes ReluGrad backprop.
  //
  // gradient: gradient backpropagated to the Relu op.
  // feature: either the inputs that were passed to the Relu, or its outputs
  //           (using either one yields the same result here).
  // backprop: gradient to backpropagate to the Relu inputs.
  void operator()(const Device& d,
                  typename TTypes<Eigen::half>::ConstTensor gradient,
                  typename TTypes<Eigen::half>::ConstTensor feature,
                  typename TTypes<Eigen::half>::Tensor backprop) {
    // NOTE: When the activation is exactly zero, we do not propagate the
    // associated gradient value. This allows the output of the Relu to be used,
    // as well as its input.
    int32 count = gradient.size();
    if (count == 0) return;
    int32 half2_count = Eigen::divup(count, 2);
    constexpr int32 kThreadInBlock = 512;
    GpuLaunchConfig config = GetGpuLaunchConfigFixedBlockSize(
        half2_count, d, ReluGradHalfKernel, 0, kThreadInBlock);
    TF_CHECK_OK(GpuLaunchKernel(
        ReluGradHalfKernel, config.block_count, config.thread_per_block, 0,
        d.stream(), gradient.data(), feature.data(), backprop.data(), count));
  }
};
#endif  // GOOGLE_CUDA

#if GOOGLE_CUDA
__global__ void Relu_int8x4_kernel(int vect_count, const int32* input,
                                   int32* output) {
  GPU_1D_KERNEL_LOOP(index, vect_count) {
    output[index] = __vmaxs4(input[index], 0);
  }
}

// Functor used by ReluOp to do the computations.
template <typename Device>
struct Relu<Device, qint8> {
  // Computes Relu activation of 'input' containing int8 elements, whose buffer
  // size should be a multiple of 4, and aligned to an int32* boundary.
  // (Alignment should be guaranteed by the GPU tensor allocator).
  // 'output' should have the same size as 'input'.
  void operator()(const Device& d, typename TTypes<qint8>::ConstTensor input,
                  typename TTypes<qint8>::Tensor output) {
    int32 count = input.size();
    if (count == 0) return;

    int32 vect_count = Eigen::divup(count, 4);
    constexpr int32 kThreadInBlock = 512;
    GpuLaunchConfig config = GetGpuLaunchConfigFixedBlockSize(
        vect_count, d, Relu_int8x4_kernel, 0, kThreadInBlock);
    TF_CHECK_OK(GpuLaunchKernel(
        Relu_int8x4_kernel, config.block_count, config.thread_per_block, 0,
        d.stream(), vect_count, reinterpret_cast<const int32*>(input.data()),
        reinterpret_cast<int32*>(output.data())));
  }
};
#endif  // GOOGLE_CUDA

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
template <class T>
__global__ void GeluKernel(const T* in, T* out, int32 count) {
  int i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i >= count) return;
  const auto scale = static_cast<T>(0.7978845608028654);
  const auto p1 = scale;
  const auto p3 = static_cast<T>(0.044715 * 0.7978845608028654);
  T x = in[i];
  out[i] = 0.5 * x * (1 + tanh(p1 * x + p3 * x * x * x));
}

template <class T>
__global__ void GeluGradKernel(const T* gradient, const T* feature, T* backprop,
                               int32 count) {
  int i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i >= count) return;

  const T p1 = static_cast<T>(0.7978845608028654);
  const T p3 = static_cast<T>(0.044715 * 0.7978845608028654);
  T x = feature[i];
  T z = p1 * x + p3 * x * x * x;
  T g = gradient[i];
  T cz = 1. / cosh(z);
  backprop[i] = static_cast<T>(
      g * 0.5 * (1. + tanh(z) + x * (p1 + 3 * p3 * x * x) * cz * cz));
}

template <>
__global__ void GeluKernel<Eigen::half>(const Eigen::half* _in,
                                        Eigen::half* _out, int32 count) {
  int i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i >= count) return;
  const half* in = reinterpret_cast<const half*>(_in);
  half* out = reinterpret_cast<half*>(_out);
  const float scale = 0.7978845608028654;
  const float p1 = scale;
  const float p3 = 0.044715 * 0.7978845608028654;
  float x = in[i];
  out[i] = 0.5 * x * (1 + tanh(p1 * x + p3 * x * x * x));
}

template <>
__global__ void GeluGradKernel<Eigen::half>(const Eigen::half* _gradient,
                                            const Eigen::half* _feature,
                                            Eigen::half* _backprop,
                                            int32 count) {
  int i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i >= count) return;
  const float scale = 0.7978845608028654;
  const float p1 = scale;
  const float p3 = 0.044715 * 0.7978845608028654;
  const half* gradient = reinterpret_cast<const half*>(_gradient);
  const half* feature = reinterpret_cast<const half*>(_feature);
  half* backprop = reinterpret_cast<half*>(_backprop);
  float x = feature[i];
  float z = p1 * x + p3 * x * x * x;
  float g = gradient[i];
  float cz = 1. / cosh(z);
  backprop[i] = g * 0.5 * (1. + tanh(z) + x * (p1 + 3 * p3 * x * x) * cz * cz);
}

template <typename T>
struct Gelu<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::ConstTensor input,
                  typename TTypes<T>::Tensor output) {
    int32 count = input.size();
    if (count == 0) return;
    constexpr int32 kThreadInBlock = 256;
    TF_CHECK_OK(GpuLaunchKernel(
        GeluKernel<T>, (count + kThreadInBlock - 1) / kThreadInBlock,
        kThreadInBlock, 0, d.stream(), input.data(), output.data(), count));
  }
};

template <typename T>
struct GeluGrad<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::ConstTensor gradient,
                  typename TTypes<T>::ConstTensor feature,
                  typename TTypes<T>::Tensor backprop) {
    int32 count = gradient.size();
    if (count == 0) return;
    constexpr int32 kThreadInBlock = 256;
    TF_CHECK_OK(GpuLaunchKernel(GeluGradKernel<T>,
                                (count + kThreadInBlock - 1) / kThreadInBlock,
                                kThreadInBlock, 0, d.stream(), gradient.data(),
                                feature.data(), backprop.data(), count));
  }
};
#endif

}  // namespace functor

// Definition of the GPU implementations declared in relu_op.cc.
#define DEFINE_GPU_KERNELS(T)                           \
  template struct functor::Relu<GPUDevice, T>;          \
  template struct functor::ReluGrad<GPUDevice, T>;      \
  template struct functor::Relu6<GPUDevice, T>;         \
  template struct functor::Relu6Grad<GPUDevice, T>;     \
  template struct functor::LeakyRelu<GPUDevice, T>;     \
  template struct functor::LeakyReluGrad<GPUDevice, T>; \
  template struct functor::Elu<GPUDevice, T>;           \
  template struct functor::EluGrad<GPUDevice, T>;       \
  template struct functor::Selu<GPUDevice, T>;          \
  template struct functor::SeluGrad<GPUDevice, T>;      \
  template struct functor::Gelu<GPUDevice, T>;          \
  template struct functor::GeluGrad<GPUDevice, T>;

TF_CALL_GPU_NUMBER_TYPES(DEFINE_GPU_KERNELS);
#if GOOGLE_CUDA
template struct functor::Relu<GPUDevice, qint8>;
#endif  // GOOGLE_CUDA

}  // end namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
