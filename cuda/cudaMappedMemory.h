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
 *	cudaMappedMemory.h  --  zero-copy CPU/GPU buffers (Jetson "unified" memory)
 *	==========================================================================
 *
 *	Position in the pipeline (Stage 2, "CUDA fundamentals" -- read this right
 *	after cudaUtility.h):
 *		Almost every image buffer in the project is born here. videoSource
 *		frames, videoOutput framebuffers, TensorRT input/output tensors,
 *		intermediate kernel scratch -- they all come from one of the
 *		cudaAllocMapped() overloads below. Understanding this header is
 *		what makes the rest of the codebase stop looking like magic: there
 *		is essentially never a `cudaMemcpy` in user code because there is
 *		nothing to copy.
 *
 *	What "mapped memory" actually means:
 *		On a Jetson, the CPU and GPU share one physical DRAM (the integrated
 *		GPU is on the same SoC). "Mapped" / "zero-copy" memory is a single
 *		set of page-locked physical pages exposed in BOTH the host virtual
 *		address space and the device virtual address space. A pointer
 *		obtained from cudaHostAlloc(..., cudaHostAllocMapped) and the
 *		"device pointer" you get from cudaHostGetDevicePointer() refer to
 *		the same bytes -- and on Jetson, they're literally the same numeric
 *		address (which is what the `cpu != gpu` check in cudaMallocMapped
 *		below is asserting). On a discrete-GPU host the same API still
 *		works, but the GPU reaches the buffer over PCIe on demand and the
 *		two virtual addresses are NOT equal -- which is why the two-pointer
 *		overload (line ~203) exists and is the only portable variant.
 *
 *	Zero-copy removes the copy, NOT the data race:
 *		There is no implicit barrier between the CPU writing into a mapped
 *		buffer and a kernel reading from it (or vice-versa). You still need
 *		cudaDeviceSynchronize() / a stream sync before the other side
 *		touches the bytes, exactly the same as with cudaMemcpy. Skipping
 *		this is the #1 way zero-copy code "works most of the time" and
 *		then mysteriously corrupts a frame under load.
 *
 *	The API surface at a glance:
 *		cudaMallocMapped(ptr, size, clear)            -- the real allocator;
 *		                                                 returns cudaError_t
 *		cudaAllocMapped(ptr, size, clear)             -- bool wrapper
 *		cudaAllocMapped(ptr, W, H, format, clear)     -- sizes via imageFormatSize
 *		cudaAllocMapped(ptr, int2, format, clear)     -- same, dims as int2
 *		cudaAllocMapped<T>(T**, W, H, clear)          -- sizes via W*H*sizeof(T)
 *		cudaAllocMapped<T>(T**, int2, clear)          -- same, dims as int2
 *		cudaAllocMapped<T>(T**, size, clear)          -- BYTES (not elements!)
 *		cudaAllocMapped(cpuPtr, gpuPtr, size, clear)  -- legacy two-pointer;
 *		                                                 portable to dGPU hosts
 *
 *		All of them are convenience wrappers around the same
 *		cudaHostAlloc + cudaHostGetDevicePointer pair -- they differ only
 *		in how the byte count is spelled at the call site.
 *
 *	Pairing with cleanup:
 *		Buffers from this file must be released with CUDA_FREE_HOST (or
 *		cudaFreeHost directly). Using CUDA_FREE / cudaFree on them is
 *		undefined behavior -- the cleanup-macro comment block in
 *		cudaUtility.h spells this out, and it's the reason CUDA_FREE_HOST
 *		exists as a separate macro.
 *
 *	Gotchas to remember when reading code that uses these:
 *		- The templated `cudaAllocMapped<T>(T**, size_t, bool)` overload at
 *		  the bottom of the size-spelling group takes BYTES, not element
 *		  count, despite being templated on T. Easy to mistake for "N
 *		  elements of T" because the W*H templated overloads multiply by
 *		  sizeof(T). Read the call site carefully.
 *		- cudaMallocMapped enforces `cpu == gpu` and returns
 *		  cudaErrorInvalidDevicePointer if they differ. This makes it
 *		  Jetson-specific by construction. On a discrete-GPU host (or any
 *		  config where the two virtual addresses differ) you must use the
 *		  two-pointer cudaAllocMapped overload instead.
 *		- Failure path leak: if cudaHostAlloc succeeds but
 *		  cudaHostGetDevicePointer fails, the host allocation is not freed
 *		  before returning. Same in the two-pointer overload. Rare in
 *		  practice but worth knowing.
 *		- The two-pointer overload deliberately does NOT enforce
 *		  `*cpuPtr == *gpuPtr`, so it works on dGPU hosts -- but it also
 *		  reports failures as `false` via CUDA_FAILED instead of returning
 *		  the raw cudaError_t the way cudaMallocMapped does. Asymmetry is
 *		  intentional; don't "unify" them.
