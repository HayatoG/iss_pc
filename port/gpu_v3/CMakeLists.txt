# tlg GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(tlggpu LANGUAGES C CXX)
# gpu_v3: shared third-party sources of renderer v1 (this tree only adds stb).
set(GPU_TP ${CMAKE_CURRENT_SOURCE_DIR}/../gpu/third_party)
set(GPU_V3_TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# port: link-time optimization (TLG_LTO, default on) and profile-guided optimization
# (TLG_PGO: off, generate = instrumented build writing into TLG_PGO_DIR, use = build with that
# profile). Covers tlggpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(TLG_LTO "Link-time optimization of tlggpu" ON)
set(TLG_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(TLG_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (TLG_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT tlg_ipo_ok OUTPUT tlg_ipo_error)
    if (tlg_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${tlg_ipo_error}")
    endif()
endif()
if (TLG_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${TLG_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${TLG_PGO_DIR})
    add_compile_definitions(TLG_PGO_GENERATE)
elseif (TLG_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, tlggpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${TLG_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# tlg (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE ${GPU_TP}/magic_enum/include
                                                    ${GPU_TP}/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(${GPU_TP}/miniz miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${GPU_TP}/sirit sirit EXCLUDE_FROM_ALL)



# Host shaders -> GLSL source strings compiled at run time by glslang (axylpup's tree, shadPS4 0.17).
add_subdirectory(shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${HOST_SHADERS_INCLUDE})
find_package(glslang CONFIG REQUIRED)

# gpu_v3: FSR 4 / Streamline DLSS are not built (see shim/tlg_vendor_stubs.cpp).

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)

# port: the shader cache key: a hash of every source that shapes the generated SPIR-V. The
# shader cache directory carries it, so a changed recompiler starts a fresh cache instead of
# reusing shaders translated by the old code (build.sh configures on every build).
file(GLOB_RECURSE TLG_SHADER_KEY_SOURCES shadps4/shader_recompiler/*.cpp shadps4/shader_recompiler/*.h)
list(APPEND TLG_SHADER_KEY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/amdgpu/resource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp)
list(SORT TLG_SHADER_KEY_SOURCES)
set(TLG_SHADER_KEY_DATA "")
foreach(source ${TLG_SHADER_KEY_SOURCES})
    file(SHA1 ${source} source_hash)
    string(APPEND TLG_SHADER_KEY_DATA ${source_hash})
endforeach()
string(SHA1 TLG_SHADER_KEY "${TLG_SHADER_KEY_DATA}")
string(SUBSTRING ${TLG_SHADER_KEY} 0 12 TLG_SHADER_KEY)
add_compile_definitions(TLG_SHADER_CACHE_KEY="${TLG_SHADER_KEY}")
message(STATUS "Shader cache key: ${TLG_SHADER_KEY}")
# gpu_v3: the vendor-SDK parts (Streamline DLSS, DLSS 5, FSR 4) are replaced by stubs (shim/tlg_vendor_stubs.cpp).
list(FILTER VIDEO_CORE EXCLUDE REGEX "(renderdoc|vk_dlss|vk_dlss5|tlg_fsr4)\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shadps4/common/fault_monitor.cpp shadps4/common/reserved_memory.cpp shadps4/common/stb.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for tlg-probe.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(TLGGPU_KIND STATIC)
else()
    set(TLGGPU_KIND SHARED)
endif()
add_library(tlggpu ${TLGGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(tlggpu host_shaders)
# In-game settings menu: Dear ImGui (${GPU_TP}/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (${GPU_TP}/fonts).
target_sources(tlggpu PRIVATE ${GPU_TP}/imgui/imgui.cpp ${GPU_TP}/imgui/imgui_draw.cpp
    ${GPU_TP}/imgui/imgui_tables.cpp ${GPU_TP}/imgui/imgui_widgets.cpp
    ${GPU_TP}/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(tlggpu PRIVATE ${GPU_TP}/imgui ${GPU_TP}/imgui/backends)
target_compile_definitions(tlggpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    TLG_FONT_PATH="${GPU_TP}/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/tlg_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${GPU_TP}/fonts/DejaVuSans.ttf)
target_include_directories(tlggpu PRIVATE shim shadps4 ${SHADER_INCLUDE} ${GPU_TP}/gcn/include ${GPU_TP}/half/include
    ${GPU_V3_TP}/stb)
target_compile_definitions(tlggpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(tlggpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against tlg-probe.
if (NOT WIN32)
    target_link_options(tlggpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
# glslang's own CMake package: SPIRV and the default resource limits are separate targets in
# older releases only.
target_link_libraries(tlggpu PRIVATE glslang::glslang)
foreach(glslang_part glslang::SPIRV glslang::glslang-default-resource-limits)
    if (TARGET ${glslang_part})
        target_link_libraries(tlggpu PRIVATE ${glslang_part})
    endif()
endforeach()
target_link_libraries(tlggpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
target_include_directories(tlggpu PRIVATE ${GPU_TP}/xbyak)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(tlggpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tlggpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/tlg_win_compat.h)
    target_link_libraries(tlggpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm pdh)
else()
    target_link_libraries(tlggpu PUBLIC PkgConfig::X11)
endif()

# tlg (Windows): tlggpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    file(GLOB TLG_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    file(GLOB TLG_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${TLG_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(tlg-probe ../src/probe.c ${TLG_RUNTIME} ../src/compat_win.c ../src/cpu_check.c ../src/vulkan_smoke.c
        ../src/isa_rewrite.c ../src/isa_emulate.c
        ../packaging/windows/tlg-probe.rc)
    set_target_properties(tlg-probe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    if (DEFINED ENV{TLG_BUILD_COMMIT})
        target_compile_definitions(tlg-probe PRIVATE TLG_BUILD_COMMIT="$ENV{TLG_BUILD_COMMIT}")
    endif()
    target_compile_options(tlg-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    target_include_directories(tlg-probe PRIVATE .. ../src)
    # tlg: libSceFont (runtime_gr2_font.c) renders glyphs with FreeType.
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    target_link_libraries(tlg-probe PRIVATE tlggpu atrac9 PkgConfig::FREETYPE onecore bcrypt ws2_32 -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(tlg-probe PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
endif()
target_link_options(tlg-probe PRIVATE -Wl,--error-limit=0)
