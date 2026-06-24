/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
*/

/*
 *	cudaGrayscale.cu  --  implementation of the 16 grayscale conversions
 *	====================================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals"):
 *		This is the first .cu file in the reading order from PLAN.md.
 *		Every kernel that follows (cudaResize, cudaRGB, cudaYUV-*,
 *		tensorConvert, ...) is some variation of the dispatch and
 *		kernel-launch pattern that lives here, so once this one is
 *		understood the rest of Stage 2 is mostly type-juggling on top.
 *
 *	Pipeline (`cudaXxxToYyy` -> `launchXxx<...>` -> `__global__ Xxx`):
 *
 *		[public C-linkage entry]   cuda<src>To<dst>(...)
 *		     |
 *		     | branches on bool swapRedBlue to pick a template
 *		     | instantiation of...
 *		     v
 *		[static templated launcher] launchRGBToGray<T_in, T_out, isBGR>
 *		     |
 *		     | null-checks args, computes a 2D grid via iDivUp(),
 *		     | then launches...
 *		     v
 *		[templated kernel]          __global__ RGBToGray<T_in, T_out, isBGR>
 *		     |
 *		     | one thread per output pixel; bounds-checked; reads one
 *		     | T_in, writes one T_out
 *		     v
 *		[return]                    CUDA(cudaGetLastError())  // async!
 *
 *	Why this layered shape:
 *		- The public entries are C-linkage (well, extern-by-default
 *		  C++) so they can be declared in a regular header and used
 *		  from .cpp files without any template plumbing leaking out.
 *		- The static launcher is the SINGLE place that computes the
 *		  grid/block dims, so changing the block shape only needs to
 *		  happen once per family.
 *		- The kernel is templated on T_in / T_out so the SAME source
 *		  covers every (src format, dst format) combo via explicit
 *		  instantiation at the call site.
 *
 *	Four kernel families in this file:
 *		1. RGBToGray<T_in, T_out, isBGR>          (no rescale)
 *		2. RGBToGray_Norm<T_in, T_out, isBGR>     (with rescale)
 *		3. GrayToRGB<T_in, T_out>                 (no rescale)
 *		4. GrayToRGB_Norm<T_in, T_out>            (with rescale)
 *
 *	Kernel shape (identical across all four):
 *		block = (32, 8, 1)              // 256 threads, warps tile rows
 *		grid  = (iDivUp(W,32), iDivUp(H,8), 1)
 *		early-out on out-of-bounds (W, H not assumed multiples of block)
 *		pixel index = y * W + x         // tightly packed, no row stride
 *
 *	BT.601 luma coefficients (see RGB2Gray below):
 *		Y = 0.2989*R + 0.5870*G + 0.1140*B
 *		Matches the classic SDTV / JPEG luma weights. The library does
 *		NOT do gamma correction first -- callers wanting linear-light
 *		luma must linearize beforehand.
 *
 *	swapRedBlue handling:
 *		The kernels are templated on a compile-time `bool isBGR`, so
 *		the branch is resolved at template instantiation -- no runtime
 *		predication inside the kernel. The public entry just picks
 *		`true` or `false` for the template arg based on the
 *		swapRedBlue parameter.
 *
 *	Why Gray8ToGray32 lives in the Gray->RGB family:
 *		The kernel writes `make_vec<T_out>(px, px, px, 255)`. For a
 *		1-channel T_out (uint8_t or float) the `make_vec` specialization
 *		simply returns `px` and ignores the other three arguments (see
 *		cudaVector.h). So the same kernel handles Gray->Gray (precision
 *		change) and Gray->RGB(A) at no extra source cost.
 *
 *	rescale() macro quirk -- READ THIS BEFORE EDITING:
 *		`#define rescale(v) ((v - min_pixel_value) * scaling_factor)`
 *		is defined inside the body of the RGBToGray_Norm kernel near
 *		line ~184, BUT it is also referenced from inside the
 *		GrayToRGB_Norm kernel further down. C/C++ macros are
 *		preprocessor-level and persist from the `#define` line until
 *		either `#undef` or end-of-translation-unit. Both kernels
 *		happen to define identically-named `min_pixel_value` and
 *		`scaling_factor` parameters, so the macro expands correctly
 *		in both bodies -- but visually it looks like a use-before-def.
 *		Don't "fix" it by moving the `#define` without first
 *		re-checking the call sites.
 *
 *	Async caveat:
 *		Every public entry returns `CUDA(cudaGetLastError())` after
 *		the launch. Launches are asynchronous, so a `cudaSuccess`
 *		return only means "the launch was accepted" -- a kernel that
 *		later faults will surface on the next stream/device sync.
*/

