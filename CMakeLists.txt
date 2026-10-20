# pgr4_recompiled - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pgr4_recompiled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Renderer selection:
#   PGR4_D3D12_RENDERER=ON   Plume/D3D12 native renderer in src/gpu (hooks the
#                            guest D3D calls; no Xenos/PM4 emulation).
#   PGR4_NATIVE_RENDERER=ON  Vulkan native renderer (needs PGR4_NATIVE_RENDERER_ROOT).
#   both OFF                 SDK Xenos GPU plugin (rexgpu-xenos).
option(PGR4_D3D12_RENDERER "Build the Plume-backed native D3D12 renderer" OFF)
option(PGR4_NATIVE_RENDERER "Build with the PGR4 Vulkan native renderer instead of Xenos" ON)
set(PGR4_PLUME_DIR "C:/Users/Bea/Documents/GH-II-Recomp/thirdparty/plume" CACHE PATH "Plume source tree")

include(generated/rexglue.cmake)

# Sources
set(PGR4RECOMPILED_SOURCES
    src/main.cpp
    src/music_volume_hook.cpp
    src/content/dlc.cpp
    src/profiler.cpp
)

if(PGR4_D3D12_RENDERER)
    list(APPEND PGR4RECOMPILED_SOURCES
        src/gpu/video.cpp
        src/gpu/imgui_drawer.cpp
        src/gpu/renderer.cpp
        src/gpu/shader_cache.cpp
        src/gpu/texture_convert.cpp
        src/gpu/hooks/device.cpp
        src/gpu/hooks/draw.cpp
        src/gpu/hooks/shader.cpp
        src/gpu/hooks/state.cpp
        src/gpu/fault_trace.cpp
    )
endif()

if(WIN32)
    add_executable(pgr4_recompiled WIN32 ${PGR4RECOMPILED_SOURCES})
else()
    add_executable(pgr4_recompiled ${PGR4RECOMPILED_SOURCES})
endif()

