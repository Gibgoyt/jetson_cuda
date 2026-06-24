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
 *	cudaUtility.h  --  the CUDA error-check / cleanup macros used everywhere
 *	========================================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals" -- first CUDA file
 *	to read):
 *		Almost every .cu / .cpp file that touches CUDA in this project wraps
 *		its CUDA Runtime API calls in the `CUDA(...)` macro defined here.
 *		The job of this header is purely cross-cutting: error-check, log,
 *		and clean up. It has no kernels of its own.
 *
 *	Why these macros exist:
 *		The CUDA Runtime API returns a `cudaError_t` from almost every call
 *		but never throws. Hand-checking every return value is verbose and
 *		easy to forget. The `CUDA(x)` macro funnels every call through
 *		`cudaCheckError`, which logs the call site (file:line + stringified
 *		expression) plus the human-readable error string on failure.
 *
 *	The macro family at a glance:
 *		CUDA(x)            -- run x, log on error, return the cudaError_t
 *		CUDA_SUCCESS(x)    -- bool: did x succeed?
 *		CUDA_FAILED(x)     -- bool: did x fail?
 *		CUDA_VERIFY(x)     -- `return false;` on failure (bool-returning fns)
 *		CUDA_ASSERT(x)     -- `return _retval;` on failure (cudaError_t fns)
 *		CUDA_FREE(x)       -- null-safe cudaFree + nulls the pointer
 *		CUDA_FREE_HOST(x)  -- null-safe cudaFreeHost + nulls the pointer
 *		SAFE_DELETE(x)     -- null-safe `delete` + nulls the pointer
 *		SAFE_FREE(x)       -- null-safe `free` + nulls the pointer
 *		iDivUp(a, b)       -- ceil(a/b), used to size CUDA grids
 *		LOG_CUDA           -- the "[cuda]   " log prefix
 *		CUDA_TRACE         -- (opt-in) compile-time flag: log EVERY successful
 *		                      CUDA call too, not just failures
 *
 *	Logging side-channel:
 *		All failures route through the `LogError` macros from logging.h, so
 *		`--log-level=...` parsed by commandLine controls verbosity here too.
 *
 *	Gotchas to remember when reading code that uses these:
 *		- These are MACROS, not functions. `x` is evaluated once inside
 *		  cudaCheckError, but the bool wrappers (CUDA_SUCCESS / CUDA_FAILED)
 *		  and CUDA_VERIFY all expand to a `CUDA(x)` call too.
 *		- The control-flow macros (CUDA_VERIFY / CUDA_ASSERT) embed `return`
 *		  statements. They only compile inside functions whose return type
 *		  matches (`bool` for VERIFY, `cudaError_t` for ASSERT). Don't use
 *		  them from `void`-returning bodies or constructors.
 *		- CUDA_VERIFY has no surrounding braces around its `if` -- safe in
 *		  current call sites but it can bite next to a dangling `else`. 
*/

