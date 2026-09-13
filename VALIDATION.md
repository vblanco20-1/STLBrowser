# Validation record

Development machine: Windows 10, Ryzen 9 5950X, RTX 4070 Ti SUPER, NVIDIA driver 616.56, Vulkan SDK 1.4.341.1, Visual Studio 2026 / MSVC 19.51. Tests performed September 13, 2026.

**Executed checks**

- Debug and Release compilation with C++20, static SDL, and static MSVC runtime.
- CTest parser suite: binary/ASCII format handling, binary headers beginning with `solid`, AABB values, bounded chunk emission, normalization, truncated records, non-finite coordinates, empty geometry, cancellation, trailing-data warnings, Unicode paths, directory enumeration, and partial-list limits.
- Vulkan integration: offscreen thumbnail contains shaded geometry, memory-limit rejection, cancellation, session-image re-upload, completed exact full-view rendering, and resource retirement.
- Separate-queue and forced single-queue runs.
- Application interaction harness: select a model, change camera, resize, return to grid, change recursive scope, and capture rendered grid/viewer images.
- Final Debug interaction runs completed every harness stage with core and synchronization validation enabled: 1,708 frames / 0 errors on separate queues, and 1,444 frames / 0 errors in forced single-queue mode.
- Visual inspection of rendered grid and viewer captures using synthetic cubes, spheres, a torus, thin/tall parts, disconnected components, and a malformed file.
- Installed `dist/STLInspector.exe` launched from a different working directory, found its packaged shaders, completed all interaction stages, and exited with code 0 after 1,443 frames / 0 validation errors. Its direct DLL dependencies are Windows system libraries and `vulkan-1.dll`; no SDL or MSVC runtime DLL needs to be copied beside it.

**Large-file measurements**

Synthetic sphere: **1,000,000,084 bytes, 20,000,000 triangles**. This is generated data, not a representative user collection. MB/GB below are decimal.

| Run | Observed result |
|---|---|
| Release grid browsing, 20 seconds | Completed thumbnail; 2,881 frames; frame time p95 8.82 ms; peak process private memory 260.8 MB; peak VMA block allocation 67.1 MB |
| Release integration | Thumbnail 1.69 s; entire check sequence 4.18 s; peak sampled process private memory 1,000.9 MB; peak VMA blocks 805.3 MB |
| Release integration with core + synchronization validation | Thumbnail 1.96 s; entire check sequence 4.73 s; 0 validation errors; peak sampled private memory 1,009.1 MB; peak VMA blocks 805.3 MB |
| Final Release integration including resource retirement | Thumbnail 1.97 s; total 4.52 s; 0 validation errors; peak private memory 1,011.3 MB; VMA blocks returned from 805.3 MB to 67.1 MB after releasing the full model and retiring frame slots |

The integration sequence includes thumbnail generation, rejection under a deliberately insufficient viewer budget, cancellation, CPU-image cache upload, full model upload, and completion of progressive full-view drawing. The reported total therefore includes more than one parse/load. Full-view memory measurements are sampled around loading/rendering rather than an OS-level allocation trace. VMA block statistics include its allocator slack and host-visible buffers, while driver/system allocations are reflected only partly in those statistics; process private bytes are reported separately.

Synchronization validation initially found a swapchain acquisition layout-transition hazard. The transition now chains correctly from the acquire wait at color-attachment-output; the large-file validation run above completed with zero errors after the correction. Installed Galaxy overlay layers emit loader naming warnings independently of application validation.

**Coverage limits**

- Real UNC latency/disconnect behavior, removable media, and third-party STL collections have not been tested; no sample share or collection was supplied. Windows file reads use overlapped I/O and cancellation, but initial open/directory-provider calls can still block their worker.
- The desktop automation helper was unavailable. Grid/viewer screenshots and interaction checks were performed through an application-side test harness; native mouse drag and folder-picker automation have not been independently exercised.
- Linux, integrated GPUs, GPUs with less available memory, and alternate drivers are not validated. The single-queue code path is tested by forcing it on the development GPU.
- Full-view rendering uses exact geometry and a bounded number of chunks per frame. Interactive orbit speed varies with geometry size and GPU cost; huge views may finish progressively after the camera stops.
- Timing is from synthetic data on this development system and is not a guaranteed latency or frame-rate contract.

Test captures and logs are under the ignored `build/` directory. The fixture generator and test commands are documented in `README.md` so these checks can be repeated.
