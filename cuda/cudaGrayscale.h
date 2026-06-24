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
 *	cudaGrayscale.h  --  grayscale <-> colour conversion kernels (public API)
 *	========================================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals" -- per PLAN.md
 *	this is the FIRST real kernel pair you read, after the CUDA macros
 *	(cudaUtility.h), zero-copy memory (cudaMappedMemory.h), and the vector
 *	traits (cudaVector.h, cudaMath.h) are in place):
 *		Once you can allocate a buffer and stamp values into it,
 *		grayscale conversion is the simplest "actual image op" in the
 *		project -- one input pixel, one output pixel, no neighbourhood
 *		access, no resampling. Reading this header tells you what the
 *		library can convert; reading cudaGrayscale.cu tells you how the
 *		dispatch pattern is wired and what the kernels look like (which
 *		then repeats for cudaResize, cudaRGB, cudaYUV-*, etc.).
 *
 *	What this file declares (16 entry points, four families):
 *
 *		grayscale <-> grayscale  (scalar precision change)
 *		  cudaGray8ToGray32        uint8_t -> float
 *		  cudaGray32ToGray8        float   -> uint8_t   (normalized)
 *
 *		RGB/BGR -> 8-bit grayscale
 *		  cudaRGB8ToGray8          uchar3  -> uint8_t
 *		  cudaRGBA8ToGray8         uchar4  -> uint8_t
 *		  cudaRGB32ToGray8         float3  -> uint8_t   (normalized)
 *		  cudaRGBA32ToGray8        float4  -> uint8_t   (normalized)
 *
 *		RGB/BGR -> floating-point grayscale
 *		  cudaRGB8ToGray32         uchar3  -> float
 *		  cudaRGBA8ToGray32        uchar4  -> float
 *		  cudaRGB32ToGray32        float3  -> float
 *		  cudaRGBA32ToGray32       float4  -> float
 *
 *		8-bit grayscale -> RGB/BGR
 *		  cudaGray8ToRGB8          uint8_t -> uchar3
 *		  cudaGray8ToRGBA8         uint8_t -> uchar4
 *		  cudaGray8ToRGB32         uint8_t -> float3
 *		  cudaGray8ToRGBA32        uint8_t -> float4
 *
 *		floating-point grayscale -> RGB/BGR
 *		  cudaGray32ToRGB8         float -> uchar3      (normalized)
 *		  cudaGray32ToRGBA8        float -> uchar4      (normalized)
 *		  cudaGray32ToRGB32        float -> float3
 *		  cudaGray32ToRGBA32       float -> float4
 *
 *	Naming convention:
 *		cuda<srcFormat>To<dstFormat>(src, dst, W, H, [opts...], stream)
 *
 *		where the formats encode channel count AND scalar precision:
 *		  Gray8   == uint8_t  (1ch, 8-bit)         RGB8  == uchar3 (3ch, 8-bit)
 *		  Gray32  == float    (1ch, fp32)          RGB32 == float3 (3ch, fp32)
 *		                                           RGBA8/32 add an alpha channel.
 *
 *	Luma formula (used by every RGB->Gray variant):
 *		Y = 0.2989*R + 0.5870*G + 0.1140*B    (BT.601, defined in the .cu)
 *
 *	swapRedBlue (BGR inputs):
 *		Every RGB->Gray entry takes `swapRedBlue`. When true, the input
 *		is treated as BGR -- the kernel swaps the first and third
 *		channels before applying the luma weights. Default false (RGB).
 *		Gray->RGB has no swapRedBlue parameter because the three output
 *		channels are written to identical values.
 *
 *	pixelRange (normalization, on every float-input variant):
 *		Floating-point images in this project don't have a fixed range:
 *		a network may produce [-1, 1], [0, 1], or [0, 255]. The Gray8
 *		side of the conversion is always [0, 255], so any path that
 *		consumes or produces a float operand carries a `pixelRange`
 *		(float2{lo, hi}) telling it which range the float side lives in.
 *		Default {0, 255} == no rescaling. Internally the kernel computes
 *		`(v - lo) * 255 / (hi - lo)` per channel before applying the
 *		luma weights.
 *
 *	When to call these directly vs cudaConvertColor:
 *		cudaConvertColor() (in cudaColorspace.h) dispatches over an
 *		imageFormat enum and picks the right cuda*To* entry below at
 *		runtime. Use it when source/dest formats are runtime-variable
 *		(e.g. driven by a videoSource). Call these directly when the
 *		formats are known at compile time -- it skips one switch and
 *		makes the call site self-documenting.
 *
 *	Gotchas to remember:
 *		- The doxygen block for cudaGray32ToRGB8 below has a duplicated
 *		  `@param pixelRange` line. Documentation typo, not a code bug.
 *		- The Gray8/Gray32 scalar-precision change paths internally
 *		  share the launcher used by Gray->RGB(A): the kernel writes
 *		  `make_vec<T_out>(px, px, px, 255)` which collapses to just
 *		  `px` for 1-channel T_out. See the .cu for details.
 *		- Every entry returns `cudaError_t` from CUDA(cudaGetLastError())
 *		  AFTER the kernel launch -- launches are asynchronous, so a
 *		  successful return here only means "no launch-time error". A
 *		  failing kernel will surface on the next sync.
