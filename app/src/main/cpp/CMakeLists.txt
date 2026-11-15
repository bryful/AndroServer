cmake_minimum_required(VERSION 3.22.1)

project("androserver")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 最適化オプション
add_compile_options(-O3 -Wall)

# -------------------------------------------------------------
# oatpp の取得と設定
# -------------------------------------------------------------
include(FetchContent)

set(OATPP_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

FetchContent_Declare(
    oatpp
    GIT_REPOSITORY https://github.com/oatpp/oatpp.git
    GIT_TAG        1.3.0-latest
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(oatpp)

# -------------------------------------------------------------
# ネイティブ共有ライブラリのビルド
# -------------------------------------------------------------
add_library(
    androserver
    SHARED
    native-lib.cpp
    ServerApp.cpp
)

target_compile_definitions(androserver PRIVATE _FILE_OFFSET_BITS=64)

target_include_directories(
    androserver
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${oatpp_SOURCE_DIR}/src
)

# 16KB ページサイズ対応 (Pixel 8 / 9 / Android 15+ 必須)
target_link_options(
    androserver
    PRIVATE
    "-Wl,-z,max-page-size=16384"
)

# Androidログ、アトミック、およびoatppのリンク
if(TARGET oatpp::oatpp)
    target_link_libraries(
        androserver
        PRIVATE
        oatpp::oatpp
        log
        atomic
    )
else()
    target_link_libraries(
        androserver
        PRIVATE
        oatpp
        log
        atomic
    )
endif()

