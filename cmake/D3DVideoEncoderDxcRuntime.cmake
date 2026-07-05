# Helpers for locating/copying the DirectX Shader Compiler runtime.
# dxcompiler.dll and dxil.dll are runtime dependencies of D3D11Helper/D3D12Helper
# whenever DXC-backed shader compilation is used by Processing.

set(D3DVIDEOENCODER_DXC_RUNTIME_DIR "" CACHE PATH "Directory that contains dxcompiler.dll and dxil.dll")
option(D3DVIDEOENCODER_COPY_DXC_RUNTIME "Copy dxcompiler.dll and dxil.dll next to D3DVideoEncoder executables" ON)

function(d3dvideoencoder_find_dxc_runtime)
    if(DEFINED D3DVIDEOENCODER_DXCOMPILER_DLL AND EXISTS "${D3DVIDEOENCODER_DXCOMPILER_DLL}")
        return()
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_dxc_arch "x64")
    else()
        set(_dxc_arch "x86")
    endif()

    set(_dxc_candidate_dirs)

    if(D3DVIDEOENCODER_DXC_RUNTIME_DIR)
        file(TO_CMAKE_PATH "${D3DVIDEOENCODER_DXC_RUNTIME_DIR}" _dxc_runtime_dir)
        list(APPEND _dxc_candidate_dirs "${_dxc_runtime_dir}")
    endif()

    # Accept D3D11Helper/D3D12Helper cache variables when the helper projects found DXC.
    if(DEFINED D3D11HELPER_DXCOMPILER_DLL)
        get_filename_component(_d3d11_dxc_dir "${D3D11HELPER_DXCOMPILER_DLL}" DIRECTORY)
        list(APPEND _dxc_candidate_dirs "${_d3d11_dxc_dir}")
    endif()
    if(DEFINED D3D12HELPER_DXCOMPILER_DLL)
        get_filename_component(_d3d12_dxc_dir "${D3D12HELPER_DXCOMPILER_DLL}" DIRECTORY)
        list(APPEND _dxc_candidate_dirs "${_d3d12_dxc_dir}")
    endif()

    # Common NuGet package locations.
    file(GLOB _local_package_dirs
        "${PROJECT_SOURCE_DIR}/packages/Microsoft.Direct3D.DXC*/bin/${_dxc_arch}"
        "${PROJECT_SOURCE_DIR}/packages/microsoft.direct3d.dxc*/bin/${_dxc_arch}"
        "${PROJECT_SOURCE_DIR}/packages/Microsoft.Direct3D.DXC*/build/native/bin/${_dxc_arch}"
        "${PROJECT_SOURCE_DIR}/packages/microsoft.direct3d.dxc*/build/native/bin/${_dxc_arch}"
        "${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DXC*/bin/${_dxc_arch}"
        "${CMAKE_BINARY_DIR}/packages/microsoft.direct3d.dxc*/bin/${_dxc_arch}"
        "${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DXC*/build/native/bin/${_dxc_arch}"
        "${CMAKE_BINARY_DIR}/packages/microsoft.direct3d.dxc*/build/native/bin/${_dxc_arch}"
    )
    list(APPEND _dxc_candidate_dirs ${_local_package_dirs})


    # Windows SDK commonly ships DXC in Windows Kits/10/bin/<sdk-version>/<arch>.
    if(DEFINED ENV{WindowsSdkDir})
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _windows_sdk_dir)
        if(DEFINED ENV{WindowsSDKVersion})
            string(REPLACE "\\" "/" _windows_sdk_version "$ENV{WindowsSDKVersion}")
            list(APPEND _dxc_candidate_dirs
                "${_windows_sdk_dir}/bin/${_windows_sdk_version}/${_dxc_arch}"
            )
        endif()
        file(GLOB _windows_sdk_env_dirs
            "${_windows_sdk_dir}/bin/*/${_dxc_arch}"
        )
        list(APPEND _dxc_candidate_dirs ${_windows_sdk_env_dirs})
    endif()

    # Avoid direct ENV{ProgramFiles(x86)} syntax because parentheses in the
    # environment variable name are parsed poorly by some CMake versions.
    # The explicit D3DVIDEOENCODER_DXC_RUNTIME_DIR above is preferred; these
    # literal Windows Kit locations are best-effort fallbacks.
    if(WIN32)
        file(GLOB _windows_kits_dirs_x86
            "C:/Program Files (x86)/Windows Kits/10/bin/*/${_dxc_arch}"
        )
        file(GLOB _windows_kits_dirs_pf
            "C:/Program Files/Windows Kits/10/bin/*/${_dxc_arch}"
        )
        list(APPEND _dxc_candidate_dirs
            ${_windows_kits_dirs_x86}
            ${_windows_kits_dirs_pf}
        )
    endif()

    if(DEFINED ENV{USERPROFILE})
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _user_profile)
        file(GLOB _global_package_dirs
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/bin/${_dxc_arch}"
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/build/native/bin/${_dxc_arch}"
        )
        list(APPEND _dxc_candidate_dirs ${_global_package_dirs})
    endif()

    list(REMOVE_DUPLICATES _dxc_candidate_dirs)

    find_file(D3DVIDEOENCODER_DXCOMPILER_DLL
        NAMES dxcompiler.dll
        PATHS ${_dxc_candidate_dirs}
        NO_DEFAULT_PATH
    )

    find_file(D3DVIDEOENCODER_DXIL_DLL
        NAMES dxil.dll
        PATHS ${_dxc_candidate_dirs}
        NO_DEFAULT_PATH
    )

    if(D3DVIDEOENCODER_DXCOMPILER_DLL)
        message(STATUS "D3DVideoEncoder: found dxcompiler.dll: ${D3DVIDEOENCODER_DXCOMPILER_DLL}")
        if(D3DVIDEOENCODER_DXIL_DLL)
            message(STATUS "D3DVideoEncoder: found dxil.dll: ${D3DVIDEOENCODER_DXIL_DLL}")
        endif()
    else()
        message(WARNING
            "D3DVideoEncoder: dxcompiler.dll was not found. "
            "D3D11/D3D12 Processing tests and samples may fail at runtime with 0xc0000135. "
            "Install/restore the Microsoft.Direct3D.DXC NuGet package or pass "
            "-DD3DVIDEOENCODER_DXC_RUNTIME_DIR=<dir containing dxcompiler.dll and dxil.dll>.")
    endif()