#include "cudaGrayscale.h"
#include "cudaVector.h"


//-----------------------------------------------------------------------------------
// RGB to Grayscale
//-----------------------------------------------------------------------------------
// Plain (non-normalized) path. Reads one pixel of T_in, computes the BT.601 luma,
// writes one pixel of T_out. isBGR selects channel order at compile time.

// BT.601 luma from a packed float3 of (R, G, B) components.
// Coefficients are the standard SDTV / JPEG weights; no gamma correction.
inline __device__ float RGB2Gray(
	float3 rgb
) {
	return float(rgb.x) * 0.2989f + float(rgb.y) * 0.5870f + float(rgb.z) * 0.1140f;
}

template <typename T_in, typename T_out, bool isBGR>
__global__ void RGBToGray(
	T_in* srcImage,
	T_out* dstImage,
	int width,
	int height
) {
	const int x = (blockIdx.x * blockDim.x) + threadIdx.x;
	const int y = (blockIdx.y * blockDim.y) + threadIdx.y;

	const int pixel = y * width + x;

	if (x >= width)
		return;

	if (y >= height)
		return;

	const T_in px = srcImage[pixel];

	// isBGR is a template bool, so the branch is dead-code eliminated
	// at instantiation -- no runtime predication per thread.
	if (isBGR)
		dstImage[pixel] = RGB2Gray(make_float3(px.z, px.y, px.x));
	else
		dstImage[pixel] = RGB2Gray(make_float3(px.x, px.y, px.z));
}

// Single per-family launcher: validates inputs, sizes the 2D grid,
// launches the kernel, returns the launch-time error (if any).
template <typename T_in, typename T_out, bool isBGR>
static cudaError_t launchRGBToGray(
	T_in* srcDev,
	T_out* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	if (!srcDev || !dstDev)
		return cudaErrorInvalidDevicePointer;

	if (width == 0 || height == 0)
		return cudaErrorInvalidValue;

	const dim3 blockDim(32, 8, 1);
	const dim3 gridDim(iDivUp(width, blockDim.x), iDivUp(height, blockDim.y), 1);

	RGBToGray<T_in, T_out, isBGR><<<gridDim, blockDim, 0, stream>>>(srcDev, dstDev, width, height);

	return CUDA(cudaGetLastError());
}

// cudaRGB8ToGray8 (uchar3 -> uint8)
cudaError_t cudaRGB8ToGray8(
	uchar3* srcDev,
	uint8_t* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<uchar3, uint8_t, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<uchar3, uint8_t, false>(srcDev, dstDev, width, height, stream);
}

// cudaRGBA8ToGray8 (uchar4 -> uint8)
cudaError_t cudaRGBA8ToGray8(
	uchar4* srcDev,
	uint8_t* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<uchar4, uint8_t, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<uchar4, uint8_t, false>(srcDev, dstDev, width, height, stream);
}

// cudaRGB8ToGray32 (uchar3 -> float)
cudaError_t cudaRGB8ToGray32(
	uchar3* srcDev,
	float* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<uchar3, float, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<uchar3, float, false>(srcDev, dstDev, width, height, stream);
}

// cudaRGBA8ToGray32 (uchar4 -> float)
cudaError_t cudaRGBA8ToGray32(
	uchar4* srcDev,
	float* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<uchar4, float, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<uchar4, float, false>(srcDev, dstDev, width, height, stream);
}

