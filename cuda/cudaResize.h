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
 *	cudaResize.h  --  GPU image rescaling (public API)
 *	==================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals" -- read right
 *	after cudaGrayscale per PLAN.md):
 *		Resize is the second "real kernel" in the reading order. It's
 *		one step more involved than grayscale conversion because the
 *		input/output grids no longer have a 1:1 pixel mapping -- the
 *		kernel must compute a source coordinate from each destination
 *		coordinate and either fetch the nearest neighbour (point) or
 *		do a 4-tap bilinear blend.
 *
 *		Within the wider library this is also the workhorse called by
 *		tensorNet to letterbox / stretch user input down to a network's
 *		expected input dimensions before tensorConvert turns it into an
 *		NCHW float buffer.
 *
 *	What this file declares (seven overloads):
 *
 *		1ch  uint8_t                 -- gray8     input/output
 *		1ch  float                   -- gray32f   input/output
 *		3ch  uchar3                  -- rgb8 / bgr8 (channel order isn't
 *		                                interpreted; resize is per-channel)
 *		3ch  float3                  -- rgb32f / bgr32f
 *		4ch  uchar4                  -- rgba8 / bgra8
 *		4ch  float4                  -- rgba32f / bgra32f
 *		void* + imageFormat          -- runtime-typed dispatch over the
 *		                                six concrete entries above
 *
 *		The runtime-typed overload is the one to call from streaming
 *		code where the format is decided by an upstream videoSource;
 *		the typed overloads are the right call when the channel type
 *		is known at compile time.
 *
 *	Filter modes:
 *		FILTER_POINT  (default) -- nearest-neighbour, one fetch per pixel
 *		FILTER_LINEAR           -- bilinear, four fetches + lerp per pixel
 *
 *		Both are defined in cudaFilterMode.h, and the per-pixel sample
 *		math lives in cudaFilterMode.cuh's `cudaFilterPixel<filter>`,
 *		which the .cu in this directory just plugs into a generic
 *		2D-grid kernel.
 *
 *	Downscaling quirk:
 *		The launcher in cudaResize.cu silently overrides FILTER_LINEAR
 *		with FILTER_POINT *when both output dimensions are smaller than
 *		the corresponding input dimensions*. The per-overload doxygen
 *		says "if downscaling, nearest-neighbour is used instead" -- but
 *		mixed cases (downscale W, upscale H, or vice versa) keep
 *		FILTER_LINEAR. Useful to know if you ever wonder why a partial
 *		downscale looks blurry while a full downscale doesn't.
 *
 *	What's NOT done here:
 *		- No anti-aliasing on downscale (no area / lanczos filter). If
 *		  you need quality downscaling, do it elsewhere (OpenCV, VPI,
 *		  or a multi-pass box reduction).
 *		- No aspect-ratio preservation or letterboxing -- the kernel
 *		  stretches input dimensions to output dimensions independently
 *		  on each axis. Callers wanting letterboxing must pre-compute
 *		  the destination region themselves (see tensorNet's PreProcess).
 *		- No in-place operation -- input and output must be distinct
 *		  device buffers.
 *
 *	Async caveat (same as the rest of Stage 2):
 *		Every overload returns CUDA(cudaGetLastError()) after the
 *		launch. A `cudaSuccess` only means "launch accepted"; an
 *		out-of-bounds fetch will only surface on the next sync.
*/

#if !defined(__CUDA_RESIZE_H__)
	#define __CUDA_RESIZE_H__

	#include "cudaUtility.h"
	#include "cudaFilterMode.h"

	#include "imageFormat.h"

	/**
	 * Rescale a uint8 grayscale image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	 *
	 *	NOTE: the "if downscaled -> FILTER_POINT" rule in the line above
	 *	only triggers when BOTH output dimensions are smaller than the
	 *	corresponding input dimensions. Mixed up/down cases keep
	 *	FILTER_LINEAR. See cudaResize.cu for the exact branch.
	*/
	cudaError_t cudaResize(
	    uint8_t* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    uint8_t* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale a floating-point grayscale image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	*/
	cudaError_t cudaResize(
	    float* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    float* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale a uchar3 RGB/BGR image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	 *
	 *	The kernel is agnostic to channel meaning -- it resamples each
	 *	channel independently. RGB and BGR therefore go through the same
	 *	overload; there's no swapRedBlue parameter because resize
	 *	preserves the channel order it's given.
	*/
	cudaError_t cudaResize(
	    uchar3* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    uchar3* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale a float3 RGB/BGR image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	*/
	cudaError_t cudaResize(
	    float3* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    float3* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale a uchar4 RGBA/BGRA image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	 *
	 *	Alpha is resampled the same way as RGB -- bilinear blends alpha
	 *	too, which can produce semi-transparent edges on hard-edged
	 *	masks. Use FILTER_POINT if that matters.
	*/
	cudaError_t cudaResize(
	    uchar4* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    uchar4* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale a float4 RGBA/BGRA image on the GPU.
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	*/
	cudaError_t cudaResize(
	    float4* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    float4* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

	/**
	 * Rescale an image on the GPU (supports grayscale, RGB/BGR, RGBA/BGRA)
	 * To use bilinear filtering for upscaling, set filter to FILTER_LINEAR.
	 * If the image is being downscaled, or if FILTER_POINT is set (default),
	 * then nearest-neighbor sampling will be used instead.
	 * @ingroup resize
	 *
	 *	Runtime-typed dispatch entry. Switches on `format` and forwards
	 *	to one of the six typed overloads above after casting input /
	 *	output. Returns cudaErrorInvalidValue and logs the supported
	 *	format list on an unrecognized format. This is the overload
	 *	callers should use when the format is decided by an upstream
	 *	videoSource (i.e. not known at compile time).
	 *
	 *	Supported formats: gray8, gray32f, rgb8 / bgr8, rgba8 / bgra8,
	 *	rgb32f / bgr32f, rgba32f / bgra32f.
	*/
	cudaError_t cudaResize(
	    void* input,
	    size_t inputWidth,
	    size_t inputHeight,
	    void* output,
	    size_t outputWidth,
	    size_t outputHeight,
	    imageFormat format,
	    cudaFilterMode filter = FILTER_POINT,
	    cudaStream_t stream = 0
	);

#endif