target_include_directories(pgr4_recompiled PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PGR4_D3D12_RENDERER)
    if(NOT WIN32)
        message(FATAL_ERROR "PGR4_D3D12_RENDERER targets D3D12 (Windows) only")
    endif()
    if(NOT EXISTS "${PGR4_PLUME_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Plume not found at ${PGR4_PLUME_DIR}; set PGR4_PLUME_DIR")
    endif()
    add_subdirectory("${PGR4_PLUME_DIR}" plume EXCLUDE_FROM_ALL)

    # Host shaders (ImGui overlay) compiled to shader model 5.1 DXBC headers
    # with fxc, which every D3D12 GPU accepts (DXIL needs SM 6.0 drivers).
    find_program(PGR4_FXC fxc
        HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.0.26100.0/x64"
        REQUIRED)
    set(PGR4_HOST_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/host_shaders")
    set(PGR4_HOST_SHADER_HEADERS)
    foreach(shader IN ITEMS imgui_vs:vs_5_1 imgui_ps:ps_5_1 present_vs:vs_5_1 present_ps:ps_5_1)
        string(REPLACE ":" ";" parts "${shader}")
        list(GET parts 0 stem)
        list(GET parts 1 profile)
        set(out "${PGR4_HOST_SHADER_DIR}/${stem}.dxbc.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${PGR4_HOST_SHADER_DIR}"
            COMMAND "${PGR4_FXC}" /nologo /T ${profile} /E main /O3 /Fh "${out}" /Vn g_${stem}_dxbc
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/${stem}.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/${stem}.hlsl"
            COMMENT "fxc ${stem}.hlsl (${profile})"
            VERBATIM)
        list(APPEND PGR4_HOST_SHADER_HEADERS "${out}")
    endforeach()
    target_sources(pgr4_recompiled PRIVATE ${PGR4_HOST_SHADER_HEADERS})
    target_include_directories(pgr4_recompiled PRIVATE "${PGR4_HOST_SHADER_DIR}")

    # Guest shader cache: the XenosRecomp HLSL in shaders/hlsl (see
    # tools/shaders/README.md) compiled to shader model 5.1 DXBC and packed
    # next to the executable.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB PGR4_GUEST_HLSL CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hlsl/*.hlsl")
    set(PGR4_SHADER_CACHE "${CMAKE_CURRENT_BINARY_DIR}/shader_cache_sm51.bin")
    add_custom_command(
        OUTPUT "${PGR4_SHADER_CACHE}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/build_shader_cache.py"
                --fxc "${PGR4_FXC}" --work "${CMAKE_CURRENT_BINARY_DIR}/shader_cache_work"
                --out "${PGR4_SHADER_CACHE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/build_shader_cache.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/shader_common_sm51.h"
                ${PGR4_GUEST_HLSL}
        COMMENT "Building the shader model 5.1 guest shader cache"
        VERBATIM)
    # Same HLSL compiled to DXIL (shader model 6.0) for GPUs that support it;
    # the renderer picks it at startup (ShaderCache::Load).
    set(PGR4_SHADER_CACHE_DXIL "${CMAKE_CURRENT_BINARY_DIR}/shader_cache_dxil.bin")
    add_custom_command(
        OUTPUT "${PGR4_SHADER_CACHE_DXIL}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/build_shader_cache.py"
                --dxil --work "${CMAKE_CURRENT_BINARY_DIR}/shader_cache_work_dxil"
                --out "${PGR4_SHADER_CACHE_DXIL}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/build_shader_cache.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/shader_common_sm51.h"
                ${PGR4_GUEST_HLSL}
        COMMENT "Building the shader model 6.0 (DXIL) guest shader cache"
        VERBATIM)
    add_custom_target(pgr4_shader_cache DEPENDS "${PGR4_SHADER_CACHE}" "${PGR4_SHADER_CACHE_DXIL}")
    add_dependencies(pgr4_recompiled pgr4_shader_cache)
    add_custom_command(TARGET pgr4_recompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PGR4_SHADER_CACHE}" "${PGR4_SHADER_CACHE_DXIL}"
                "$<TARGET_FILE_DIR:pgr4_recompiled>"
        VERBATIM)
    rexglue_setup_target(pgr4_recompiled)
    target_link_libraries(pgr4_recompiled PRIVATE plume d3d12 dxgi dxguid)
    target_compile_definitions(pgr4_recompiled PRIVATE PGR4_D3D12_RENDERER=1 PGR4_NATIVE_RENDERER=0)
elseif(PGR4_NATIVE_RENDERER)
    rexglue_setup_target(pgr4_recompiled)
    target_compile_definitions(pgr4_recompiled PRIVATE PGR4_D3D12_RENDERER=0 PGR4_NATIVE_RENDERER=1)
    include("${PGR4_NATIVE_RENDERER_ROOT}/native_renderer/integration/pgr4_native_renderer.cmake")
else()
    rexglue_setup_target(pgr4_recompiled GPU_PLUGINS xenos)
    target_compile_definitions(pgr4_recompiled PRIVATE PGR4_D3D12_RENDERER=0 PGR4_NATIVE_RENDERER=0)
endif()

# Launcher: game folder, GPU / shader format, title update and DLC packages.
# Static runtime and no extra DLLs, so it runs as is under Proton.
if(WIN32)
    add_executable(pgr4_launcher WIN32 tools/launcher/launcher.cpp tools/launcher/launcher.rc)
    target_compile_features(pgr4_launcher PRIVATE cxx_std_20)
    target_compile_definitions(pgr4_launcher PRIVATE UNICODE _UNICODE)
    set_property(TARGET pgr4_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    # Hybrid CRT: vcruntime linked in, the UCRT from the system (Windows 10+
    # and Wine both ship it). Own map file: the global /MAP names the game's.
    target_link_options(pgr4_launcher PRIVATE "LINKER:/MANIFEST:EMBED" "LINKER:/NODEFAULTLIB:libucrt.lib"
                        "LINKER:/MAP:pgr4_launcher.map")
endif()
