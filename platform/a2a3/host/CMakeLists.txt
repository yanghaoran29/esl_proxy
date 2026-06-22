cmake_minimum_required(VERSION 3.16.3)
project(esl_onboard_runner LANGUAGES CXX)

if(NOT DEFINED ASCEND_HOME_PATH)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
    else()
        message(FATAL_ERROR "ASCEND_HOME_PATH must be set")
    endif()
endif()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(esl_onboard_runner
    onboard_runner.cpp
    aicpu_loader.cpp
    elf_fingerprint.cpp
)

target_compile_features(esl_onboard_runner PRIVATE cxx_std_17)
target_compile_options(esl_onboard_runner PRIVATE -Wall -Wextra -O2 -g)

target_include_directories(esl_onboard_runner
    PRIVATE
        "${ROOT}/platform/include"
        "${ASCEND_HOME_PATH}/include"
        "${ASCEND_HOME_PATH}/pkg_inc"
        "${ASCEND_HOME_PATH}/pkg_inc/runtime"
        "${ASCEND_HOME_PATH}/pkg_inc/runtime/runtime"
        "${ASCEND_HOME_PATH}/pkg_inc/profiling"
        "${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/asc/include"
        "${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include/driver"
)

target_link_directories(esl_onboard_runner
    PRIVATE
        "${ASCEND_HOME_PATH}/lib64"
        "${ASCEND_HOME_PATH}/runtime/lib64"
)

target_link_libraries(esl_onboard_runner
    PRIVATE
        ascendcl
        runtime
        dl
        pthread
)

set_target_properties(esl_onboard_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ROOT}/build/onboard/host"
)
