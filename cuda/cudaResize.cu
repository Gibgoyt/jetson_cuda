/*
 * Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
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
 *	cudaResize.cu  --  resize-kernel implementation
 *	===============================================
 *
 *	Position in the pipeline (Stage 2, immediately after cudaGrayscale.cu
 *	per PLAN.md). Same overall layered shape as cudaGrayscale -- public
 *	C-linkage entries -> static templated launcher -> templated __global__
 *	kernel -- but with one extra dimension of templating (the filter
 *	mode) and one extra dispatch layer (the void* / imageFormat
 *	overload at the bottom).
 *
 *	Dispatch chain:
 *
 *		[runtime-typed]    cudaResize(void*, ..., imageFormat, filter, stream)
 *		     |
 *		     | switches on imageFormat, casts to a concrete T*, and
 *		     | calls...
 *		     v
 *		[typed C entry]    cudaResize(T*, ..., filter, stream)        // T in
 *		     |                                                        //   {uint8_t, float,
 *		     | unconditional forward to...                            //    uchar3, uchar4,
 *		     v                                                        //    float3, float4}
 *		[static launcher]  launchResize<T>(...)
 *		     |
 *		     | null-checks, computes 8x8 block / iDivUp grid, picks
 *		     | a compile-time filter via the launch_resize() helper
 *		     | macro, calls...
 *		     v
 *		[templated kernel] gpuResize<T, filter>(...)
 *		     |
 *		     | one thread per OUTPUT pixel; delegates per-pixel sampling
 *		     | to cudaFilterPixel<filter>() (defined in cudaFilterMode.cuh)
 *		     v
 *		[return]           CUDA(cudaGetLastError())  // async!
 *
 *	Where the actual sampling math lives:
 *		Nothing in this file knows what "bilinear" or "nearest-neighbour"
 *		means -- gpuResize just calls cudaFilterPixel<filter>(...) once
 *		per output pixel. cudaFilterPixel is a template specialized over
 *		cudaFilterMode in cudaFilterMode.cuh; that's where the source-
 *		coordinate computation, the per-tap fetches, and the lerp live.
 *		Treat this .cu as the "wrap a 2D output grid around any per-pixel
 *		sampler" boilerplate.
 *
 *	Kernel shape (gpuResize):
 *		block = (8, 8)                              // 64 threads
 *		grid  = (iDivUp(outW, 8), iDivUp(outH, 8))
 *		thread coords (x, y) map to OUTPUT pixel coordinates, with an
 *		early-out for (x >= outW || y >= outH).
 *
 *	Why block (8,8) here vs (32,8) in cudaGrayscale.cu:
 *		Bilinear filtering does materially more per-thread work than a
 *		grayscale multiply-add (4 fetches + 4 fmas + 2 lerp setups per
 *		pixel for FILTER_LINEAR, vs 3 muls + 2 adds in cudaGrayscale).
 *		A smaller block keeps register pressure low enough to keep
 *		occupancy reasonable on the more arithmetic-heavy path. The
 *		POINT path is fine either way -- it's the LINEAR path the size
 *		is chosen for.
 *
 *	Downscale -> FILTER_POINT override:
 *		The launcher silently downgrades FILTER_LINEAR to FILTER_POINT
 *		when (outW < inW && outH < inH) -- i.e. ONLY when both axes
 *		shrink. Mixed cases (downscale one axis, upscale the other)
 *		keep FILTER_LINEAR. The header's docstring rounds this off to
 *		"if downscaling, point is used instead", which is technically
 *		misleading; see the inline NOTE on the branch.
 *
 *	The launch_resize() macro:
 *		Used to dispatch on a runtime `cudaFilterMode` value into a
 *		compile-time template argument so the kernel can be specialized
 *		(and the filter branch dead-code-eliminated) per call. The
 *		macro just spells out the long <<<grid, block, 0, stream>>>(...)
 *		argument list once instead of twice; it has no other purpose.
 *
 *	No-op inputs:
 *		There is no fast-path for (inW == outW && inH == outH). The
 *		kernel runs and (for FILTER_POINT) does a 1:1 copy via the
 *		sampler. Saves a branch at the cost of an avoidable launch.
 *
 *	Async caveat (same as the rest of Stage 2):
 *		Every public entry returns CUDA(cudaGetLastError()) immediately
 *		after the launch. The launch being accepted does NOT mean the
 *		kernel succeeded -- a fault will surface on the next sync.
*/

