# STL Inspector

A Windows-first C++20 / Dear ImGui / Vulkan browser for large collections of STL printing files.

**Run**

Launch `dist/STLInspector.exe`, choose **Open folder**, or enter a local/UNC path. You can also pass a directory on the command line:

```powershell
.\dist\STLInspector.exe "D:\Printing models"
```

The folder tree expands on demand. The grid initially shows files directly in the selected folder; **Subfolders** enables recursive browsing. Filter by filename, sort by name/size/date, or adjust tile size. Visible thumbnails are generated before nearby files. **Pause previews** cancels pending thumbnail work; resuming allows it to be requested again. Right-click a card to retry a failed thumbnail.

Click a thumbnail to open the full viewer. Left-drag orbits, the wheel zooms, and middle/right-drag pans. **Fit**, **Front**, **Right**, and **Top** set the camera. **Back to grid** or Escape returns to browsing. Dimensions are AABB dimensions, with millimeters explicitly assumed because STL contains no unit definition. Triangle counts and malformed/degenerate geometry warnings are shown; this application does not certify printability or repair meshes.

Each thumbnail card and the full viewer have an **Open in Explorer** button. It opens the containing folder and selects the STL, including files with spaces or Unicode names. Explorer requests run in the background; failures are reported in the status bar. `__MACOSX` directories are ignored in the folder tree, recursive browsing, and prebuild scans.

**Large files and memory**

- Thumbnails use two streaming passes: compute bounds, then render exact geometry in bounded chunks. Thumbnail meshes are never retained in full.
- The UI and background rendering use separate graphics queues when supported. Single-queue hardware serializes submissions and throttles worker submissions.
- The default GPU target is 4,000 MB, with a 3,500 MB allocator admission ceiling to leave headroom. Full-view geometry initially has a 2,300 MB allowance. A model that exceeds the allowance is rejected with an estimate and controls to raise the limits explicitly.
- The CPU thumbnail cache is limited to 1,000 MB and the GPU thumbnail cache to 400 MB. Process memory is monitored against the 8 GB RAM target. Bounded read/upload buffers and two workers keep normal use far below that target. Vulkan allocation statistics conservatively include host-visible staging allocations as well as device allocations.
- Full-view geometry uses 36 bytes per renderable triangle. It is uploaded in chunks and kept resident until the viewer closes. Expensive views render progressively in exact chunks; a completed image remains visible while a changed camera is rendered.
- Thumbnails and metadata are session-only. Evicted CPU/GPU thumbnails are regenerated when revisited. **Prebuild tree** stops at session-cache capacity, and cannot make an unlimited tree permanently hot.
- Directory scans stop at 100,000 entries per listing and label partial results. Select a smaller subtree when this limit is reached. Links/junctions are skipped to prevent cycles.
- File reads and scans run off the UI thread. Windows overlapped reads support cancellation; provider-dependent directory enumeration and initial file opening may still take time on an unavailable network share. Refresh revalidates the selected scope. There is no whole-tree file watcher.

**Build**

Requirements: a C++20 compiler, CMake 3.25+, a Vulkan SDK with `glslc`, and a Vulkan 1.3 GPU/driver supporting dynamic rendering, synchronization2, and timeline semaphores. The supplied Windows preset targets Visual Studio 2026 x64. Dependencies are vendored; the neighboring Vulkan Guide repository is not needed to build or run.

```powershell
cmake --preset windows
cmake --build --preset release --parallel 2 -- /p:CL_MPCount=4
cmake --install build --config Release --prefix dist
```

The executable, shaders, documentation, and license notices are installed together. SDL and the MSVC runtime are linked statically. Keep the `shaders` directory beside the executable. System Segoe UI is used when available, with ImGui's built-in font as the fallback; no system font is distributed.

Some process launchers provide both `Path` and `PATH`, which MSBuild rejects. The optional Python wrapper normalizes only the child environment:

```powershell
python tools/build.py cmake --preset windows
python tools/build.py cmake --build --preset debug --parallel 2 -- /p:CL_MPCount=4
python tools/build.py ctest --preset debug
```

The core uses portable filesystem/STL interfaces. Linux does not yet have a tested build/picker/package; the Windows folder picker and cancellable I/O implementation are isolated in `src/platform.cpp`.

Project C++ follows the Vulkan Guide layout: four-space indentation, function braces on their own line, braced control flow, and blank lines between logical steps. The root `.clang-format` captures the style (validated with clang-format 21). Preserve those logical blank lines when editing. Format application code and tests in PowerShell:

```powershell
$formatFiles = Get-ChildItem src/*.cpp, src/*.hpp, tests/*.cpp
clang-format -i $formatFiles.FullName
```

Bundled code under `third_party/` keeps its upstream formatting. Shaders follow the same layout and are formatted separately because clang-format does not parse GLSL interface blocks correctly.

**Validation and diagnostics**

Debug builds enable Vulkan core and synchronization validation. Release builds opt in with `--validation`. `--single-queue` exercises fallback scheduling. Console output includes errors and final frame/memory statistics; the application itself never writes a thumbnail cache to disk.

```powershell
ctest --preset debug
python tools/fixtures.py test-output/gallery
.\build\bin\Debug\STLInspector.exe test-output/gallery --validation --exercise --smoke 15 --screenshot build/gallery.ppm
.\build\bin\Debug\STLInspector.exe --self-test build/test-output/tetra.stl --validation --single-queue
```

`--exercise` tests model selection, a camera change, resizing, return to the grid, and recursive-scope changes inside the running application. `--self-test` verifies rendered pixels, budget rejection, cancellation, CPU-image re-upload, and complete full-view rendering. `--screenshot` is an explicit test capture to PPM; it is independent of thumbnail caching.

Generate a 1 GB sphere for performance testing (requires Python NumPy and sufficient disk space):

```powershell
python tools/fixtures.py test-output/large --large-mb 1000
.\build\bin\Release\STLInspector.exe test-output/large --smoke 20
.\build\bin\Release\STLInspector.exe --self-test test-output/large/large_sphere.stl --screenshot build/large.ppm
```

See `VALIDATION.md` for observed results and remaining platform/network coverage. `DESIGN.md` records the design decisions; implementation uses two workers with per-worker Vulkan command pools/fences and a shared completion timeline rather than a separate GPU service thread.

**Source map**

| File | Responsibility |
|---|---|
| `src/application.cpp` | ImGui browser/viewer, jobs, session caches, test interaction path |
| `src/renderer.cpp` | Vulkan setup, queues, offscreen rendering, resource lifetime, frame capture |
| `src/stl.cpp` | Binary/ASCII streaming parser, AABB, normalized geometry chunks |
| `src/platform.cpp` | Unicode paths, Windows picker, overlapped I/O, memory/font adapters |
| `src/browser.cpp` | Bounded enumeration and priority worker executor |
| `tests/stl_tests.cpp` | Parser, streaming, cancellation, path, and enumeration tests |

Vulkan setup follows the structure of the requested `vulkan-guide-2` reference, with dedicated streaming/resource-lifetime code for this application. Attribution is in `THIRD_PARTY_NOTICES.md` and `licenses/`.
