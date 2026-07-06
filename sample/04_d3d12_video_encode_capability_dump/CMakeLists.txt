add_executable(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
    main.cpp
)

target_link_libraries(D3DVideoEncoder_04_d3d12_video_encode_capability_dump
    PRIVATE
        D3DVideoEncoder::D3DVideoEncoder
)

if(MSVC)
    target_compile_options(D3DVideoEncoder_04_d3d12_video_encode_capability_dump PRIVATE /utf-8 /EHsc)
endif()
