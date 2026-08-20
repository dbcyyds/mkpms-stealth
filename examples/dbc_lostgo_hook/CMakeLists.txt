cmake_minimum_required(VERSION 3.28.0)
# dbc_lostgo_hook + kpm 一体：
#   outputs/arm64-v8a/libdbc.so  — 业务/钩子模块
#   outputs/arm64-v8a/dbc        — 单文件启动器（内嵌 so，内核 kload 注入，无 tinjector）

option(ENABLE_CCACHE "启用 ccache" ON)

set(NDK_PATH "D:/SDK/ndk/28.2.13676358" CACHE PATH "Android NDK 路径")
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 30)
set(ANDROID_PLATFORM 30)
set(ANDROID_ABI arm64-v8a)
set(ANDROID_NDK ${NDK_PATH})
set(CMAKE_TOOLCHAIN_FILE "${NDK_PATH}/build/cmake/android.toolchain.cmake")
set(CMAKE_ANDROID_STL_TYPE c++_static)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

if(ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    endif()
endif()

project(dbc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-w -O3)

set(SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(OUT "${CMAKE_SOURCE_DIR}/outputs/${ANDROID_ABI}")
file(MAKE_DIRECTORY "${OUT}")

file(GLOB SOLOAD_SOURCES "${SRC_ROOT}/DbcHideSoinfo/src/*.c")
file(GLOB SOLOAD_MODULES "${SRC_ROOT}/DbcHideSoinfo/module/*.cxx")

# 业务模块：内核 WxShadow hook + Engine 帧入口
set(DBC_MODULES
    DBC/modules/core/Memory.cppm
    DBC/modules/core/GlobalConfig.cppm
    DBC/modules/core/GameOffsets.cppm
    DBC/modules/core/WxShadow.cppm
    DBC/modules/core/Hook.cppm
    DBC/modules/engine/Engine.cppm
)

# ---------- soload（隐藏 soinfo 辅助）----------
add_library(soload STATIC ${SOLOAD_SOURCES})
target_include_directories(soload PRIVATE DbcHideSoinfo/include)
target_link_libraries(soload PUBLIC android log)
set_target_properties(soload PROPERTIES LINKER_LANGUAGE C)

# ---------- 1) libdbc.so ----------
add_library(dbc_so SHARED
    DBC/Main.cpp
    ${DBC_MODULES}
    ${SOLOAD_MODULES}
)
set_target_properties(dbc_so PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_sources(dbc_so PRIVATE
    FILE_SET ms TYPE CXX_MODULES FILES
    ${DBC_MODULES}
    ${SOLOAD_MODULES}
)
target_include_directories(dbc_so PRIVATE
    DbcHideSoinfo/include
    DBC
    DBC/include
    DBC/includes
    DBC/libs
)
target_link_libraries(dbc_so PRIVATE soload android log dl)
set_target_properties(dbc_so PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${OUT}"
    RUNTIME_OUTPUT_DIRECTORY "${OUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${OUT}"
    OUTPUT_NAME "dbc"
    PREFIX "lib"
    SUFFIX ".so"
)

set(LIBDBC_SO "${OUT}/libdbc.so")
set(SO_EMBED_C "${CMAKE_SOURCE_DIR}/launcher/so_embed.c")
set(SO_EMBED_H "${CMAKE_SOURCE_DIR}/launcher/so_embed.h")

# ---------- 2) so → 内嵌数组 ----------
find_program(Python3_EXECUTABLE NAMES python3 python py REQUIRED)
add_custom_command(
    OUTPUT "${SO_EMBED_C}"
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/embed_so.py
        ${LIBDBC_SO}
        ${SO_EMBED_C}
        ${SO_EMBED_H}
    DEPENDS dbc_so ${CMAKE_SOURCE_DIR}/scripts/embed_so.py
    COMMENT "Embedding libdbc.so into launcher/so_embed.c"
)

# ---------- 3) 可执行文件 dbc（静态，内嵌 so，内核 kload 注入）----------
add_executable(dbc_bin
    launcher/dbc_main.c
    "${SO_EMBED_C}"
)
target_include_directories(dbc_bin PRIVATE launcher)
target_compile_options(dbc_bin PRIVATE -O2 -w)
target_link_options(dbc_bin PRIVATE -static)
target_link_libraries(dbc_bin PRIVATE dl)
set_target_properties(dbc_bin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT}"
    OUTPUT_NAME "dbc"
    SUFFIX ""
)

add_custom_target(all_outputs ALL DEPENDS dbc_so dbc_bin)

message(STATUS "NDK: ${NDK_PATH}")
message(STATUS "Outputs: ${OUT}/libdbc.so + ${OUT}/dbc (kpm kernel inject)")
