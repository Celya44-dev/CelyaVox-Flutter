
cmake_minimum_required(VERSION 3.22.1)
project(voip_engine)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_MESSAGE_LOG_LEVEL VERBOSE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define endianness for PJSIP
add_definitions(-DPJ_IS_LITTLE_ENDIAN=1 -DPJ_IS_BIG_ENDIAN=0)

# Headers expected under android/pjsip/pjproject-<version>/
include_directories(
    ${CMAKE_SOURCE_DIR}/../../../../pjsip/pjproject-2.17/pjsip/include
    ${CMAKE_SOURCE_DIR}/../../../../pjsip/pjproject-2.17/pjlib/include
    ${CMAKE_SOURCE_DIR}/../../../../pjsip/pjproject-2.17/pjlib-util/include
    ${CMAKE_SOURCE_DIR}/../../../../pjsip/pjproject-2.17/pjmedia/include
    ${CMAKE_SOURCE_DIR}/../../../../pjsip/pjproject-2.17/pjnath/include
)

add_library(voip_engine SHARED voip_engine.cpp)

# PJSIP prebuilt libraries expected in app/src/main/jniLibs/<abi>/
set(PJSIP_LIB_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})

if(NOT EXISTS ${PJSIP_LIB_DIR})
    message(FATAL_ERROR "PJSIP libs directory not found: ${PJSIP_LIB_DIR}")
endif()

set(PJSIP_LIB_SUFFIXES ".so" ".a")

function(find_pjsip_lib out_var name)
    set(found "")
    foreach(suffix IN LISTS PJSIP_LIB_SUFFIXES)
        if(EXISTS "${PJSIP_LIB_DIR}/lib${name}${suffix}")
            set(found "${PJSIP_LIB_DIR}/lib${name}${suffix}")
            break()
        endif()
    endforeach()

    if(NOT found)
        message(FATAL_ERROR "PJSIP library not found: lib${name}.so|.a in ${PJSIP_LIB_DIR}")
    endif()

    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

# Import PJSIP libraries (shared or static)
set(PJSIP_CORE_PATH "")
if(EXISTS "${PJSIP_LIB_DIR}/libpjsip-core.a")
    set(PJSIP_CORE_PATH "${PJSIP_LIB_DIR}/libpjsip-core.a")
elseif(EXISTS "${PJSIP_LIB_DIR}/libpjsip-core.so")
    set(PJSIP_CORE_PATH "${PJSIP_LIB_DIR}/libpjsip-core.so")
endif()
find_pjsip_lib(PJ_LIB_PATH pj)
find_pjsip_lib(PJLIB_UTIL_PATH pjlib-util)
find_pjsip_lib(PJNATH_PATH pjnath)
find_pjsip_lib(PJMEDIA_AUDIODEV_PATH pjmedia-audiodev)
find_pjsip_lib(PJMEDIA_CODEC_PATH pjmedia-codec)
find_pjsip_lib(PJMEDIA_PATH pjmedia)
find_pjsip_lib(PJSIP_PATH pjsip)
find_pjsip_lib(PJSIP_SIMPLE_PATH pjsip-simple)
find_pjsip_lib(PJSIP_UA_PATH pjsip-ua)
find_pjsip_lib(PJSUA_PATH pjsua)
find_pjsip_lib(PJSUA2_PATH pjsua2)

# Set linker flags to include the library path
set_target_properties(voip_engine PROPERTIES LINK_FLAGS "-L${PJSIP_LIB_DIR}")

find_library(log-lib log)
find_library(opensles-lib OpenSLES)

target_link_libraries(
    voip_engine
    -Wl,--start-group
    ${PJSIP_CORE_PATH}
    ${PJSUA2_PATH}
    ${PJSUA_PATH}
    ${PJSIP_UA_PATH}
    ${PJSIP_SIMPLE_PATH}
    ${PJSIP_PATH}
    ${PJMEDIA_AUDIODEV_PATH}
    ${PJMEDIA_CODEC_PATH}
    ${PJMEDIA_PATH}
    ${PJNATH_PATH}
    ${PJLIB_UTIL_PATH}
    ${PJ_LIB_PATH}
    -Wl,--end-group
    ${log-lib}
    ${opensles-lib}
)