// cudaRGB32ToGray32 (float3 -> float)
cudaError_t cudaRGB32ToGray32(
	float3* srcDev,
	float* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<float3, float, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<float3, float, false>(srcDev, dstDev, width, height, stream);
}

// cudaRGBA32ToGray32 (float4 -> float)
cudaError_t cudaRGBA32ToGray32(
	float4* srcDev,
	float* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray<float4, float, true>(srcDev, dstDev, width, height, stream);
	else
		return launchRGBToGray<float4, float, false>(srcDev, dstDev, width, height, stream);
}


//-----------------------------------------------------------------------------------
// RGB to Grayscale (normalized)
//-----------------------------------------------------------------------------------
// Same shape as the plain RGB->Gray kernel, but the float-side input is rescaled
// from [inputRange.lo, inputRange.hi] to [0, 255] before the luma weights are
// applied. The launcher converts the pixelRange to a (min, multiplier) pair the
// kernel can apply with one subtract + one multiply per channel.
//
// NOTE the `#define rescale(v)` below is referenced AGAIN from inside
// GrayToRGB_Norm further down -- macros persist until end-of-TU. See the
// file-header block for details.

template <typename T_in, typename T_out, bool isBGR>
__global__ void RGBToGray_Norm(
	T_in* srcImage,
	T_out* dstImage,
	int width,
	int height,
	float min_pixel_value,
	float scaling_factor
) {
	const int x = (blockIdx.x * blockDim.x) + threadIdx.x;
	const int y = (blockIdx.y * blockDim.y) + threadIdx.y;

	const int pixel = y * width + x;

	if (x >= width)
		return;

	if (y >= height)
		return;

	// NOTE: this #define persists for the rest of the translation
	// unit -- GrayToRGB_Norm below relies on it. Don't relocate
	// without checking call sites.
	#define rescale(v) ((v - min_pixel_value) * scaling_factor)

	const T_in px = srcImage[pixel];

	if (isBGR)
		dstImage[pixel] = RGB2Gray(make_float3(rescale(px.z), rescale(px.y), rescale(px.x)));
	else
		dstImage[pixel] = RGB2Gray(make_float3(rescale(px.x), rescale(px.y), rescale(px.z)));
}

template <typename T_in, typename T_out, bool isBGR>
static cudaError_t launchRGBToGray_Norm(
	T_in* srcDev,
	T_out* dstDev,
	size_t width,
	size_t height,
	const float2& inputRange,
	cudaStream_t stream
) {
	if (!srcDev || !dstDev)
		return cudaErrorInvalidDevicePointer;

	if (width == 0 || height == 0)
		return cudaErrorInvalidValue;

	// Precompute the affine to map [inputRange.x, inputRange.y] -> [0, 255]:
	//   out = (v - inputRange.x) * (255 / (inputRange.y - inputRange.x))
	const float multiplier = 255.0f / (inputRange.y - inputRange.x);

	const dim3 blockDim(32, 8, 1);
	const dim3 gridDim(iDivUp(width, blockDim.x), iDivUp(height, blockDim.y), 1);

	RGBToGray_Norm<T_in, T_out, isBGR>
		<<<gridDim, blockDim, 0, stream>>>(srcDev, dstDev, width, height, inputRange.x, multiplier);

	return CUDA(cudaGetLastError());
}

// cudaRGB32ToGray8 (float3 -> uint8)
cudaError_t cudaRGB32ToGray8(
	float3* srcDev,
	uint8_t* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	const float2& inputRange,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray_Norm<float3, uint8_t, true>(
		    srcDev,
		    dstDev,
		    width,
		    height,
		    inputRange,
		    stream
		);
	else
		return launchRGBToGray_Norm<float3, uint8_t, false>(
		    srcDev,
		    dstDev,
		    width,
		    height,
		    inputRange,
		    stream
		);
}

