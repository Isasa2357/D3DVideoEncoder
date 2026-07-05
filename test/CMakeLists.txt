set(D3DVIDEOENCODER_TEST_TARGETS
    D3DVideoEncoder_test_timestamp_generator
    D3DVideoEncoder_test_types
    D3DVideoEncoder_test_desc_validation
    D3DVideoEncoder_test_hresult
    D3DVideoEncoder_test_encode_job_queue
    D3DVideoEncoder_test_d3d12_unsupported_backend
    D3DVideoEncoder_test_d3d11_encode_smoke
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
add_test(NAME D3D12UnsupportedBackend COMMAND D3DVideoEncoder_test_d3d12_unsupported_backend)
add_test(NAME D3D11EncodeSmoke COMMAND D3DVideoEncoder_test_d3d11_encode_smoke)
set_tests_properties(D3D11EncodeSmoke PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3DVideoEncoder_test_d3d11_encode_smoke>")
