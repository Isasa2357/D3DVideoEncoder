add_executable(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
    main.cpp
)

target_link_libraries(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
    PRIVATE
        D3DVideoEncoder::D3DVideoEncoder
)

target_include_directories(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    target_compile_definitions(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
        PRIVATE D3DVE_DIAGNOSTIC_WINDOWS_SDK_VERSION=\"${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\"
    )
endif()

if(MSVC)
    target_compile_options(D3DVideoEncoder_04_d3d12_video_encode_capability_dump PRIVATE /utf-8 /EHsc)
endif()