// cudaRGBA32ToGray8 (float4 -> uint8)
cudaError_t cudaRGBA32ToGray8(
	float4* srcDev,
	uint8_t* dstDev,
	size_t width,
	size_t height,
	bool swapRedBlue,
	const float2& inputRange,
	cudaStream_t stream
) {
	if (swapRedBlue)
		return launchRGBToGray_Norm<float4, uint8_t, true>(
		    srcDev,
		    dstDev,
		    width,
		    height,
		    inputRange,
		    stream
		);
	else
		return launchRGBToGray_Norm<float4, uint8_t, false>(
		    srcDev,
		    dstDev,
		    width,
		    height,
		    inputRange,
		    stream
		);
}


//-----------------------------------------------------------------------------------
// Grayscale to RGB
//-----------------------------------------------------------------------------------
// One scalar in, broadcast into three (or four) output channels. The use of
// `make_vec<T_out>(px, px, px, 255)` lets the same kernel cover Gray->RGB,
// Gray->RGBA, AND Gray->Gray (precision change), because the make_vec
// specialization for scalar T_out just returns its first argument and ignores
// the rest. See cudaVector.h for the make_vec dispatch table.

template <typename T_in, typename T_out>
__global__ void GrayToRGB(
	T_in* srcImage,
	T_out* dstImage,
	int width,
	int height
) {
	const int x = (blockIdx.x * blockDim.x) + threadIdx.x;
	const int y = (blockIdx.y * blockDim.y) + threadIdx.y;

	const int pixel = y * width + x;

	if (x >= width)
		return;

	if (y >= height)
		return;

	const T_in px = srcImage[pixel];

	// Default alpha = 255 (uchar4) or ignored (uchar3 / float3 / scalar);
	// see cudaVector.h::make_vec for the per-T_out specialization.
	dstImage[pixel] = make_vec<T_out>(px, px, px, 255);
}

template <typename T_in, typename T_out>
cudaError_t launchGrayToRGB(
	T_in* srcDev,
	T_out* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	if (!srcDev || !dstDev)
		return cudaErrorInvalidDevicePointer;

	if (width == 0 || height == 0)
		return cudaErrorInvalidValue;

	const dim3 blockDim(32, 8, 1);
	const dim3 gridDim(iDivUp(width, blockDim.x), iDivUp(height, blockDim.y), 1);

	GrayToRGB<T_in, T_out><<<gridDim, blockDim, 0, stream>>>(srcDev, dstDev, width, height);

	return CUDA(cudaGetLastError());
}

// cudaGray8ToRGB8 (uint8 -> uchar3)
cudaError_t cudaGray8ToRGB8(
	uint8_t* srcDev,
	uchar3* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<uint8_t, uchar3>(srcDev, dstDev, width, height, stream);
}

// cudaGray8ToRGBA8 (uint8 -> uchar4)
cudaError_t cudaGray8ToRGBA8(
	uint8_t* srcDev,
	uchar4* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<uint8_t, uchar4>(srcDev, dstDev, width, height, stream);
}

// cudaGray8ToRGB32 (uint8 -> float3)
cudaError_t cudaGray8ToRGB32(
	uint8_t* srcDev,
	float3* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<uint8_t, float3>(srcDev, dstDev, width, height, stream);
}

// cudaGray8ToRGBA32 (uint8 -> float4)
cudaError_t cudaGray8ToRGBA32(
	uint8_t* srcDev,
	float4* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<uint8_t, float4>(srcDev, dstDev, width, height, stream);
}

// cudaGray32ToRGB32 (float -> float3)
cudaError_t cudaGray32ToRGB32(
	float* srcDev,
	float3* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<float, float3>(srcDev, dstDev, width, height, stream);
}

// cudaGray32ToRGBA32 (float -> float4)
cudaError_t cudaGray32ToRGBA32(
	float* srcDev,
	float4* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<float, float4>(srcDev, dstDev, width, height, stream);
}

