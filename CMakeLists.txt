cmake_minimum_required(VERSION 3.20)

project(D3DVideoEncoder
    VERSION 0.2.0
    LANGUAGES CXX
)

option(D3DVIDEOENCODER_BUILD_SAMPLE "Build D3DVideoEncoder samples" ON)
option(D3DVIDEOENCODER_BUILD_TESTS "Build D3DVideoEncoder tests" ON)
option(D3DVIDEOENCODER_ENABLE_WARNINGS "Enable compiler warnings" ON)
option(D3DVIDEOENCODER_FETCH_D3DHELPERS "Download D3D11Helper/D3D12Helper with FetchContent when targets are not already provided" ON)
option(D3DVIDEOENCODER_ENABLE_NVENC "Enable NVENC backend sources when NVIDIA Video Codec SDK is available" OFF)
option(D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE "Enable native D3D12 Video Encode backend sources" OFF)

set(D3DVIDEOENCODER_D3D11HELPER_DIR "" CACHE PATH "Optional path to a local D3D11Helper repository. If empty, FetchContent downloads it.")
set(D3DVIDEOENCODER_D3D12HELPER_DIR "" CACHE PATH "Optional path to a local D3D12Helper repository. If empty, FetchContent downloads it.")
set(D3DVIDEOENCODER_D3D11HELPER_GIT_REPOSITORY "https://github.com/Isasa2357/D3D11Helper.git" CACHE STRING "D3D11Helper Git repository URL")
set(D3DVIDEOENCODER_D3D12HELPER_GIT_REPOSITORY "https://github.com/Isasa2357/D3D12Helper.git" CACHE STRING "D3D12Helper Git repository URL")
set(D3DVIDEOENCODER_D3D11HELPER_GIT_TAG "main" CACHE STRING "D3D11Helper Git tag/branch/commit")
set(D3DVIDEOENCODER_D3D12HELPER_GIT_TAG "main" CACHE STRING "D3D12Helper Git tag/branch/commit")

set(D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR "" CACHE PATH "D3D11Processing shader directory. Auto-detected from D3D11Helper when empty.")
set(D3DVIDEOENCODER_DXC_RUNTIME_DIR "" CACHE PATH "Directory containing dxcompiler.dll and dxil.dll. Needed for samples/tests that use runtime shader compilation.")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(WARNING "D3DVideoEncoder currently targets Windows / Direct3D / Media Foundation only. No targets will be generated.")
    return()
endif()

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(D3DVideoEncoderDxcRuntime)

# -----------------------------------------------------------------------------
# External dependencies
# -----------------------------------------------------------------------------
# D3D11VideoEncoder uses D3D11Helper for D3D11 resource creation and Processing.
# D3D12Helper is still fetched because the public D3D12VideoEncoder API references
# D3D12Core and future D3D12 backends will use it. D3D12 is no longer routed
# through D3D11/Media Foundation interop.

