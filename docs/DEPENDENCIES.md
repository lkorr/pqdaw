# Third-party dependencies

All are pinned. Add new ones in `cmake/PqDependencies.cmake` and list them here. Permissive licences only.

| Name | Version | Licence | Used by | Purpose |
|---|---|---|---|---|
| LLVM (prebuilt, external) | 23.1.3 `clang+llvm-23.1.3-x86_64-pc-windows-msvc` at `C:\dev\llvm-23.1.3` | Apache-2.0 WITH LLVM-exception | jit, tests | ORC JIT + AOT codegen |
| doctest | v2.4.11 | MIT | tests | unit tests |
| zlib | v1.3.1 | zlib | (LLVM Support), ui | required by LLVM's exported link interface; deflate/inflate + CRC for the ui PNG encoder/decoder (screenshots, golden images) |
| zstd | v1.5.7 | BSD-3-Clause | (LLVM Support) | required by LLVM's exported link interface |
| nlohmann/json | v3.11.3 (release tarball `json.tar.xz`, SHA-256 pinned) | MIT | tests (tuning fixtures) | JSON parsing of Node-generated fixtures; later project files |
| RtAudio | 6.0.1 (2023-08-01; GitHub tag tarball, SHA-256 pinned) | MIT-style (RtAudio licence: MIT text plus a non-binding request to send modifications upstream) | engine | Audio device I/O: WASAPI (shared mode only in this version). Static, `/MT`, built with `/w`. DirectSound/JACK/Pulse/ALSA/OSS/Core off. **ASIO is opt-in** (`-DPQ_WITH_ASIO=ON`, default OFF): it compiles the Steinberg ASIO SDK 2.3 files RtAudio bundles under `include/` (`asio.h`, `asiodrvr.h`, `iasiodrv.h`, `asiodrivers.cpp`, `asiolist.cpp`, `asio.cpp`, © Steinberg), which are under Steinberg's proprietary ASIO SDK licence (or GPLv3 for the 2025 SDK), not a permissive one; see docs/QUESTIONS.md (T0.6). `iasiothiscallresolver.cpp` is Ross Bencina's (permissive), irrelevant on x64 MSVC |

Planned (add when the task needs them; verify the licence and pin a version that is at least two weeks old):
SDL3 (zlib), VulkanMemoryAllocator (MIT), FreeType (FTL), HarfBuzz (MIT), Inter font (OFL), dr_libs (public domain/MIT-0), pffft or KissFFT (BSD), signalsmith-stretch (MIT), MTS-ESP (0BSD).
| Vulkan SDK (LunarG, external) | 1.4.357.0 at `$VULKAN_SDK` (`find_package(Vulkan 1.3 COMPONENTS glslc)`) | Apache-2.0 (headers, loader), glslc Apache-2.0 | ui | Vulkan headers + `vulkan-1` loader import lib; `glslc` compiles the UI shaders to SPIR-V at build time (embedded, no runtime files). Validation layer optional (`PQ_VK_VALIDATION=1`) |
| SDL3 | `release-3.4.18` (2026-10-02) | zlib | ui, app | window, events, Vulkan surface. Static, `/MT`; audio/camera/gpu/render/joystick/haptic/hidapi/sensor subsystems disabled |
| VulkanMemoryAllocator | `v3.4.0` (2026-06-04) | MIT | ui | GPU memory for textures, render targets, draw-list buffers (header-only; implementation in `src/ui/src/vma_impl.cpp`) |
| FreeType | `VER-2-14-3` (2026-03-22) | FTL (FreeType License; chosen over the GPLv2 alternative) | ui | hinted grayscale glyph rasterisation for text. Static `/MT`, no zlib/bzip2/png/brotli/harfbuzz (`FT_DISABLE_*`); its Windows version resource is dropped from the static lib. 2.15.0 (2026-10-11) is newer than the two-week rule allows |
| HarfBuzz | `14.5.1` (2026-09-30) | MIT | ui | text shaping (GPOS kerning, GSUB contextual alternates, `tnum`). Built from the single-file `src/harfbuzz.cc` as `pq_harfbuzz` (no ICU/glib/graphite/subsetter); shapes with its own OpenType functions, FreeType only rasterises |
| Inter (font, committed) | 4.1 (2024-11-16), `extras/ttf/Inter-Regular.ttf` + `Inter-SemiBold.ttf` from `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip` (zip SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`) | OFL-1.1 (`assets/fonts/OFL.txt`) | ui | UI typeface (SPEC §15). Static hinted TTFs (ttfautohint `fpgm`/`prep`/`cvt`), unmodified. SHA-256: Regular `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` (412 KB), SemiBold `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` (420 KB) |

Planned (add when the task needs them; verify the licence and pin a version that is at least two weeks old):
FreeType (FTL), HarfBuzz (MIT), Inter font (OFL), dr_libs (public domain/MIT-0), nlohmann/json (MIT), pffft or KissFFT (BSD), signalsmith-stretch (MIT), MTS-ESP (0BSD).
