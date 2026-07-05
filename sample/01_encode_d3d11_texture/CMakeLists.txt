add_executable(D3DVideoEncoder_01_encode_d3d11_texture
    main.cpp
)

target_link_libraries(D3DVideoEncoder_01_encode_d3d11_texture
    PRIVATE
        D3DVideoEncoder::D3DVideoEncoder
)

if(MSVC)
    target_compile_options(D3DVideoEncoder_01_encode_d3d11_texture PRIVATE /utf-8 /EHsc)
endif()

d3dvideoencoder_copy_dxc_runtime(D3DVideoEncoder_01_encode_d3d11_texture)
