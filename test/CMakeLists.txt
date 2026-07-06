set(D3DVIDEOENCODER_TEST_TARGETS
    D3DVideoEncoder_test_timestamp_generator
    D3DVideoEncoder_test_types
    D3DVideoEncoder_test_desc_validation
    D3DVideoEncoder_test_hresult
    D3DVideoEncoder_test_encode_job_queue
    D3DVideoEncoder_test_d3d12_encode_job_queue
    D3DVideoEncoder_test_d3d12_unsupported_backend
    D3DVideoEncoder_test_d3d11_encode_smoke
    D3DVideoEncoder_test_d3d11_encode_async_smoke
    D3DVideoEncoder_test_d3d11_mf_capabilities
    D3DVideoEncoder_test_d3d11_hevc_p010_smoke
)

add_executable(D3DVideoEncoder_test_timestamp_generator
    test_timestamp_generator.cpp
)
target_link_libraries(D3DVideoEncoder_test_timestamp_generator PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_timestamp_generator PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(D3DVideoEncoder_test_types
    test_types.cpp
)
target_link_libraries(D3DVideoEncoder_test_types PRIVATE D3DVideoEncoder::D3DVideoEncoder)

add_executable(D3DVideoEncoder_test_desc_validation
    test_desc_validation.cpp
)
target_link_libraries(D3DVideoEncoder_test_desc_validation PRIVATE D3DVideoEncoder::D3DVideoEncoder)

add_executable(D3DVideoEncoder_test_hresult
    test_hresult.cpp
)
target_link_libraries(D3DVideoEncoder_test_hresult PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_hresult PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(D3DVideoEncoder_test_encode_job_queue
    test_encode_job_queue.cpp
)
target_link_libraries(D3DVideoEncoder_test_encode_job_queue PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_encode_job_queue PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(D3DVideoEncoder_test_d3d12_encode_job_queue
    test_d3d12_encode_job_queue.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d12_encode_job_queue PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_d3d12_encode_job_queue PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(D3DVideoEncoder_test_d3d12_unsupported_backend
    test_d3d12_unsupported_backend.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d12_unsupported_backend PRIVATE D3DVideoEncoder::D3DVideoEncoder)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/test_config.hpp"
    @ONLY
)

add_executable(D3DVideoEncoder_test_d3d11_encode_smoke
    test_d3d11_encode_smoke.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d11_encode_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_d3d11_encode_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(D3DVideoEncoder_test_d3d11_encode_async_smoke
    test_d3d11_encode_async_smoke.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d11_encode_async_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_d3d11_encode_async_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(D3DVideoEncoder_test_d3d11_mf_capabilities
    test_d3d11_mf_capabilities.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d11_mf_capabilities PRIVATE D3DVideoEncoder::D3DVideoEncoder)

add_executable(D3DVideoEncoder_test_d3d11_hevc_p010_smoke
    test_d3d11_hevc_p010_smoke.cpp
)
target_link_libraries(D3DVideoEncoder_test_d3d11_hevc_p010_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
target_include_directories(D3DVideoEncoder_test_d3d11_hevc_p010_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)


if(D3DVIDEOENCODER_HAS_NVENC)
    list(APPEND D3DVIDEOENCODER_TEST_TARGETS
        D3DVideoEncoder_test_nvenc_capabilities
        D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke
        D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke
    )

    add_executable(D3DVideoEncoder_test_nvenc_capabilities
        test_nvenc_capabilities.cpp
    )
    target_link_libraries(D3DVideoEncoder_test_nvenc_capabilities PRIVATE D3DVideoEncoder::D3DVideoEncoder)

    add_executable(D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke
        test_nvenc_d3d11_h264_mp4_smoke.cpp
    )
    target_link_libraries(D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
    target_include_directories(D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke
        test_nvenc_d3d12_rgba_h264_mp4_smoke.cpp
    )
    target_link_libraries(D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
    target_include_directories(D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

if(D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE)
    list(APPEND D3DVIDEOENCODER_TEST_TARGETS
        D3DVideoEncoder_test_d3d12_video_encode_capabilities
    )

    add_executable(D3DVideoEncoder_test_d3d12_video_encode_capabilities
        test_d3d12_video_encode_capabilities.cpp
    )
    target_link_libraries(D3DVideoEncoder_test_d3d12_video_encode_capabilities PRIVATE D3DVideoEncoder::D3DVideoEncoder)
    target_include_directories(D3DVideoEncoder_test_d3d12_video_encode_capabilities PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

if(D3DVIDEOENCODER_HAS_NVENC)
    foreach(tgt IN LISTS D3DVIDEOENCODER_TEST_TARGETS)
        target_compile_definitions(${tgt} PRIVATE D3DVIDEOENCODER_TEST_HAS_NVENC=1)
    endforeach()
endif()
if(D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE)
    list(APPEND D3DVIDEOENCODER_TEST_TARGETS
        D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke
    )

    add_executable(D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke
        test_d3d12_video_encode_h264_nv12_smoke.cpp
    )
    target_link_libraries(D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke PRIVATE D3DVideoEncoder::D3DVideoEncoder)
    target_include_directories(D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )

    foreach(tgt IN LISTS D3DVIDEOENCODER_TEST_TARGETS)
        target_compile_definitions(${tgt} PRIVATE D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE=1)
    endforeach()
endif()

if(MSVC)
    foreach(tgt IN LISTS D3DVIDEOENCODER_TEST_TARGETS)
        target_compile_options(${tgt} PRIVATE /utf-8 /EHsc)
    endforeach()
endif()

foreach(tgt IN LISTS D3DVIDEOENCODER_TEST_TARGETS)
    d3dvideoencoder_copy_dxc_runtime(${tgt})
endforeach()

add_test(NAME TimestampGenerator COMMAND D3DVideoEncoder_test_timestamp_generator)
add_test(NAME Types COMMAND D3DVideoEncoder_test_types)
add_test(NAME DescValidation COMMAND D3DVideoEncoder_test_desc_validation)
add_test(NAME HResult COMMAND D3DVideoEncoder_test_hresult)
add_test(NAME EncodeJobQueue COMMAND D3DVideoEncoder_test_encode_job_queue)
add_test(NAME D3D12EncodeJobQueue COMMAND D3DVideoEncoder_test_d3d12_encode_job_queue)
add_test(NAME D3D12UnsupportedBackend COMMAND D3DVideoEncoder_test_d3d12_unsupported_backend)
add_test(NAME D3D11EncodeSmoke COMMAND D3DVideoEncoder_test_d3d11_encode_smoke)
add_test(NAME D3D11EncodeAsyncSmoke COMMAND D3DVideoEncoder_test_d3d11_encode_async_smoke)
add_test(NAME D3D11MediaFoundationCapabilities COMMAND D3DVideoEncoder_test_d3d11_mf_capabilities)
add_test(NAME D3D11HevcP010Smoke COMMAND D3DVideoEncoder_test_d3d11_hevc_p010_smoke)

if(D3DVIDEOENCODER_HAS_NVENC)
    add_test(NAME NvencCapabilities COMMAND D3DVideoEncoder_test_nvenc_capabilities)
    add_test(NAME NvencD3D11H264Mp4Smoke COMMAND D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke)
    add_test(NAME NvencD3D12RgbaH264Mp4Smoke COMMAND D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke)
    set_tests_properties(NvencD3D11H264Mp4Smoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_nvenc_d3d11_h264_mp4_smoke>")
    set_tests_properties(NvencD3D12RgbaH264Mp4Smoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_nvenc_d3d12_rgba_h264_mp4_smoke>")
endif()

if(D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE)
    add_test(NAME D3D12VideoEncodeCapabilities COMMAND D3DVideoEncoder_test_d3d12_video_encode_capabilities)
    add_test(NAME D3D12VideoEncodeH264Nv12Smoke COMMAND D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke)
    set_tests_properties(D3D12VideoEncodeH264Nv12Smoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_d3d12_video_encode_h264_nv12_smoke>")
endif()

set_tests_properties(D3D11EncodeSmoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_d3d11_encode_smoke>")
set_tests_properties(D3D11EncodeAsyncSmoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_d3d11_encode_async_smoke>")

set_tests_properties(D3D11HevcP010Smoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_d3d11_hevc_p010_smoke>")