// cudaGray8ToGray32 (uint8 -> float)
// Uses the GrayToRGB launcher because make_vec<float>(px, px, px, 255)
// degenerates to just `px` -- see cudaVector.h::make_vec<float>.
cudaError_t cudaGray8ToGray32(
	uint8_t* srcDev,
	float* dstDev,
	size_t width,
	size_t height,
	cudaStream_t stream
) {
	return launchGrayToRGB<uint8_t, float>(srcDev, dstDev, width, height, stream);
}


//-----------------------------------------------------------------------------------
// Grayscale to RGB (normalized)
//-----------------------------------------------------------------------------------
// Same broadcast pattern as GrayToRGB, but the scalar is first rescaled from
// the caller-supplied float range into [0, 255]. Reuses the `rescale(v)` macro
// defined inside RGBToGray_Norm above -- that macro is still in scope here
// because C macros persist until end-of-TU.

template <typename T_in, typename T_out>
__global__ void GrayToRGB_Norm(
	T_in* srcImage,
	T_out* dstImage,
	int width,
	int height,
	float min_pixel_value,
	float scaling_factor
) {
	const int x = (blockIdx.x * blockDim.x) + threadIdx.x;
	const int y = (blockIdx.y * blockDim.y) + threadIdx.y;

	const int pixel = y * width + x;

	if (x >= width)
		return;

	if (y >= height)
		return;

	// `rescale` is the macro defined inside RGBToGray_Norm above; it
	// expands to ((srcImage[pixel] - min_pixel_value) * scaling_factor),
	// and the names line up because this kernel has identically-named
	// parameters. See the file-header block before relocating.
	const T_in px = rescale(srcImage[pixel]);
	dstImage[pixel] = make_vec<T_out>(px, px, px, 255);
}

template <typename T_in, typename T_out>
static cudaError_t launchGrayToRGB_Norm(
	T_in* srcDev,
	T_out* dstDev,
	size_t width,
	size_t height,
	const float2& inputRange,
	cudaStream_t stream
) {
	if (!srcDev || !dstDev)
		return cudaErrorInvalidDevicePointer;

	if (width == 0 || height == 0)
		return cudaErrorInvalidValue;

	const float multiplier = 255.0f / (inputRange.y - inputRange.x);

	const dim3 blockDim(32, 8, 1);
	const dim3 gridDim(iDivUp(width, blockDim.x), iDivUp(height, blockDim.y), 1);

	GrayToRGB_Norm<T_in, T_out>
		<<<gridDim, blockDim, 0, stream>>>(srcDev, dstDev, width, height, inputRange.x, multiplier);

	return CUDA(cudaGetLastError());
}

// cudaGray32ToRGB8 (float-> uchar3)
cudaError_t cudaGray32ToRGB8(
	float* srcDev,
	uchar3* dstDev,
	size_t width,
	size_t height,
	const float2& inputRange,
	cudaStream_t stream
) {
	return launchGrayToRGB_Norm<float, uchar3>(srcDev, dstDev, width, height, inputRange, stream);
}

// cudaGray32ToRGBA8 (float-> uchar4)
cudaError_t cudaGray32ToRGBA8(
	float* srcDev,
	uchar4* dstDev,
	size_t width,
	size_t height,
	const float2& inputRange,
	cudaStream_t stream
) {
	return launchGrayToRGB_Norm<float, uchar4>(srcDev, dstDev, width, height, inputRange, stream);
}

// cudaGray32ToGray8 (float -> uint8)
// Same shape as Gray32ToRGB8 -- the 1-channel make_vec specialization
// reduces `make_vec<uint8_t>(px, px, px, 255)` to just `px`.
cudaError_t cudaGray32ToGray8(
	float* srcDev,
	uint8_t* dstDev,
	size_t width,
	size_t height,
	const float2& inputRange,
	cudaStream_t stream
) {
	return launchGrayToRGB_Norm<float, uint8_t>(srcDev, dstDev, width, height, inputRange, stream);
}