*/

#if !defined(__CUDA_MAPPED_MEMORY_H_)
	#define __CUDA_MAPPED_MEMORY_H_

	#include "cudaUtility.h"
	#include "imageFormat.h"
	#include "logging.h"

	/*
	 *	------------------------------------------------------------------
	 *	cudaMallocMapped -- the real allocator
	 *	------------------------------------------------------------------
	 *	This is the only function in the file that actually talks to the
	 *	CUDA Runtime. Every cudaAllocMapped() overload below is just a
	 *	different way of computing the `size` argument and then forwarding
	 *	here (directly or via the bool wrapper).
	*/

	/**
	 * Allocate mapped memory that shares the same physical memory between CPU and GPU,
	 * as is the case on Jetson devices with unified memory.  This eliminates the need
	 * for memory copies (Zero Copy), although synchronization is still required so that
	 * both processors are not accessing the same memory simultaneously.
	 *
	 * @param[out] ptr Returned pointer to the shared memory, can be accessed from both the CPU
	 *                 and in CUDA kernels This memory should be released with cudaFreeHost()
	 * @param[in] size Size (in bytes) of the shared memory to allocate.
	 * @param[in] clear If `true` (the default), the memory contents will be filled with zeros.
	 *
	 * @note This function is the same as cudaAllocMapped(), but returns cudaError_t instead of bool.
	 * @returns cudaSuccess on success, cudaError_t on failure.
	 * @ingroup cudaMemory
	 *
	 *	Implementation walk-through:
	 *	  1. cudaHostAlloc(..., cudaHostAllocMapped) -- page-locks `size`
	 *	     bytes of host memory AND tags the pages so the GPU can map
	 *	     them into its own virtual address space. Without the
	 *	     cudaHostAllocMapped flag this would just be pinned host
	 *	     memory (still useful, but not zero-copy).
	 *	  2. cudaHostGetDevicePointer -- asks the driver for the device-
	 *	     side virtual address that backs the same physical pages.
	 *	     On Jetson this equals the host address; on dGPU it doesn't.
	 *	  3. The `cpu != gpu` check is the Jetson-unified-memory invariant.
	 *	     If it ever fires you are NOT on a true unified-memory
	 *	     platform -- callers on dGPU hosts must use the two-pointer
	 *	     cudaAllocMapped overload instead, which doesn't enforce this.
	 *
	 *	The commented-out cudaSetDeviceFlags(cudaDeviceMapHost) below is
	 *	historical: older CUDA required it before the first
	 *	cudaHostAllocMapped call. Modern L4T enables it implicitly, so
	 *	the line is left as a tombstone.
	 *
	 *	Leak-on-failure caveat: if cudaHostAlloc succeeds but the
	 *	subsequent cudaHostGetDevicePointer fails, `cpu` is not freed
	 *	before the early return. Rare in practice (the only realistic
	 *	failure is OOM, which kills the first call), but worth knowing.
	*/
	inline cudaError_t cudaMallocMapped(
		void** ptr, 
		size_t size, 
		bool clear = true
	) {
		void* cpu = NULL;
		void* gpu = NULL;

		if (!ptr || size == 0) {
			return cudaErrorInvalidValue;
		}

		// CUDA_ASSERT(cudaSetDeviceFlags(cudaDeviceMapHost));

		CUDA_ASSERT(cudaHostAlloc(&cpu, size, cudaHostAllocMapped));
		CUDA_ASSERT(cudaHostGetDevicePointer(&gpu, cpu, 0));

		// Jetson-unified-memory invariant: on a true UMA platform the
		// host and device virtual addresses must coincide. If they
		// differ we're on a dGPU host -- caller should use the
		// two-pointer overload instead.
		if (cpu != gpu) {
			LogError(
				LOG_CUDA
				"cudaMallocMapped() - addresses of CPU and GPU pointers don't match (CPU=%p GPU=%p)\n",
				cpu,
				gpu
			);
			return cudaErrorInvalidDevicePointer;
		}

		if (clear)
			memset(cpu, 0, size);

		*ptr = cpu;
		// TODO!!: Use of undeclared identifier 'cudaSuccess'??? I think cmake at ../build/ needs to config
		return cudaSuccess;
	}

	/*
	 *	------------------------------------------------------------------
	 *	cudaAllocMapped -- size-spelling convenience overloads
	 *	------------------------------------------------------------------
	 *	Seven overloads, one job: compute `size` in bytes and forward to
	 *	cudaMallocMapped. They differ ONLY in how the call site spells
	 *	the allocation size. Pick the one whose signature matches the
	 *	data you already have:
	 *
	 *		caller already has...          use overload...
	 *		bytes                          (ptr, size)
	 *		W, H, imageFormat              (ptr, W, H, format)
	 *		int2{W,H}, imageFormat         (ptr, int2, format)
	 *		W, H, vector type T            (T**, W, H)
	 *		int2{W,H}, vector type T       (T**, int2)
	 *		bytes, want T-typed pointer    (T**, size)        -- BYTES, NOT N!
	*/

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * @note this overload of cudaAllocMapped returns one pointer, assumes that the
	 *       CPU and GPU addresses will match (as is the case with any recent CUDA version).
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] size Size (in bytes) of the shared memory to allocate.
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	Bool wrapper over cudaMallocMapped. Use this when you don't
	 *	care about the specific cudaError_t -- which is most of the
	 *	time, because the underlying error is already logged by the
	 *	CUDA() macro inside CUDA_ASSERT.
	*/
	inline bool cudaAllocMapped(
		void** ptr, 
		size_t size, 
		bool clear = true
	) {
		return CUDA_SUCCESS(cudaMallocMapped(ptr, size, clear));
	}

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * This overload is for allocating images from an imageFormat type
	 * and the image dimensions.  The overall size of the allocation
	 * will be calculated with the imageFormatSize() function.
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] width Width (in pixels) to allocate.
	 * @param[in] height Height (in pixels) to allocate.
	 * @param[in] format Format of the image.
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	Size = imageFormatSize(format, W, H) -- delegates to the format
	 *	registry in imageFormat.h, which knows the bytes/pixel and any
	 *	planar layout for each supported format (RGB8, RGBA32F, NV12, ...).
	*/
	inline bool cudaAllocMapped(
		void** ptr, 
		size_t width, 
		size_t height, 
		imageFormat format, 
		bool clear = true
	) {
		return cudaAllocMapped(ptr, imageFormatSize(format, width, height), clear);
	}

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * This overload is for allocating images from an imageFormat type
	 * and the image dimensions.  The overall size of the allocation
	 * will be calculated with the imageFormatSize() function.
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] dims `int2` vector where `width=dims.x` and `height=dims.y`
	 * @param[in] format Format of the image.
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	Same as the (W, H, format) overload above but spelled as an int2.
	 *	Convenient at call sites that already carry dims as a CUDA vector
	 *	type (videoSource, gstCamera, etc.).
	*/
	inline bool cudaAllocMapped(
		void** ptr, 
		const int2& dims, 
		imageFormat format, 
		bool clear = true
	) {
		return cudaAllocMapped(ptr, imageFormatSize(format, dims.x, dims.y), clear);
	}

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * This is a templated version for allocating images from vector types
	 * like uchar3, uchar4, float3, float4, ect.  The overall size of the
	 * allocation will be calculated as `width * height * sizeof(T)`.
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] width Width (in pixels) to allocate.
	 * @param[in] height Height (in pixels) to allocate.
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	Size = W * H * sizeof(T). Use this when you're working in a
	 *	known pixel type (e.g. uchar4 for RGBA8 or float4 for RGBA32F)
	 *	and don't need the imageFormat machinery -- the resulting
	 *	pointer is already typed.
	*/
	template <typename T>
	inline bool cudaAllocMapped(
		T** ptr, 
		size_t width, 
		size_t height, 
		bool clear = true
	) {
		return cudaAllocMapped((void**)ptr, width * height * sizeof(T), clear);
	}

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * This is a templated version for allocating images from vector types
	 * like uchar3, uchar4, float3, float4, ect.  The overall size of the
	 * allocation will be calculated as `dims.x * dims.y * sizeof(T)`.
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] dims `int2` vector where `width=dims.x` and `height=dims.y`
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	Same as the templated (T**, W, H) overload, spelled as an int2.
	*/
	template <typename T>
	inline bool cudaAllocMapped(
		T** ptr, 
		const int2& dims, 
		bool clear = true
	) {
		return cudaAllocMapped((void**)ptr, dims.x * dims.y * sizeof(T), clear);
	}

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * This is a templated version for allocating images from vector types
	 * like uchar3, uchar4, float3, float4, ect.  The overall size of the
	 * allocation is specified by the size parameter.
	 *
	 * @param[out] ptr Returned pointer to the shared CPU/GPU memory.
	 * @param[in] size size of the allocation, in bytes.
	 * @param[in] clear If `true` (default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	 *
	 *	WARNING: `size` is BYTES, not an element count, despite the T**
	 *	template parameter. This is the most error-prone overload in the
	 *	file -- the templated (T**, W, H) and (T**, int2) variants above
	 *	DO multiply by sizeof(T), so readers naturally expect this one to
	 *	as well. It doesn't. If you want N elements of T, pass
	 *	`N * sizeof(T)`.
	*/
	template <typename T>
	inline bool cudaAllocMapped(
		T** ptr, 
		size_t size, 
		bool clear = true
	) {
		return cudaAllocMapped((void**)ptr, size, clear);
	}

	/*
	 *	------------------------------------------------------------------
	 *	cudaAllocMapped -- legacy two-pointer overload
	 *	------------------------------------------------------------------
	 *	Predates the assumption that host and device pointers are equal.
	 *	Returns the two pointers separately, does NOT enforce that they
	 *	match, and so is the only variant in this file that works
	 *	correctly on a discrete-GPU host.
	 *
	 *	Intentional asymmetry vs cudaMallocMapped:
	 *	  - Reports failure as `false` (via CUDA_FAILED) rather than a
	 *	    raw cudaError_t. Don't "unify" the two -- the bool form is
	 *	    what existing callers expect.
	 *	  - No `cpu == gpu` check, deliberately (that's the whole point).
	 *	  - Logs a LogDebug line on success (the single-pointer path
	 *	    doesn't). Useful when chasing zero-copy lifecycle issues.
	 *	  - Same leak-on-failure shape as cudaMallocMapped: if
	 *	    cudaHostAlloc succeeds but cudaHostGetDevicePointer fails,
	 *	    *cpuPtr is not freed before returning false.
	*/

	/**
	 * Allocate ZeroCopy mapped memory, shared between CUDA and CPU.
	 *
	 * @note although two pointers are returned, one for CPU and GPU, they both resolve to the same
	 * physical memory.
	 *
	 * @param[out] cpuPtr Returned CPU pointer to the shared memory.
	 * @param[out] gpuPtr Returned GPU pointer to the shared memory.
	 * @param[in] size Size (in bytes) of the shared memory to allocate.
	 * @param[in] clear If `true` (the default), the memory contents will be filled with zeros.
	 *
	 * @returns `true` if the allocation succeeded, `false` otherwise.
	 * @ingroup cudaMemory
	*/
	inline bool cudaAllocMapped(
		void** cpuPtr, 
		void** gpuPtr, 
		size_t size, 
		bool clear = true
	) {
		if (!cpuPtr || !gpuPtr || size == 0)
			return false;

		// CUDA(cudaSetDeviceFlags(cudaDeviceMapHost));

		if (CUDA_FAILED(cudaHostAlloc(cpuPtr, size, cudaHostAllocMapped)))
			return false;

		if (CUDA_FAILED(cudaHostGetDevicePointer(gpuPtr, *cpuPtr, 0)))
			return false;

		if (clear)
			memset(*cpuPtr, 0, size);

		LogDebug(LOG_CUDA "cudaAllocMapped %zu bytes, CPU %p GPU %p\n", size, *cpuPtr, *gpuPtr);
		return true;
	}

#endif
