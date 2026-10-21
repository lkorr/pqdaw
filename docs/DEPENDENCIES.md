# Third-party dependencies

All are pinned. Add new ones in `cmake/PqDependencies.cmake` and list them here. Permissive licences only.

| Name | Version | Licence | Used by | Purpose |
|---|---|---|---|---|
| LLVM (prebuilt, external) | 23.1.3 `clang+llvm-23.1.3-x86_64-pc-windows-msvc` at `C:\dev\llvm-23.1.3` | Apache-2.0 WITH LLVM-exception | jit, tests | ORC JIT + AOT codegen |
| doctest | v2.4.11 | MIT | tests | unit tests |
| zlib | v1.3.1 | zlib | (LLVM Support), ui | required by LLVM's exported link interface; deflate/inflate + CRC for the ui PNG encoder/decoder (screenshots, golden images) |
| zstd | v1.5.7 | BSD-3-Clause | (LLVM Support) | required by LLVM's exported link interface |
| nlohmann/json | v3.11.3 (release tarball `json.tar.xz`, SHA-256 pinned) | MIT | tests (tuning fixtures) | JSON parsing of Node-generated fixtures; later project files |

Planned (add when the task needs them; verify the licence and pin a version that is at least two weeks old):
RtAudio (MIT-like; ships the ASIO host files), SDL3 (zlib), VulkanMemoryAllocator (MIT), FreeType (FTL), HarfBuzz (MIT), Inter font (OFL), dr_libs (public domain/MIT-0), pffft or KissFFT (BSD), signalsmith-stretch (MIT), MTS-ESP (0BSD).
| Vulkan SDK (LunarG, external) | 1.4.357.0 at `$VULKAN_SDK` (`find_package(Vulkan 1.3 COMPONENTS glslc)`) | Apache-2.0 (headers, loader), glslc Apache-2.0 | ui | Vulkan headers + `vulkan-1` loader import lib; `glslc` compiles the UI shaders to SPIR-V at build time (embedded, no runtime files). Validation layer optional (`PQ_VK_VALIDATION=1`) |
| SDL3 | `release-3.4.18` (2026-10-02) | zlib | ui, app | window, events, Vulkan surface. Static, `/MT`; audio/camera/gpu/render/joystick/haptic/hidapi/sensor subsystems disabled |
| VulkanMemoryAllocator | `v3.4.0` (2026-06-04) | MIT | ui | GPU memory for textures, render targets, draw-list buffers (header-only; implementation in `src/ui/src/vma_impl.cpp`) |

Planned (add when the task needs them; verify the licence and pin a version that is at least two weeks old):
RtAudio (MIT-like; ships the ASIO host files), FreeType (FTL), HarfBuzz (MIT), Inter font (OFL), dr_libs (public domain/MIT-0), nlohmann/json (MIT), pffft or KissFFT (BSD), signalsmith-stretch (MIT), MTS-ESP (0BSD).
