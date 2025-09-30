cmake_minimum_required(VERSION 3.7)

project(skg_flatscreen VERSION 1.0
                       DESCRIPTION "An sk_gpu.h demo that illustrates usage in a flatscreen app."
                       LANGUAGES CXX)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

if (NOT DEFINED SKGPU_TEST_USE_GL)
    if (EMSCRIPTEN)
        set(SKGPU_TEST_USE_GL ON)
    else()
        set(SKGPU_TEST_USE_GL OFF)
    endif()
endif()

## Target definition

add_executable(skg_flatscreen
    main.cpp
    ../common/app.h
    ../common/app.cpp
    ../common/micro_ply.h
    ../common/HandmadeMath.h
    ../common/xr_app.h
    ../../src/sk_gpu_dev.h
    ../../src/sk_gpu_common.h
    ../../src/sk_gpu_common.cpp
    ../../src/sk_gpu_dx11.h
    ../../src/sk_gpu_dx11.cpp
    ../../src/sk_gpu_gl.h
    ../../src/sk_gpu_gl.cpp
    ../../src/sk_gpu_vk.h
    ../../src/sk_gpu_vk.cpp )

target_include_directories(skg_flatscreen PRIVATE
    ../common
    ../../src)

## Compile shaders

if (COMMAND skshaderc_compile_headers)
    skshaderc_compile_headers(skg_flatscreen
        ${CMAKE_BINARY_DIR}/shaders_test/
        "-e -t xge" SKGPU_TEST_SHADERS_H
        ../common/compute_test.hlsl
        ../common/cubemap.hlsl
        ../common/test.hlsl)
endif()

set(SKGPU_EXAMPLE_LIBS)

if (WIN32)
    list(APPEND SKGPU_EXAMPLE_LIBS
        user32
        gdi32
        shell32)
else()
    find_package(X11 REQUIRED)
    list(APPEND SKGPU_EXAMPLE_LIBS ${X11_LIBRARIES})
endif()

if (SKGPU_TEST_USE_GL)
    target_compile_definitions(skg_flatscreen PRIVATE SKGPU_TEST_USE_GL=1 SKG_FORCE_OPENGL)
    if (UNIX)
        list(APPEND SKGPU_EXAMPLE_LIBS
            dl
            GL
            GLX)
    endif()
else()
    target_compile_definitions(skg_flatscreen PRIVATE SKG_FORCE_VULKAN)
    find_package(Vulkan REQUIRED)
    list(APPEND SKGPU_EXAMPLE_LIBS Vulkan::Vulkan)
endif()

target_link_libraries(skg_flatscreen PRIVATE
    ${SKGPU_EXAMPLE_LIBS})

add_dependencies(skg_flatscreen sk_gpu_header)