#include "cudaResize.h"
#include "cudaFilterMode.cuh"


//-----------------------------------------------------------------------------------
// Kernel + launcher
//-----------------------------------------------------------------------------------
// One thread per OUTPUT pixel. The per-pixel sampling is delegated entirely to
// cudaFilterPixel<filter>() (in cudaFilterMode.cuh) -- this file is just the
// "wrap a 2D output grid around the sampler" boilerplate.

// gpuResize
template <typename T, cudaFilterMode filter>
__global__ void gpuResize(
    T* input,
    int inputWidth,
    int inputHeight,
    T* output,
    int outputWidth,
    int outputHeight
) {
	const int x = blockIdx.x * blockDim.x + threadIdx.x;
	const int y = blockIdx.y * blockDim.y + threadIdx.y;

	if (x >= outputWidth || y >= outputHeight)
		return;

	// Compile-time `filter` selects the sampler at instantiation time
	// (FILTER_POINT vs FILTER_LINEAR); no per-thread predication.
	output[y * outputWidth + x] =
	    cudaFilterPixel<filter>(input, x, y, inputWidth, inputHeight, outputWidth, outputHeight);
}

// launchResize
template <typename T>
static cudaError_t launchResize(
    T* input,
    size_t inputWidth,
    size_t inputHeight,
    T* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	if (!input || !output)
		return cudaErrorInvalidDevicePointer;

	if (inputWidth == 0 || outputWidth == 0 || inputHeight == 0 || outputHeight == 0)
		return cudaErrorInvalidValue;

	// NOTE: this only forces POINT when BOTH dimensions are shrinking.
	// Mixed cases (W shrinks, H grows -- or vice versa) keep LINEAR.
	// The header docstring rounds this off to "if downscaling, POINT" --
	// not strictly accurate but close enough for most callers.
	if (outputWidth < inputWidth && outputHeight < inputHeight)
		filter = FILTER_POINT;

	// launch kernel
	// Block is 8x8 (64 threads): smaller than the 32x8 used in
	// cudaGrayscale.cu because FILTER_LINEAR does materially more
	// per-thread arithmetic (4 fetches + 4 fmas + 2 lerps vs a single
	// luma multiply-add). Smaller block keeps register pressure low
	// on the LINEAR path; the POINT path doesn't care.
	const dim3 blockDim(8, 8);
	const dim3 gridDim(iDivUp(outputWidth, blockDim.x), iDivUp(outputHeight, blockDim.y));

	// launch_resize() -- runtime cudaFilterMode -> compile-time template
	// argument. The macro is purely to avoid duplicating the long
	// kernel-launch argument list; no other purpose.
	#define launch_resize(filterMode)                                                                  \
		gpuResize<T, filterMode><<<gridDim, blockDim, 0, stream>>>(                                    \
		    input,                                                                                     \
		    inputWidth,                                                                                \
		    inputHeight,                                                                               \
		    output,                                                                                    \
		    outputWidth,                                                                               \
		    outputHeight                                                                               \
		)

	if (filter == FILTER_POINT)
		launch_resize(FILTER_POINT);
	else if (filter == FILTER_LINEAR)
		launch_resize(FILTER_LINEAR);

	return CUDA(cudaGetLastError());
}


//-----------------------------------------------------------------------------------
// Typed C-linkage entries
//-----------------------------------------------------------------------------------
// Six thin overloads -- one per supported pixel type. Each does nothing but
// pin T at the call site so the templated launcher above gets instantiated
// for the right pixel layout. No format-specific logic; resize is per-channel
// and channel meaning (RGB vs BGR, luma vs alpha) is irrelevant here.

