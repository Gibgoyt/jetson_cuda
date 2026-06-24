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
 *	cudaVector.h  --  generic-programming helpers over CUDA's vector types
 *	======================================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals" -- read right
 *	after cudaUtility.h and cudaMappedMemory.h, paired with cudaMath.h):
 *		CUDA's built-in vector types (uchar3, uchar4, float3, float4, ...)
 *		are plain structs with no common base, no traits, and no
 *		generic constructor. That makes it awkward to write ONE kernel
 *		template that works for "RGB8 or RGBA8 or RGB32F or RGBA32F" --
 *		you would otherwise have to hand-specialize four times. This
 *		header bolts on the small amount of trait/factory/cast plumbing
 *		that the image-op kernels in this project (cudaResize, cudaRGB,
 *		cudaYUV-*, tensorConvert, ...) all rely on to stay generic.
 *
 *	What this file provides (four small facilities, all header-only):
 *		1. cudaVectorTypeInfo<T>::Base
 *		     A traits struct that maps a vector type to its underlying
 *		     scalar -- e.g. cudaVectorTypeInfo<float4>::Base == float,
 *		     cudaVectorTypeInfo<uchar3>::Base == uint8_t. Lets a kernel
 *		     template say `typename cudaVectorTypeInfo<T>::Base` instead
 *		     of having to know whether the channel type is float or
 *		     uint8_t up front.
 *
 *		2. make_vec<T>(x, y, z, w)
 *		     A uniform 4-arg constructor across all supported vector
 *		     types. Specializations drop the unused arguments (e.g.
 *		     make_vec<uchar3> ignores w, make_vec<uchar> returns just
 *		     x). This is what lets the resize/conversion kernels emit
 *		     `make_vec<T>(r, g, b, a)` without caring whether T is 3-
 *		     or 4-channel.
 *
 *		3. cast_vec<T>(src)
 *		     Convert between any pair of supported vector types in a
 *		     single call. Delegates to the make_uchar3 / make_uchar4 /
 *		     make_float3 / make_float4 overloads defined in cudaMath.h,
 *		     which handle the channel-count and scalar-type adaptation
 *		     (8-bit <-> float, RGB <-> RGBA with default alpha).
 *
 *		4. alpha<T>(vec, default_alpha)
 *		     Read the alpha channel of a pixel, or fall back to a
 *		     default for 3-channel types that don't carry one. Used by
 *		     overlay / compositing kernels that may run over either RGB
 *		     or RGBA inputs.
 *
 *	The supported vector type set (cross every overload in this file):
 *		uchar (1ch, 8-bit)    uchar3 (3ch, 8-bit)    uchar4 (4ch, 8-bit)
 *		float (1ch, fp32)     float3 (3ch, fp32)     float4 (4ch, fp32)
 *
 *		Other CUDA vector types (uchar2, float2, int*, double*, half*)
 *		are deliberately NOT supported -- if you try to use them you'll
 *		hit the `cuda_assert_false<T>` static_assert in the unspecialized
 *		primary template. The supported set matches the image formats
 *		this project actually deals with (grayscale, RGB8, RGBA8, RGB32F,
 *		RGBA32F).
 *
 *	How the "unsupported type" guardrail works:
 *		The primary templates of make_vec / cast_vec / alpha don't have
 *		a meaningful body -- they instantiate `cuda_assert_false<T>`,
 *		a struct that derives from std::false_type. Its `::value` is
 *		`false` BUT only after T has been substituted, which defers the
 *		static_assert firing until the template is actually instantiated.
 *		A plain `static_assert(false, ...)` in the primary body would
 *		fire at parse time and break the file. This idiom is the
 *		standard C++ workaround.
 *
 *	Gotchas to remember when reading code that uses these:
 *		- `alpha<T>` is declared `__device__` in its primary template
 *		  but the four specializations are `__host__ __device__`. The
 *		  primary will never actually get called (it's a static_assert),
 *		  so the qualifier mismatch is harmless in practice -- just be
 *		  aware if you try to take its address.
 *		- cast_vec has four overloads of the primary template (one per
 *		  source type) rather than one. Each is then specialized over
 *		  every target type. The result is a 4x4 matrix of supported
 *		  (src -> dst) conversions, all delegating to cudaMath.h's
 *		  make_uchar3 / make_uchar4 / make_float3 / make_float4 family.
 *		- The default alpha in the `alpha<uchar*>` overloads is 255
 *		  (declared at the call site, not here), matching the typical
 *		  RGBA8 max. For float pixels the convention is 1.0f, but the
 *		  default value lives at the call site -- this header just
 *		  passes it through.
 *
 *	Cross-references:
 *		- cudaMath.h -- defines the make_uchar3/4 and make_float3/4
 *		  overloads that cast_vec ultimately delegates to.
 *		- cudaUtility.h -- error-check macros used by callers, not here.
 *		- tensorConvert.cu, cudaResize.cu, cudaRGB.cu, cudaYUV-*.cu --
 *		  the primary consumers of these templates.
*/

#include <cwchar>
#include <istream>
#if !defined(__CUDA_VECTOR_TEMPLATES_H__)
	#define __CUDA_VECTOR_TEMPLATES_H__

	/* vector overloads (make_uchar3/4, make_float3/4, channel adapters) */
	#include "cudaMath.h"

	/* std::false_type for the deferred-static_assert idiom below */
	#include <type_traits>

	/*
	 * @name Vector Templates
	 * @internal
	 * @ingroup cuda
	*/

	///@{

	/*
	 *	------------------------------------------------------------------
	 *	cudaVectorTypeInfo<T> -- scalar-type trait
	 *	------------------------------------------------------------------
	 *	Maps a supported vector type to its underlying scalar (`Base`):
	 *
	 *		cudaVectorTypeInfo<uchar  >::Base == uint8_t
	 *		cudaVectorTypeInfo<uchar3 >::Base == uint8_t
	 *		cudaVectorTypeInfo<uchar4 >::Base == uint8_t
	 *		cudaVectorTypeInfo<float  >::Base == float
	 *		cudaVectorTypeInfo<float3 >::Base == float
	 *		cudaVectorTypeInfo<float4 >::Base == float
	 *
	 *	The primary template is intentionally only DECLARED (no body) so
	 *	an unsupported T causes a clear "incomplete type" compile error
	 *	at the point of misuse. Used by make_vec and alpha below to spell
	 *	their channel-type-agnostic parameters.
	*/

	/* get base type (uint8 or float) from vector */
	template <class T>
		struct cudaVectorTypeInfo;

	template <>
		struct cudaVectorTypeInfo<uchar> {
			typedef uint8_t Base;
		};
	template <>
		struct cudaVectorTypeInfo<uchar3> {
			typedef uint8_t Base;
		};
	template <>
		struct cudaVectorTypeInfo<uchar4> {
			typedef uint8_t Base;
		};

	template <>
		struct cudaVectorTypeInfo<float> {
			typedef float Base;
		};
	template <>
		struct cudaVectorTypeInfo<float3> {
			typedef float Base;
		};
	template <>
		struct cudaVectorTypeInfo<float4> {
			typedef float Base;
		};

	/*
	 *	------------------------------------------------------------------
	 *	cuda_assert_false<T> -- deferred static_assert helper
	 *	------------------------------------------------------------------
	 *	Derives from std::false_type so `::value` is `false`, but only
	 *	AFTER T is substituted at instantiation. This lets the primary
	 *	templates of make_vec / cast_vec / alpha hold a static_assert
	 *	that fires only when an unsupported T is actually used -- a
	 *	plain `static_assert(false, ...)` in the primary body would fire
	 *	at parse time and break compilation of every TU including this
	 *	header. Standard C++ idiom; don't "simplify" it.
	*/

	/* static compile-time assertion */
	template <typename T>
		struct cuda_assert_false : std::false_type {};

	/*
	 *	------------------------------------------------------------------
	 *	make_vec<T>(x, y, z, w) -- uniform 4-arg constructor
	 *	------------------------------------------------------------------
	 *	Construct a value of vector type T from up to four scalar
	 *	components, dropping the unused ones for 1- and 3-channel
	 *	specializations:
	 *
	 *		make_vec<uchar >(x, y, z, w)  ==  x                                (drops y, z, w)
	 *		make_vec<uchar3>(x, y, z, w)  ==  make_uchar3(x, y, z)             (drops w)
	 *		make_vec<uchar4>(x, y, z, w)  ==  make_uchar4(x, y, z, w)
	 *		make_vec<float >(x, y, z, w)  ==  x                                (drops y, z, w)
	 *		make_vec<float3>(x, y, z, w)  ==  make_float3(x, y, z)             (drops w)
	 *		make_vec<float4>(x, y, z, w)  ==  make_float4(x, y, z, w)
	 *
	 *	The parameter types are spelled
	 *	`typename cudaVectorTypeInfo<T>::Base` so that callers don't
	 *	need to know whether the channel scalar is uint8_t or float --
	 *	exactly the point of the trait. Primary template is the
	 *	deferred-static_assert sentinel; unsupported T errors here with
	 *	the "invalid vector type" message.
	*/

	/* make_vec<T> templates */
	template <typename T>
		inline __host__ __device__ T make_vec(
			typename cudaVectorTypeInfo<T>::Base x,
			typename cudaVectorTypeInfo<T>::Base y,
			typename cudaVectorTypeInfo<T>::Base z,
			typename cudaVectorTypeInfo<T>::Base w
		) {
			static_assert(
				cudaVectorTypeInfocuda_assert_false<T>::value,
				"invalid vector type - supported types are uchar3, uchar4, float3, float4"
			);
		}

	template <>
		inline __host__ __device__ uchar make_vec(
			uint8_t x, 
			uint8_t y, 
			uint8_t z, 
			uint8_t w
		) {
			return x;
		}
	template <>
		inline __host__ __device__ uchar3 make_vec(
			uint8_t x, 
			uint8_t y, 
			uint8_t z, 
			uint8_t w
		) {
			return make_uchar3(x, y, z);
		}
	template <>
		inline __host__ __device__ uchar4 make_vec(
			uint8_t x,
			uint8_t y, 
			uint8_t z, 
			uint8_t w
		) {
			return make_uchar4(x, y, z, w);
		}

	template <>
		inline __host__ __device__ float make_vec(
			float x, 
			float y, 
			float z, 
			float w
		) {
			return x;
		}
	template <>
		inline __host__ __device__ float3 make_vec(
			float x, 
			float y, 
			float z, 
			float w
		) {
			return make_float3(x, y, z);
	}
	template <>
		inline __host__ __device__ float4 make_vec(
			float x, 
			float y, 
			float z, 
			float w
		) {
			return make_float4(x, y, z, w);
		}

	/*
	 *	------------------------------------------------------------------
	 *	cast_vec<T>(src) -- generic vector-type conversion
	 *	------------------------------------------------------------------
	 *	Convert between any pair of {uchar3, uchar4, float3, float4} in
	 *	one call. The actual channel-count / scalar adaptation lives in
	 *	cudaMath.h's make_uchar3 / make_uchar4 / make_float3 /
	 *	make_float4 overloads -- this file just routes by target type.
	 *
	 *	Conversion matrix (src down, dst across):
	 *
	 *		           uchar3       uchar4       float3       float4
	 *		uchar3   identity     +alpha=255   uint8->fp32  uint8->fp32+alpha
	 *		uchar4   drop alpha   identity     uint8->fp32  uint8->fp32
	 *		float3   fp32->uint8  fp32->uint8  identity     +alpha=1.0
	 *		float4   fp32->uint8  fp32->uint8  drop alpha   identity
	 *
	 *	(Exact alpha defaults and rounding rules are decided inside
	 *	cudaMath.h -- check there if you need to know the precise value
	 *	a particular conversion produces.)
	 *
	 *	There are FOUR primary templates -- one per supported source
	 *	type -- each of which is then specialized over every target
	 *	type. The unspecialized primaries hit the same
	 *	cuda_assert_false sentinel as make_vec.
	*/

	/* cast_vec<T> templates */
	template <typename T>
	inline __host__ __device__ T cast_vec(
		const uchar3& a
	) {
		static_assert(
		    cuda_assert_false<T>::value,
		    "invalid vector type - supported types are uchar3, uchar4, float3, float4"
		);
	}
	template <typename T>
	inline __host__ __device__ T cast_vec(
		const uchar4& a
	) {
		static_assert(
		    cuda_assert_false<T>::value,
		    "invalid vector type - supported types are uchar3, uchar4, float3, float4"
		);
	}
	template <typename T>
	inline __host__ __device__ T cast_vec(
		const float3& a
	) {
		static_assert(
		    cuda_assert_false<T>::value,
		    "invalid vector type - supported types are uchar3, uchar4, float3, float4"
		);
	}
	template <typename T>
	inline __host__ __device__ T cast_vec(
		const float4& a
	) {
		static_assert(
		    cuda_assert_false<T>::value,
		    "invalid vector type - supported types are uchar3, uchar4, float3, float4"
		);
	}

	/* uchar3 -> {uchar3, uchar4, float3, float4} */
	template <>
	inline __host__ __device__ uchar3 cast_vec(
		const uchar3& a
	) {
		return make_uchar3(a);
	}
	template <>
	inline __host__ __device__ uchar4 cast_vec(
		const uchar3& a
	) {
		return make_uchar4(a);
	}
	template <>
	inline __host__ __device__ float3 cast_vec(
		const uchar3& a
	) {
		return make_float3(a);
	}
	template <>
	inline __host__ __device__ float4 cast_vec(
		const uchar3& a
	) {
		return make_float4(a);
	}

	/* uchar4 -> {uchar3, uchar4, float3, float4} */
	template <>
	inline __host__ __device__ uchar3 cast_vec(
		const uchar4& a
	) {
		return make_uchar3(a);
	}
	template <>
	inline __host__ __device__ uchar4 cast_vec(
		const uchar4& a
	) {
		return make_uchar4(a);
	}
	template <>
	inline __host__ __device__ float3 cast_vec(
		const uchar4& a
	) {
		return make_float3(a);
	}
	template <>
	inline __host__ __device__ float4 cast_vec(
		const uchar4& a
	) {
		return make_float4(a);
	}

	/* float3 -> {uchar3, uchar4, float3, float4} */
	template <>
	inline __host__ __device__ uchar3 cast_vec(
		const float3& a
	) {
		return make_uchar3(a);
	}
	template <>
	inline __host__ __device__ uchar4 cast_vec(
		const float3& a
	) {
		return make_uchar4(a);
	}
	template <>
	inline __host__ __device__ float3 cast_vec(
		const float3& a
	) {
		return make_float3(a);
	}
	template <>
	inline __host__ __device__ float4 cast_vec(
		const float3& a
	) {
		return make_float4(a);
	}

	/* float4 -> {uchar3, uchar4, float3, float4} */
	template <>
	inline __host__ __device__ uchar3 cast_vec(
		const float4& a
	) {
		return make_uchar3(a);
	}
	template <>
	inline __host__ __device__ uchar4 cast_vec(
		const float4& a
	) {
		return make_uchar4(a);
	}
	template <>
	inline __host__ __device__ float3 cast_vec(
		const float4& a
	) {
		return make_float3(a);
	}
	template <>
	inline __host__ __device__ float4 cast_vec(
		const float4& a
	) {
		return make_float4(a);
	}

	/*
	 *	------------------------------------------------------------------
	 *	alpha<T>(vec, default_alpha) -- read or substitute the A channel
	 *	------------------------------------------------------------------
	 *	For 4-channel inputs returns vec.w; for 3-channel inputs returns
	 *	default_alpha (255 for uchar3, typically 1.0f for float3 -- the
	 *	default value lives at the call site, this header just routes).
	 *	Used by overlay / compositing kernels that may run over either
	 *	RGB or RGBA inputs without branching on channel count.
	 *
	 *	Quirk: the primary template is qualified `__device__` only,
	 *	while the four real specializations are `__host__ __device__`.
	 *	The primary never gets called (it's a static_assert sentinel) so
	 *	in practice the mismatch is harmless -- just don't try to take
	 *	the address of the primary template.
	*/

	/* extract alpha color component */
	template <typename T>
	inline __device__ typename cudaVectorTypeInfo<T>::Base
	alpha(
		T vec, 
		typename cudaVectorTypeInfo<T>::Base default_alpha = 255
	) {
		static_assert(
		    cuda_assert_false<T>::value,
		    "invalid vector type - supported types are uchar3, uchar4, float3, float4"
		);
	}

	template <>
	inline __host__ __device__ uint8_t alpha(
		uchar3 vec, 
		uint8_t default_alpha
	) {
		return default_alpha;
	}
	template <>
	inline __host__ __device__ uint8_t alpha(
		uchar4 vec, 
		uint8_t default_alpha
	) {
		return vec.w;
	}

	template <>
	inline __host__ __device__ float alpha(
		float3 vec, 
		float default_alpha
	) {
		return default_alpha;
	}
	template <>
	inline __host__ __device__ float alpha(
		float4 vec, 
		float default_alpha
	) {
		return vec.w;
	}

	///@}

#endif