set(D3D11HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(D3D11HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(NOT TARGET D3D11Helper::D3D11Helper)
    if(D3DVIDEOENCODER_D3D11HELPER_DIR)
        message(STATUS "Using local D3D11Helper: ${D3DVIDEOENCODER_D3D11HELPER_DIR}")
        add_subdirectory("${D3DVIDEOENCODER_D3D11HELPER_DIR}" "${CMAKE_BINARY_DIR}/_deps/D3D11Helper-build")
    elseif(D3DVIDEOENCODER_FETCH_D3DHELPERS)
        message(STATUS "Fetching D3D11Helper from ${D3DVIDEOENCODER_D3D11HELPER_GIT_REPOSITORY} (${D3DVIDEOENCODER_D3D11HELPER_GIT_TAG})")
        FetchContent_Declare(
            D3D11Helper
            GIT_REPOSITORY "${D3DVIDEOENCODER_D3D11HELPER_GIT_REPOSITORY}"
            GIT_TAG        "${D3DVIDEOENCODER_D3D11HELPER_GIT_TAG}"
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(D3D11Helper)
    else()
        message(FATAL_ERROR "D3D11Helper::D3D11Helper target was not found. Enable D3DVIDEOENCODER_FETCH_D3DHELPERS or set D3DVIDEOENCODER_D3D11HELPER_DIR.")
    endif()
endif()

if(NOT TARGET D3D12Helper::D3D12Helper)
    if(D3DVIDEOENCODER_D3D12HELPER_DIR)
        message(STATUS "Using local D3D12Helper: ${D3DVIDEOENCODER_D3D12HELPER_DIR}")
        add_subdirectory("${D3DVIDEOENCODER_D3D12HELPER_DIR}" "${CMAKE_BINARY_DIR}/_deps/D3D12Helper-build")
    elseif(D3DVIDEOENCODER_FETCH_D3DHELPERS)
        message(STATUS "Fetching D3D12Helper from ${D3DVIDEOENCODER_D3D12HELPER_GIT_REPOSITORY} (${D3DVIDEOENCODER_D3D12HELPER_GIT_TAG})")
        FetchContent_Declare(
            D3D12Helper
            GIT_REPOSITORY "${D3DVIDEOENCODER_D3D12HELPER_GIT_REPOSITORY}"
            GIT_TAG        "${D3DVIDEOENCODER_D3D12HELPER_GIT_TAG}"
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(D3D12Helper)
    else()
        message(FATAL_ERROR "D3D12Helper::D3D12Helper target was not found. Enable D3DVIDEOENCODER_FETCH_D3DHELPERS or set D3DVIDEOENCODER_D3D12HELPER_DIR.")
    endif()
endif()

if(NOT D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR)
    if(DEFINED d3d11helper_SOURCE_DIR)
        set(D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR "${d3d11helper_SOURCE_DIR}/shaders/D3D11Processing" CACHE PATH "" FORCE)
    elseif(DEFINED D3D11Helper_SOURCE_DIR)
        set(D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR "${D3D11Helper_SOURCE_DIR}/shaders/D3D11Processing" CACHE PATH "" FORCE)
    elseif(D3DVIDEOENCODER_D3D11HELPER_DIR)
        set(D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR "${D3DVIDEOENCODER_D3D11HELPER_DIR}/shaders/D3D11Processing" CACHE PATH "" FORCE)
    endif()
endif()

message(STATUS "D3D11Processing shaders: ${D3DVIDEOENCODER_D3D11_PROCESSING_SHADER_DIR}")
message(STATUS "D3D12 MediaFoundation backend: postponed / not built")

set(D3DVIDEOENCODER_PUBLIC_HEADERS
    include/D3DVideoEncoder/D3DVideoEncoder.hpp
    include/D3DVideoEncoder/D3D11VideoEncoder.hpp
    include/D3DVideoEncoder/D3D12VideoEncoder.hpp
    include/D3DVideoEncoder/D3DVideoEncoderDesc.hpp
    include/D3DVideoEncoder/D3D11VideoEncoderDesc.hpp
    include/D3DVideoEncoder/D3D12VideoEncoderDesc.hpp
    include/D3DVideoEncoder/D3DVideoEncoderError.hpp
    include/D3DVideoEncoder/D3DVideoEncoderTypes.hpp
)

set(D3DVIDEOENCODER_PRIVATE_HEADERS
    src/D3D11VideoEncoderImpl.hpp
    src/D3D12VideoEncoderImpl.hpp
    src/backend/IVideoEncoderBackend.hpp
    src/backend/MediaFoundationEncoderBackend.hpp
    src/backend/MediaFoundationRuntime.hpp
    src/backend/UnsupportedEncoderBackend.hpp
    src/input/D3D11VideoInputAdapter.hpp
    src/async/EncodeJobQueue.hpp
    src/surface/D3D11EncodeSurfacePool.hpp
    src/surface/EncodeSurface.hpp
    src/util/DebugLog.hpp
    src/util/HResult.hpp
    src/util/TimestampGenerator.hpp
)

set(D3DVIDEOENCODER_SOURCES
    src/D3DVideoEncoder.cpp
    src/D3DVideoEncoderDesc.cpp
    src/D3D11VideoEncoder.cpp
    src/D3D11VideoEncoderImpl.cpp
    src/D3D12VideoEncoder.cpp
    src/D3D12VideoEncoderImpl.cpp
    src/backend/MediaFoundationEncoderBackend.cpp
    src/backend/MediaFoundationRuntime.cpp
    src/backend/UnsupportedEncoderBackend.cpp
    src/input/D3D11VideoInputAdapter.cpp
    src/async/EncodeJobQueue.cpp
    src/surface/D3D11EncodeSurfacePool.cpp
    src/util/DebugLog.cpp
    src/util/HResult.cpp
    src/util/TimestampGenerator.cpp
)

add_library(D3DVideoEncoder STATIC
    ${D3DVIDEOENCODER_PUBLIC_HEADERS}
    ${D3DVIDEOENCODER_PRIVATE_HEADERS}
    ${D3DVIDEOENCODER_SOURCES}
)
add_library(D3DVideoEncoder::D3DVideoEncoder ALIAS D3DVideoEncoder)

target_include_directories(D3DVideoEncoder
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(D3DVideoEncoder
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(D3DVideoEncoder
    PUBLIC
        D3D11Helper::D3D11Helper
        D3D12Helper::D3D12Helper
    PRIVATE
        d3d11
        d3d12
        dxgi
        dxguid
        mf
        mfplat
        mfreadwrite
        mfuuid
)

if(MSVC)
    target_compile_options(D3DVideoEncoder PRIVATE /utf-8 /EHsc)
    if(D3DVIDEOENCODER_ENABLE_WARNINGS)
        target_compile_options(D3DVideoEncoder PRIVATE /W4 /permissive-)
    endif()
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header" FILES ${D3DVIDEOENCODER_PUBLIC_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${D3DVIDEOENCODER_PRIVATE_HEADERS} ${D3DVIDEOENCODER_SOURCES})

if(D3DVIDEOENCODER_BUILD_SAMPLE)
    add_subdirectory(sample/01_encode_d3d11_texture)
endif()

if(D3DVIDEOENCODER_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()