// cudaResize (uint8 grayscale)
cudaError_t cudaResize(
    uint8_t* input,
    size_t inputWidth,
    size_t inputHeight,
    uint8_t* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    uint8_t>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}

// cudaResize (float grayscale)
cudaError_t cudaResize(
    float* input,
    size_t inputWidth,
    size_t inputHeight,
    float* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    float>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}

// cudaResize (uchar3)
cudaError_t cudaResize(
    uchar3* input,
    size_t inputWidth,
    size_t inputHeight,
    uchar3* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    uchar3>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}

// cudaResize (uchar4)
cudaError_t cudaResize(
    uchar4* input,
    size_t inputWidth,
    size_t inputHeight,
    uchar4* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    uchar4>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}

// cudaResize (float3)
cudaError_t cudaResize(
    float3* input,
    size_t inputWidth,
    size_t inputHeight,
    float3* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    float3>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}

// cudaResize (float4)
cudaError_t cudaResize(
    float4* input,
    size_t inputWidth,
    size_t inputHeight,
    float4* output,
    size_t outputWidth,
    size_t outputHeight,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	return launchResize<
	    float4>(input, inputWidth, inputHeight, output, outputWidth, outputHeight, filter, stream);
}


//-----------------------------------------------------------------------------------
// Runtime-typed dispatch
//-----------------------------------------------------------------------------------
// Switches on imageFormat, casts input/output to the matching pixel pointer,
// and forwards to one of the six typed overloads above. This is the entry
// streaming code calls when the format is decided by an upstream videoSource
// rather than known at compile time. Unknown formats log a supported-format
// list and return cudaErrorInvalidValue (no kernel launch).

cudaError_t cudaResize(
    void* input,
    size_t inputWidth,
    size_t inputHeight,
    void* output,
    size_t outputWidth,
    size_t outputHeight,
    imageFormat format,
    cudaFilterMode filter,
    cudaStream_t stream
) {
	if (format == IMAGE_RGB8 || format == IMAGE_BGR8)
		return cudaResize(
		    (uchar3*)input,
		    inputWidth,
		    inputHeight,
		    (uchar3*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);
	else if (format == IMAGE_RGBA8 || format == IMAGE_BGRA8)
		return cudaResize(
		    (uchar4*)input,
		    inputWidth,
		    inputHeight,
		    (uchar4*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);
	else if (format == IMAGE_RGB32F || format == IMAGE_BGR32F)
		return cudaResize(
		    (float3*)input,
		    inputWidth,
		    inputHeight,
		    (float3*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);
	else if (format == IMAGE_RGBA32F || format == IMAGE_BGRA32F)
		return cudaResize(
		    (float4*)input,
		    inputWidth,
		    inputHeight,
		    (float4*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);
	else if (format == IMAGE_GRAY8)
		return cudaResize(
		    (uint8_t*)input,
		    inputWidth,
		    inputHeight,
		    (uint8_t*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);
	else if (format == IMAGE_GRAY32F)
		return cudaResize(
		    (float*)input,
		    inputWidth,
		    inputHeight,
		    (float*)output,
		    outputWidth,
		    outputHeight,
		    filter,
		    stream
		);

	LogError(LOG_CUDA "cudaResize() -- invalid image format '%s'\n", imageFormatToStr(format));
	LogError(LOG_CUDA "                supported formats are:\n");
	LogError(LOG_CUDA "                    * gray8\n");
	LogError(LOG_CUDA "                    * gray32f\n");
	LogError(LOG_CUDA "                    * rgb8, bgr8\n");
	LogError(LOG_CUDA "                    * rgba8, bgra8\n");
	LogError(LOG_CUDA "                    * rgb32f, bgr32f\n");
	LogError(LOG_CUDA "                    * rgba32f, bgra32f\n");

	return cudaErrorInvalidValue;
}