endfunction()

function(d3dvideoencoder_copy_dxc_runtime target_name)
    if(NOT D3DVIDEOENCODER_COPY_DXC_RUNTIME)
        return()
    endif()

    d3dvideoencoder_find_dxc_runtime()

    if(D3DVIDEOENCODER_DXCOMPILER_DLL)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${D3DVIDEOENCODER_DXCOMPILER_DLL}"
                    "$<TARGET_FILE_DIR:${target_name}>"
            VERBATIM
        )
    endif()

    if(D3DVIDEOENCODER_DXIL_DLL)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${D3DVIDEOENCODER_DXIL_DLL}"
                    "$<TARGET_FILE_DIR:${target_name}>"
            VERBATIM
        )
    endif()
endfunction()

function(d3dvideoencoder_set_dxc_test_environment test_name target_name)
    d3dvideoencoder_find_dxc_runtime()

    set(_path_value "$<TARGET_FILE_DIR:${target_name}>")
    if(D3DVIDEOENCODER_DXCOMPILER_DLL)
        get_filename_component(_dxc_dir "${D3DVIDEOENCODER_DXCOMPILER_DLL}" DIRECTORY)
        set(_path_value "${_path_value};${_dxc_dir}")
    endif()

    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "PATH=${_path_value};$ENV{PATH}"
    )
endfunction()
