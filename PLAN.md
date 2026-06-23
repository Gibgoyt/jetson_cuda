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