*/

#if !defined(__CUDA_GRAYSCALE_CONVERT_H)
	#define __CUDA_GRAYSCALE_CONVERT_H

	#include "cudaUtility.h"

	/*
	 * @name 8-bit grayscale to floating-point grayscale (and vice versa)
	 * @see cudaConvertColor() from cudaColorspace.h for automated format conversion
	 * @ingroup colorspace
	*/

	///@{

	/**
	 * Convert uint8 grayscale image into float grayscale.
	 * @ingroup colorspace
	 *
	 *	No `pixelRange` is needed for this direction -- the uint8 input
	 *	is always in [0, 255], and the output float simply carries the
	 *	same numeric value (no normalization to [0, 1]).
	*/
	cudaError_t cudaGray8ToGray32(
		uint8_t* input,
		float* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	/**
	 * Convert float grayscale image into uint8 grayscale.
	 *
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 *                   which is used to rescale the fixed-point pixel outputs to [0,255].
	 *                   The default input range is [0,255], where no rescaling occurs.
	 *                   Other common input ranges are [-1, 1] or [0,1].
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray32ToGray8(
		float* input,
		uint8_t* output,
		size_t width,
		size_t height,
		const float2& pixelRange = make_float2(0, 255),
		cudaStream_t stream = 0
	);

	///@}

	//////////////////////////////////////////////////////////////////////////////////
	/// @name RGB/BGR to 8-bit grayscale
	/// @see cudaConvertColor() from cudaColorspace.h for automated format conversion
	/// @ingroup colorspace
	//////////////////////////////////////////////////////////////////////////////////

	///@{

	/**
	 * Convert uchar3 RGB/BGR image into uint8 grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaRGB8ToGray8(
		uchar3* input,
		uint8_t* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	/**
	 * Convert uchar4 RGBA/BGRA image into uint8 grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	 *
	 *	Alpha is discarded -- grayscale is single-channel.
	*/
	cudaError_t cudaRGBA8ToGray8(
		uchar4* input,
		uint8_t* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	/**
	 * Convert float3 RGB/BGR image into uint8 grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 *                   which is used to rescale the fixed-point pixel outputs to [0,255].
	 *                   The default input range is [0,255], where no rescaling occurs.
	 *                   Other common input ranges are [-1, 1] or [0,1].
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaRGB32ToGray8(
		float3* input,
		uint8_t* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		const float2& pixelRange = make_float2(0, 255),
		cudaStream_t stream = 0
	);

	/**
	 * Convert float4 RGBA/BGRA image into uint8 grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 *                   which is used to rescale the fixed-point pixel outputs to [0,255].
	 *                   The default input range is [0,255], where no rescaling occurs.
	 *                   Other common input ranges are [-1, 1] or [0,1].
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaRGBA32ToGray8(
		float4* input,
		uint8_t* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		const float2& pixelRange = make_float2(0, 255),
		cudaStream_t stream = 0
	);

	///@}

	//////////////////////////////////////////////////////////////////////////////////
	/// @name RGB/BGR to floating-point grayscale
	/// @see cudaConvertColor() from cudaColorspace.h for automated format conversion
	/// @ingroup colorspace
	//////////////////////////////////////////////////////////////////////////////////

	///@{

	/**
	 * Convert uchar3 RGB/BGR image into float grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	 *
	 *	No `pixelRange` here -- the output is the BT.601 luma of the
	 *	8-bit input in the same [0, 255] numeric range; callers wanting
	 *	[0, 1] floats can divide downstream.
	*/
	cudaError_t cudaRGB8ToGray32(
		uchar3* input,
		float* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	/**
	 * Convert uchar4 RGBA/BGRA image into float grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaRGBA8ToGray32(
		uchar4* input,
		float* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	/**
	 * Convert float3 RGB/BGR image into float grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	 *
	 *	No `pixelRange` here -- this is the pure-float path, BT.601
	 *	weights are applied to the input values as-is and the result
	 *	stays in whatever numeric range the input was.
	*/
	cudaError_t cudaRGB32ToGray32(
		float3* input,
		float* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	/**
	 * Convert float4 RGB/BGR image into float grayscale.
	 *
	 * @param swapRedBlue if true, swap the input's red and blue channels.
	 *                    The default is false, and the channels will remain the same.
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaRGBA32ToGray32(
		float4* input,
		float* output,
		size_t width,
		size_t height,
		bool swapRedBlue = false,
		cudaStream_t stream = 0
	);

	///@}

	//////////////////////////////////////////////////////////////////////////////////
	/// @name 8-bit grayscale to RGB/BGR
	/// @see cudaConvertColor() from cudaColorspace.h for automated format conversion
	/// @ingroup colorspace
	//////////////////////////////////////////////////////////////////////////////////

	///@{

	/*
	 *	No swapRedBlue parameter on any of these: the three output
	 *	channels are written to the same grayscale value, so R<->B
	 *	swap is a no-op. The alpha-bearing destinations get a default
	 *	alpha of 255 (uchar) or whatever make_vec uses for float4 --
	 *	see cudaVector.h.
	*/

	/**
	 * Convert uint8 grayscale image into uchar3 RGB/BGR.
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray8ToRGB8(
		uint8_t* input,
		uchar3* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	/**
	 * Convert uint8 grayscale image into uchar4 RGB/BGR.
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray8ToRGBA8(
		uint8_t* input,
		uchar4* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	/**
	 * Convert uint8 grayscale image into float3 RGB/BGR.
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray8ToRGB32(
		uint8_t* input,
		float3* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	/**
	 * Convert uint8 grayscale image into float4 RGB/BGR.
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray8ToRGBA32(
		uint8_t* input,
		float4* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	///@}

	//////////////////////////////////////////////////////////////////////////////////
	/// @name Floating-point grayscale to RGB/BGR
	/// @see cudaConvertColor() from cudaColorspace.h for automated format conversion
	/// @ingroup colorspace
	//////////////////////////////////////////////////////////////////////////////////

	///@{

	/**
	 * Convert float grayscale image into uchar3 RGB/BGR.
	 *
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 *                   which is used to rescale the fixed-point pixel outputs to [0,255].
	 *                   The default input range is [0,255], where no rescaling occurs.
	 *                   Other common input ranges are [-1, 1] or [0,1].
	 *
	 * @ingroup colorspace
	 *
	 *	NOTE: the @param pixelRange line above is duplicated in the
	 *	source -- doxygen typo, not a code bug. Kept verbatim so the
	 *	generated docs match upstream.
	*/
	cudaError_t cudaGray32ToRGB8(
		float* input,
		uchar3* output,
		size_t width,
		size_t height,
		const float2& pixelRange = make_float2(0, 255),
		cudaStream_t stream = 0
	);

	/**
	 * Convert float grayscale image into uchar4 RGB/BGR.
	 *
	 * @param pixelRange specifies the floating-point pixel value range of the input image,
	 *                   which is used to rescale the fixed-point pixel outputs to [0,255].
	 *                   The default input range is [0,255], where no rescaling occurs.
	 *                   Other common input ranges are [-1, 1] or [0,1].
	 *
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray32ToRGBA8(
		float* input,
		uchar4* output,
		size_t width,
		size_t height,
		const float2& pixelRange = make_float2(0, 255),
		cudaStream_t stream = 0
	);

	/**
	 * Convert float grayscale image into float3 RGB/BGR.
	 * @ingroup colorspace
	 *
	 *	No pixelRange -- the input float is broadcast into three float
	 *	channels as-is. If you want normalization, do it before/after.
	*/
	cudaError_t cudaGray32ToRGB32(
		float* input,
		float3* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	/**
	 * Convert float grayscale image into float4 RGB/BGR.
	 * @ingroup colorspace
	*/
	cudaError_t cudaGray32ToRGBA32(
		float* input,
		float4* output,
		size_t width,
		size_t height,
		cudaStream_t stream = 0
	);

	///@}

#endif