#if !defined(__CUDA_UTILITY_H_) || 1
	#define __CUDA_UTILITY_H_

	#include <cuda_runtime.h>
	#include <cuda.h>
	#include <stdio.h>
	#include <string.h>
	#include <stdint.h>
	
	#include "logging.h"

	/*
	 *	------------------------------------------------------------------
	 *	Error-check macros (the canonical CUDA(...) wrapper)
	 *	------------------------------------------------------------------
	 *	Every CUDA Runtime API call in the project is expected to go through
	 *	CUDA(). The other macros below (CUDA_SUCCESS / CUDA_FAILED /
	 *	CUDA_VERIFY / CUDA_ASSERT) are convenience wrappers built on top.
	*/

	/**
	 * Execute a CUDA call and print out any errors
	 *
	 * Stringifies `x` via `#x` so the log line shows the actual source
	 * expression that failed (e.g. `cudaMalloc(&ptr, size)`), along with
	 * `__FILE__` and `__LINE__` for the call site.
	 *
	 * @return the original cudaError_t result
	 * @ingroup cudaError
	*/
	#define CUDA(x) cudaCheckError((x), #x, __FILE__, __LINE__)

	/**
	 * Evaluates to true on success
	 *
	 * Expands to a full `CUDA(x)` call -- i.e. the wrapped expression runs
	 * exactly once and any error is logged before the comparison.
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_SUCCESS(x) (CUDA(x) == cudaSuccess)

	/**
	 * Evaluates to true on failure
	 *
	 * Same single-evaluation semantics as CUDA_SUCCESS(x).
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_FAILED(x) (CUDA(x) != cudaSuccess)

	/**
	 * Return from the boolean function if CUDA call fails
	 *
	 * Only legal inside a function whose return type is `bool`. Note this
	 * macro expands to a bare `if (...) return false;` with NO surrounding
	 * braces, so be careful using it next to a dangling `else` in the
	 * caller.
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_VERIFY(x)		\
		if (CUDA_FAILED(x))      \
			return false;

	/**
	 * Return on CUDA errors, continue on cudaSuccess.
	 *
	 * Only legal inside a function returning `cudaError_t`. The raw error
	 * code is propagated unchanged on failure so the caller's caller can
	 * still branch on the specific cudaError_t value.
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_ASSERT(x)						\
		{									\
			const cudaError_t _retval = CUDA(x);    \
			if (_retval != cudaSuccess)             \
				return _retval;				\
		}

	/*
	 *	------------------------------------------------------------------
	 *	Logging configuration
	 *	------------------------------------------------------------------
	*/

	/**
	 * LOG_CUDA string.
	 *
	 * Prefix prepended to every log line emitted from CUDA error reporting.
	 * Trailing spaces align it with other subsystem prefixes ("[tensorrt] ",
	 * "[gstreamer] ", ...) used elsewhere in the project.
	 *
	 * @ingroup cudaError
	*/
	#define LOG_CUDA "[cuda]   "

	/*
	 * define this if you want all cuda calls to be printed
	 *
	 * Compile-time opt-in: when defined, cudaCheckError will LogDebug the
	 * stringified call on *success* too. Off by default -- otherwise the
	 * log fills up with one line per CUDA call per frame.
	 *
	 * @ingroup cudaError
	*/
	// #define CUDA_TRACE

	/*
	 *	------------------------------------------------------------------
	 *	cudaCheckError -- the inline body behind CUDA(x)
	 *	------------------------------------------------------------------
	*/

	/**
	 * cudaCheckError
	 *
	 * On success: silent (or LogDebug if CUDA_TRACE is defined at compile
	 * time). On failure: three LogError lines --
	 *   1. the stringified call (`txt`)
	 *   2. cudaGetErrorString + numeric / hex error code
	 *   3. file:line of the call site
	 *
	 * The original cudaError_t is always returned unchanged so callers can
	 * branch on it (which is what the wrapper macros above do).
	 *
	 * @ingroup cudaError
	*/
	inline cudaError_t cudaCheckError(
		cudaError_t retval, 
		const char* txt, 
		const char* file, 
		int line
	) {
		if (retval == cudaSuccess) {
		#if !defined(CUDA_TRACE)
			return cudaSuccess;
		#else
			LogDebug(LOG_CUDA "%s\n", txt);
		#endif
	} else {
		LogError(LOG_CUDA "%s\n", txt);
		LogError(
			LOG_CUDA "   %s (error %u) (hex 0x%02X)\n",
			cudaGetErrorString(retval),
			retval,
			retval
		);
			LogError(LOG_CUDA "   %s:%i\n", file, line);
		}

		return retval;
	}

	/*
	 *	------------------------------------------------------------------
	 *	Null-safe cleanup macros
	 *	------------------------------------------------------------------
	 *	All four macros share the same shape: NULL-check, release, then set
	 *	the pointer to NULL so a subsequent call is a no-op. Pair each
	 *	allocation with the matching cleanup:
	 *
	 *		cudaMalloc                                       -> CUDA_FREE
	 *		cudaMallocHost / cudaHostAlloc / cudaAllocMapped -> CUDA_FREE_HOST
	 *		new T                                            -> SAFE_DELETE
	 *		malloc                                           -> SAFE_FREE
	 *
	 *	Caveat: these are macros and reference `x` more than once, so do not
	 *	pass an expression with side-effects (e.g. `CUDA_FREE(getPtr())`
	 *	would call getPtr() twice). In practice all call sites pass a plain
	 *	member variable.
	*/

	/**
	 * Check for non-NULL pointer before freeing it, and then set the pointer to NULL.
	 *
	 * For device-side allocations made via `cudaMalloc`. Calling cudaFree
	 * on a host-pinned pointer is undefined behavior -- use CUDA_FREE_HOST
	 * for those instead.
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_FREE(x)     \
		if (x != NULL) {	\
			cudaFree(x);   \
			x = NULL;		\
		}

	/**
	 * Check for non-NULL pointer before freeing it, and then set the pointer to NULL.
	 *
	 * Use this for buffers allocated via `cudaMallocHost`, `cudaHostAlloc`,
	 * or `cudaAllocMapped` (i.e. the zero-copy / pinned host paths covered
	 * in cudaMappedMemory.h). Mismatching CUDA_FREE / CUDA_FREE_HOST is
	 * undefined behavior, hence the separate macro.
	 *
	 * @ingroup cudaError
	*/
	#define CUDA_FREE_HOST(x)          \
		if (x != NULL) {              \
			cudaFreeHost(x);         \
			x = NULL;                \
		}

	/**
	 * Check for non-NULL pointer before deleting it, and then set the pointer to NULL.
	 *
	 * Uses scalar `delete`, NOT `delete[]`. For arrays allocated with
	 * `new T[N]` you must roll your own cleanup, not use this macro.
	 *
	 * @ingroup util
	*/
	#define SAFE_DELETE(x)   \
		if (x != NULL) {    \
			delete x;      \
			x = NULL;      \
		}

	/**
	 * Check for non-NULL pointer before freeing it, and then set the pointer to NULL.
	 *
	 * The host-`malloc` counterpart to SAFE_DELETE. Do not use this on
	 * CUDA-allocated buffers -- pair `cudaMalloc` with CUDA_FREE and the
	 * pinned-host allocators with CUDA_FREE_HOST.
	 *
	 * @ingroup util
	*/
	#define SAFE_FREE(x)          \
		if (x != NULL) {         \
			free(x);            \
			x = NULL;           \
		}

	/*
	 *	------------------------------------------------------------------
	 *	Grid-sizing helper
	 *	------------------------------------------------------------------
	 *	This is THE helper you will see at the top of every kernel launch in
	 *	the project. CUDA grids are sized in whole blocks, so when the image
	 *	dimensions are not a multiple of the block size you need to round
	 *	the block count up and then bounds-check inside the kernel.
	 *
	 *	`__device__ __host__` means the same function is compiled for both
	 *	the host (so the launch site can call it) and the device (so kernels
	 *	can call it too, e.g. for nested tiling math). nvcc handles the dual
	 *	compilation; for plain .cpp translation units this still works
	 *	because the qualifiers are no-ops to the host compiler when the
	 *	function is only called from host code.
	*/

	/**
	 * If a / b has a remainder, round up.  This function is commonly using when launching
	 * CUDA kernels, to compute a grid size inclusive of the entire dataset if it's dimensions
	 * aren't evenly divisible by the block size.
	 *
	 * For example:
	 *
	 *    const dim3 blockDim(8,8);
	 *    const dim3 gridDim(iDivUp(imgWidth,blockDim.x), iDivUp(imgHeight,blockDim.y));
	 *
	 * Then inside the CUDA kernel, there is typically a check that thread index is in-bounds.
	 *
	 * Without the use of iDivUp(), if the data dimensions weren't evenly divisible by the
	 * block size, parts of the data wouldn't be covered by the grid and not processed.
	 *
	 * Caller must guarantee `b != 0` (there is no divide-by-zero guard) and
	 * `a >= 0`. With negative `a` the result is implementation-defined
	 * because `%` and `/` round toward zero in C++.
	 *
	 * @ingroup cuda
	*/
	inline __device__ __host__ int iDivUp(
		int a, 
		int b
	) {
		return (a % b != 0) ? (a / b + 1) : (a / b);
	}

#endif
