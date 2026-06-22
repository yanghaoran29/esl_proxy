# esl_proxy onboard AICPU kernel — fork of simpler onboard/aicpu CMakeLists.
# Does NOT compile simpler's kernel.cpp; esl_proxy supplies its own entry in CUSTOM_SOURCE_DIRS.
cmake_minimum_required(VERSION 3.16.3)

project(aicpu_kernel LANGUAGES C CXX)

if(NOT DEFINED SIMPLER_ROOT)
    if(DEFINED ENV{SIMPLER_ROOT})
        set(SIMPLER_ROOT "$ENV{SIMPLER_ROOT}")
    else()
        message(FATAL_ERROR "MUST set SIMPLER_ROOT")
    endif()
endif()
if(NOT DEFINED ASCEND_HOME_PATH)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
    else()
        message(FATAL_ERROR "MUST set ASCEND_HOME_PATH")
    endif()
endif()

set(SIMPLER_A2A3_PLATFORM "${SIMPLER_ROOT}/src/a2a3/platform")
set(SIMPLER_COMMON "${SIMPLER_ROOT}/src/common")

set(CMAKE_CUSTOM_INCLUDE_DIRS "")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_A2A3_PLATFORM}/include")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_COMMON}/platform/include")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_COMMON}/platform/onboard/aicpu")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_COMMON}/task_interface")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_COMMON}/log/include")
list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${SIMPLER_COMMON}")

if(DEFINED CUSTOM_INCLUDE_DIRS)
    foreach(INC_DIR ${CUSTOM_INCLUDE_DIRS})
        list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${INC_DIR}")
    endforeach()
else()
    message(FATAL_ERROR "MUST set CUSTOM_INCLUDE_DIRS to build AICPU kernel")
endif()

set(AICPU_KERNEL_SOURCES "")
file(GLOB COMMON_SOURCES CONFIGURE_DEPENDS
    "${SIMPLER_A2A3_PLATFORM}/shared/aicpu/*.cpp"
    "${SIMPLER_COMMON}/platform/shared/aicpu/*.cpp"
    "${SIMPLER_COMMON}/platform/onboard/aicpu/*.cpp"
)
list(APPEND AICPU_KERNEL_SOURCES ${COMMON_SOURCES})

if(DEFINED CUSTOM_SOURCE_DIRS)
    foreach(SRC_DIR ${CUSTOM_SOURCE_DIRS})
        file(GLOB_RECURSE DIR_SOURCES "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.c")
        list(APPEND AICPU_KERNEL_SOURCES ${DIR_SOURCES})
    endforeach()
else()
    message(FATAL_ERROR "MUST set CUSTOM_SOURCE_DIRS to build AICPU kernel")
endif()

list(LENGTH AICPU_KERNEL_SOURCES NUM_AICPU_KERNEL_SOURCES)
message(STATUS "esl_proxy AICpu kernel: ${NUM_AICPU_KERNEL_SOURCES} source files")

add_library(aicpu_kernel SHARED ${AICPU_KERNEL_SOURCES})

option(WERROR "Treat compiler warnings as errors" ON)
if(DEFINED ENV{SIMPLER_DISABLE_WARNINGS_AS_ERRORS})
    set(WERROR OFF)
endif()

target_compile_options(aicpu_kernel
    PRIVATE
        -Wall
        -Wextra
        -rdynamic
        -O3
        -fPIC
        $<$<BOOL:${WERROR}>:-Werror>
        -Wno-error=variadic-macros
        -fno-common
        -g
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)

target_include_directories(aicpu_kernel
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CUSTOM_INCLUDE_DIRS}
        ${ASCEND_HOME_PATH}/include
        ${ASCEND_HOME_PATH}/include/toolchain
        ${ASCEND_HOME_PATH}/pkg_inc/base
)

target_link_directories(aicpu_kernel
    PRIVATE
        ${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/devlib/linux/aarch64/
        ${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/devlib/device/
        ${ASCEND_HOME_PATH}/lib64
)

set_target_properties(aicpu_kernel PROPERTIES OUTPUT_NAME aicpu_kernel)

set(AICPU_DISPATCHER_SOURCES
    "${SIMPLER_COMMON}/aicpu_loader/device/aicpu_dispatcher.cpp"
)
add_library(aicpu_dispatcher SHARED ${AICPU_DISPATCHER_SOURCES})

target_compile_options(aicpu_dispatcher
    PRIVATE
        -Wall
        -Wextra
        -rdynamic
        -O3
        -fPIC
        -g
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
)

target_include_directories(aicpu_dispatcher
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CUSTOM_INCLUDE_DIRS}
        ${SIMPLER_COMMON}
        ${ASCEND_HOME_PATH}/include
)

target_link_libraries(aicpu_dispatcher PRIVATE dl)

set_target_properties(aicpu_dispatcher PROPERTIES
    LINK_FLAGS "-Wl,--build-id"
    OUTPUT_NAME "simpler_aicpu_dispatcher"
)
