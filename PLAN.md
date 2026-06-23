## What this codebase is

`jetson-inference` (NVIDIA Jetson DNN library by dusty-nv). It's structured as roughly three layers:

| Layer | Where | What |
|---|---|---|
| **Foundation utilities** | `utils/` (a git submodule!) | C++ helpers: filesystem, logging, command-line parsing, GStreamer camera/codec, OpenGL display, CUDA image ops, networking, threads |
| **DNN engine + wrappers** | `c/` | TensorRT base class (`tensorNet`) + per-network wrappers (`imageNet`, `detectNet`, `segNet`, `poseNet`, `actionNet`, `depthNet`, `backgroundNet`) and CUDA pre/post-processing kernels |
| **Apps** | `examples/`, `tools/`, `ros/`, `python/bindings/` | Small CLI apps that consume the library; ROS nodes; Python bindings |

The build is CMake → links against TensorRT (`nvinfer`, `nvinfer_plugin`, `nvonnxparser`), CUDA, GStreamer, optionally OpenCV/VPI.

## Important constraint: `utils/` is a git submodule

`utils/` has its own `.git` directory, meaning your comments inside `utils/` would live in a separate repo. If you ever run `git submodule update`, local-only annotations could conflict. We should pick a doc strategy upfront (see Q3 below).

---

## Proposed learning path (easy → hard)

I'd order it so each stage builds the mental model needed for the next:

**Stage 1 — Pure C++ foundations (no CUDA, no DNN)**
- `utils/commandLine.{h,cpp}` — argv parsing, used by every example
- `utils/filesystem.{h,cpp}` — paths, file checks
- `utils/logging.{h,cpp}` — LogInfo/LogError macros you'll see everywhere
- `utils/timespec.{h,cpp}` — timing helpers

**Stage 2 — CUDA fundamentals**
- `utils/cuda/cudaUtility.h` — `CUDA()` error-check macros
- `utils/cuda/cudaMappedMemory.h` — zero-copy CPU/GPU memory (key Jetson concept)
- `utils/cuda/cudaVector.h`, `cudaMath.h` — small math helpers
- A simple kernel pair: `cudaGrayscale.cu` then `cudaResize.cu`
- Color conversion: `cudaRGB.cu` then `cudaYUV-*.cu`
- `tensorConvert.cu` — converts image → NCHW float tensor (the bridge to TensorRT)

**Stage 3 — Image / video I/O**
- `utils/image/imageFormat.h` + `imageIO.{h,cpp}` (uses `stb_image`)
- `utils/video/videoSource.{h,cpp}` and `videoOutput.{h,cpp}` — the unified streaming abstraction
- `utils/camera/gstCamera.{h,cpp}` — GStreamer pipeline for CSI/USB cameras

**Stage 4 — TensorRT base class**
- `c/tensorNet.{h,cpp}` — engine load, serialization, binding, profiling. This is the most important file in `c/`.

**Stage 5 — One DNN wrapper end-to-end**
- `c/imageNet.{h,cpp}` + `examples/imagenet/imagenet.cpp` — the simplest classifier; trace one image from disk → CUDA tensor → TensorRT → top-K output

**Stage 6 — Build out from there**
- `c/detectNet` (adds bounding-box decoding in CUDA), then `segNet`, `poseNet`, `depthNet`

**Stage 7 (optional/advanced)** — display (`utils/display/glDisplay`), tracking, TensorRT plugins under `c/plugins/`

## Plan: Inline-comment walkthrough of `jetson-inference`

### Conventions
- **Style:** inline `//` comments, light density — each file gets:
  1. A multi-line header block at the top: what the file does, where it fits in the pipeline, key dependencies.
  2. Section comments (~3–8 per file) marking the major functions/blocks.
  3. No line-by-line annotation. No restating obvious code.
- **Tone:** explanatory, written for someone comfortable with C++ but new to CUDA/TensorRT. C++ idioms get no explanation; CUDA/TensorRT concepts get a one-line "this is what X is" the first time they appear.
- **No code changes** — only comments. No refactors, no renames.

### Submodule warning (`utils/`)
`utils/` is a separate git repo (submodule). Inline edits there will:
- Live in the submodule's own working tree, not the parent repo.
- Show as `modified content` in the parent's `git status` (you already see this: `m utils` in your gitStatus).
- Get wiped if you ever run `git submodule update --remote` or `--force`.

**Mitigation options** (decide later, not blocking):
- Commit comments inside the submodule on a local branch (`git -C utils checkout -b learning-notes`)
- Or fork the submodule on GitHub and point `.gitmodules` at your fork
- Or just accept the risk and re-apply on conflict

I'll flag this again when we first touch a `utils/` file.

### File order (Stage 1 → Stage 7)

**Stage 1 — Pure C++ foundations** *(starts here)*
1. `utils/commandLine.h`
2. `utils/commandLine.cpp`
3. `utils/filesystem.h` + `utils/filesystem.cpp`
4. `utils/logging.h` + `utils/logging.cpp`
5. `utils/timespec.h` + `utils/timespec.cpp`

**Stage 2 — CUDA fundamentals**
6. `utils/cuda/cudaUtility.h` — error-check macros, the first CUDA file to read
7. `utils/cuda/cudaMappedMemory.h` — zero-copy memory (critical Jetson concept)
8. `utils/cuda/cudaVector.h` + `cudaMath.h`
9. `utils/cuda/cudaGrayscale.{h,cu}` — first real kernel
10. `utils/cuda/cudaResize.{h,cu}`
11. `utils/cuda/cudaRGB.{h,cu}`
12. `utils/cuda/cudaYUV.h` + one of the `cudaYUV-*.cu` variants
13. `c/tensorConvert.{h,cu}` — image → NCHW float tensor

**Stage 3 — Image / video I/O**
14. `utils/image/imageFormat.h`
15. `utils/image/imageIO.{h,cpp}` + `imageLoader/imageWriter`
16. `utils/video/videoSource.{h,cpp}` + `videoOutput.{h,cpp}`
17. `utils/video/videoOptions.{h,cpp}`
18. `utils/camera/gstCamera.{h,cpp}`

**Stage 4 — TensorRT core**
19. `c/tensorNet.h`
20. `c/tensorNet.cpp` ← the most important file in the project
21. `c/modelDownloader.{h,cpp}`

**Stage 5 — First DNN, end-to-end**
22. `c/imageNet.h` + `c/imageNet.cpp`
23. `examples/imagenet/imagenet.cpp`

**Stage 6 — Other DNN wrappers**
24. `c/detectNet.{h,cpp,cu}` + `examples/detectnet/detectnet.cpp`
25. `c/segNet.{h,cpp,cu}` + example
26. `c/poseNet.{h,cpp}` + example
27. `c/depthNet`, `c/actionNet`, `c/backgroundNet` (lighter pass)

**Stage 7 — Optional / advanced**
- `utils/display/glDisplay.{h,cpp}` (OpenGL)
- `c/tracking/objectTracker*`
- `c/plugins/*` (custom TensorRT plugins)
- ROS nodes (`ros/src/node_*`), Python bindings (`python/bindings/Py*`)

### Pace question (still open)

You didn't pick a pace. My recommendation given "light annotation": **3–5 files per session**, organized as a logical group. So a typical session looks like "finish Stage 1" or "do all of cuda image-ops in Stage 2." That keeps each session under ~1 hr but still leaves something concrete behind.

### What I'll do per session
1. Tell you which file(s) we're on.
2. Read the file in full.
3. Write the header block + section comments via `Edit`.
4. End-of-turn: one sentence on what changed, one sentence on what's next.

