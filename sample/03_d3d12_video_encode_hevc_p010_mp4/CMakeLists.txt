add_executable(D3DVideoEncoder_03_d3d12_video_encode_hevc_p010_mp4
    main.cpp
)

target_link_libraries(D3DVideoEncoder_03_d3d12_video_encode_hevc_p010_mp4
    PRIVATE
        D3DVideoEncoder::D3DVideoEncoder
)

if(MSVC)
    target_compile_options(D3DVideoEncoder_03_d3d12_video_encode_hevc_p010_mp4 PRIVATE /utf-8 /EHsc)
endif()
