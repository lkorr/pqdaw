# Third-party dependencies

All are pinned. Permissive licences only. Add new ones in `cmake/PqDependencies.cmake` and list them here.

**Fetched at configure time** (FetchContent) as release archives pinned by SHA-256 (`URL_HASH`), never by git tag: a tag can be moved, the hash can't, and configuring needs no git. Where a project publishes its own release archive, that is what we use. Otherwise we use GitHub's tag archive (marked "tag archive" below). To bump a dependency, change the URL, download the archive, `sha256sum` it, and update the hash in both places. All are built statically with `/MT`. `BUILD_SHARED_LIBS` is forced OFF in the cache before the first dependency is declared.

| Name | Version | Archive (SHA-256) | Licence | Used by | Purpose |
|---|---|---|---|---|---|
| doctest | v2.4.11 | tag archive `github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz` (`632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186`) | MIT | tests | unit tests |
| zlib | 1.3.1 | release asset `zlib-1.3.1.tar.xz` (`38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32`) | zlib | (LLVM Support), ui | required by LLVM's exported link interface; deflate/inflate + CRC for the ui PNG encoder/decoder (screenshots, golden images) |
| zstd | 1.5.7 | release asset `zstd-1.5.7.tar.gz` (`eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`) | BSD-3-Clause | (LLVM Support) | required by LLVM's exported link interface (built from `build/cmake`, static, no programs) |
| nlohmann/json | 3.11.3 | release asset `json.tar.xz` (`d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d`) | MIT | document (`pq_doc`, public), ui (`pq_ui`, private), tests | theme and keymap files in `pq_ui` (T3.2: `assets/themes/*.json`, `assets/keymaps/*.json`; read once at startup or on a dev hot reload, ~0.3 ms); project-file schema migrations (generic DOM transforms of old `project.pqd` files, T3.5) and the `MigrationFn` signature; JSON parsing of Node-generated test fixtures. The current-schema load/save path uses pq_doc's own reader/writer (nlohmann's SAX lexer alone takes twice our whole load and its DOM 239-373 ms against the 250 ms budget, bench/RESULTS.md) |
| SDL3 | 3.4.18 (2026-10-02) | release asset `SDL3-3.4.18.tar.gz` (`9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`) | zlib | ui, app | window, events, Vulkan surface. Static; audio/camera/gpu/render/joystick/haptic/hidapi/sensor subsystems disabled |
| VulkanMemoryAllocator | v3.4.0 (2026-06-04) | tag archive `github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz` (`822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1`) | MIT | ui | GPU memory for textures, render targets, draw-list buffers (header-only; implementation in `src/ui/src/vma_impl.cpp`) |
| RtAudio | 6.0.1 (2023-08-01) | tag archive `github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz` (`7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123`) | MIT-style (RtAudio licence: MIT text plus a non-binding request to send modifications upstream) | engine | Audio device I/O: WASAPI (shared mode only in this version). Built with `/w`. DirectSound/JACK/Pulse/ALSA/OSS/Core off. **RtAudio's ASIO backend is never built** (T0.6-fix2; the `PQ_WITH_ASIO` option is gone): it would compile the Steinberg ASIO SDK files RtAudio bundles under `include/` (© Steinberg, proprietary SDK licence or GPLv3), which are not permissive. ASIO is pqdaw's own host (below). Still used for `Backend::RtAudioWasapi` (comparison, and the only WASAPI input path until our WASAPI backend does capture) |
| FreeType | 2.14.3 (2026-03-22) | official release tarball `freetype-2.14.3.tar.xz` from download.savannah.gnu.org, SourceForge mirror as fallback (`36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`) | FTL (FreeType License; chosen over the GPLv2 alternative) | ui | hinted grayscale glyph rasterisation for text. No zlib/bzip2/png/brotli/harfbuzz (`FT_DISABLE_*`). Its Windows version resource is dropped from the static lib. 2.15.0 (2026-10-11) is newer than the two-week rule allows |
| HarfBuzz | 14.5.1 (2026-09-30) | release asset `harfbuzz-14.5.1.tar.xz` (`7e2fa4e8c7c98e8d8140671f5772542afaaa6acccfbd746506886b6d85f7f8d6`) | MIT | ui | text shaping (GPOS kerning, GSUB contextual alternates, `tnum`). Built from the single-file `src/harfbuzz.cc` as `pq_harfbuzz` (no ICU/glib/graphite/subsetter). It shapes with its own OpenType functions; FreeType only rasterises |

"Release asset" means the file attached to the project's GitHub release `…/releases/download/<tag>/<file>`.

**Interoperability reimplementations** (our own code, our licence; no third-party files):

| Interface | Where | Position |
|---|---|---|
| ASIO host ABI | `src/engine/include/pq/engine/asio_abi.h`, `asio_device.*` (T0.6-fix2) | Clean-room: written from the publicly known binary layout every ASIO driver implements (COM-style vtable order, struct layouts, numeric constants), without copying, downloading or consulting Steinberg's ASIO SDK. Names are our own. Drivers are found in `HKLM\SOFTWARE\ASIO` and loaded with `CoCreateInstance`. "ASIO" is a Steinberg trademark, used only to name the interface. See docs/QUESTIONS.md Q?-T0.6-fix2-a |

**External** (installed on the machine, not fetched):

| Name | Version | Location | Licence | Used by | Purpose |
|---|---|---|---|---|---|
| LLVM (prebuilt) | 23.1.3 `clang+llvm-23.1.3-x86_64-pc-windows-msvc` | `C:\dev\llvm-23.1.3` (`PQ_LLVM_DIR`) | Apache-2.0 WITH LLVM-exception | jit, tests | ORC JIT + AOT codegen |
| Vulkan SDK (LunarG) | 1.4.357.0 | `$VULKAN_SDK` (`find_package(Vulkan 1.3 COMPONENTS glslc)`) | Apache-2.0 (headers, loader), glslc Apache-2.0 | ui | Vulkan headers + `vulkan-1` loader import lib. `glslc` compiles the UI shaders to SPIR-V at build time (embedded, no runtime files). Validation layer optional (`PQ_VK_VALIDATION=1`) |

**Committed** (in the repository):

| Name | Version | Source (SHA-256) | Licence | Used by | Purpose |
|---|---|---|---|---|---|
| Inter (font) | 4.1 (2024-11-16) | `extras/ttf/Inter-Regular.ttf` + `Inter-SemiBold.ttf` from `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip` (zip `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`; Regular `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`, 412 KB; SemiBold `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3`, 420 KB) | OFL-1.1 (`assets/fonts/OFL.txt`) | ui | UI typeface (SPEC §15). Static hinted TTFs (ttfautohint `fpgm`/`prep`/`cvt`), unmodified |

Planned (add when a task needs them; verify the licence and pin a version that is at least two weeks old): dr_libs (public domain/MIT-0), pffft or KissFFT (BSD), signalsmith-stretch (MIT), MTS-ESP (0BSD).
