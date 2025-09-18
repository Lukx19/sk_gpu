cmake_minimum_required(VERSION 3.7)

project(skg_api_tests LANGUAGES CXX)

add_executable(skg_api_test
    sk_gpu_api_test.cpp)

target_compile_features(skg_api_test PRIVATE cxx_std_17)
target_compile_definitions(skg_api_test PRIVATE SKG_FORCE_OPENGL SKG_LINUX_EGL)

target_include_directories(skg_api_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)

set(TEST_LIBS
    EGL
    GLESv2
    dl)

target_link_libraries(skg_api_test PRIVATE ${TEST_LIBS})

add_dependencies(skg_api_test sk_gpu_header)

find_program(XVFB_RUN xvfb-run)
if (XVFB_RUN)
    add_test(NAME skg_api_gl COMMAND ${XVFB_RUN} -a $<TARGET_FILE:skg_api_test>)
else()
    add_test(NAME skg_api_gl COMMAND $<TARGET_FILE:skg_api_test>)
endif()
