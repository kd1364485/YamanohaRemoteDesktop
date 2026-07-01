cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(yamanoha_core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows SDK リンク
set(LIBS d3d11 dxgi d3dcompiler dxguid dbghelp)

# libdatachannel (WebRTC) の依存暗号化ライブラリ MbedTLS の自動取得と構成
include(FetchContent)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.6
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(mbedtls)
if(NOT mbedtls_POPULATED)
    FetchContent_Populate(mbedtls)
    
    # DTLS-SRTP 機能を有効化
    set(CONFIG_H "${mbedtls_SOURCE_DIR}/include/mbedtls/mbedtls_config.h")
    file(READ "${CONFIG_H}" CONFIG_CONTENT)
    string(REPLACE "//#define MBEDTLS_SSL_DTLS_SRTP" "#define MBEDTLS_SSL_DTLS_SRTP" CONFIG_CONTENT "${CONFIG_CONTENT}")
    # TLS 1.3 を無効化する。mbedTLS は DTLS 1.3 を実装しておらず、TLS 1.3 が有効だと
    # libdatachannel の DATAGRAM(DTLS) ハンドシェイクでバージョン交渉が破綻し
    # MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE で失敗するため。
    string(REPLACE "\n#define MBEDTLS_SSL_PROTO_TLS1_3" "\n//#define MBEDTLS_SSL_PROTO_TLS1_3" CONFIG_CONTENT "${CONFIG_CONTENT}")
    file(WRITE "${CONFIG_H}" "${CONFIG_CONTENT}")
    
    add_subdirectory(${mbedtls_SOURCE_DIR} ${mbedtls_BINARY_DIR})
endif()

# libsrtp/libdatachannel が内部ビルドの MbedTLS を見つけられるように変数を設定
FetchContent_GetProperties(mbedtls)
set(MbedTLS_FOUND TRUE CACHE BOOL "MbedTLS found" FORCE)
set(MBEDTLS_FOUND TRUE CACHE BOOL "MbedTLS found" FORCE)
set(MbedTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "MbedTLS include dir" FORCE)
set(MbedTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "MbedTLS include dirs" FORCE)
set(MbedTLS_LIBRARY mbedtls CACHE STRING "MbedTLS library" FORCE)
set(MbedCrypto_LIBRARY mbedcrypto CACHE STRING "MbedCrypto library" FORCE)
set(MbedX509_LIBRARY mbedx509 CACHE STRING "MbedX509 library" FORCE)
set(MbedTLS_LIBRARIES mbedtls mbedx509 mbedcrypto CACHE STRING "MbedTLS libraries" FORCE)

# リンカが mbedtls.lib 等のバイナリを確実に見つけられるようにリンクパスを設定
link_directories(
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/mbedtls-build/library"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/mbedtls-build/library/$<CONFIG>"
)

# libdatachannel (WebRTC) の自動取得と構成
set(USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    datachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(datachannel)

# コアライブラリ（DLL）
add_library(yamanoha_core SHARED
    src/core.cpp
    src/gpu/d3d11_device.cpp
    src/capture/display_manager.cpp
    src/capture/desktop_duplicator.cpp
    src/capture/capture_engine.cpp
    src/capture/cursor_cpu_compositor.cpp
    src/encoder/nvenc_encoder.cpp
    src/pipeline/adaptive_bitrate_controller.cpp
    src/pipeline/frame_pipeline.cpp
    src/webrtc/webrtc_streamer.cpp
)
target_include_directories(yamanoha_core PUBLIC src PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yamanoha_core PRIVATE ${LIBS} datachannel)
target_compile_definitions(yamanoha_core PRIVATE YAMANOHA_EXPORTS)
if(MSVC)
    target_compile_options(yamanoha_core PRIVATE /utf-8)
endif()

# テスト用コンソールアプリ (キャプチャ)
add_executable(capture_test
    test/capture_test.cpp
    src/gpu/d3d11_device.cpp
    src/capture/display_manager.cpp
    src/capture/desktop_duplicator.cpp
    src/capture/capture_engine.cpp
)
target_include_directories(capture_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(capture_test PRIVATE ${LIBS})
if(MSVC)
    target_compile_options(capture_test PRIVATE /utf-8)
endif()

# テスト用コンソールアプリ (エンコード)
add_executable(encode_test
    test/encode_test.cpp
    src/gpu/d3d11_device.cpp
    src/capture/display_manager.cpp
    src/capture/desktop_duplicator.cpp
    src/capture/capture_engine.cpp
    src/encoder/nvenc_encoder.cpp
)
target_include_directories(encode_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(encode_test PRIVATE ${LIBS} datachannel)
if(MSVC)
    target_compile_options(encode_test PRIVATE /utf-8)
endif()

# テスト用コンソールアプリ (WebRTCストリーミング統合テスト)
add_executable(webrtc_test
    test/webrtc_test.cpp
    src/gpu/d3d11_device.cpp
    src/capture/display_manager.cpp
    src/capture/desktop_duplicator.cpp
    src/capture/capture_engine.cpp
    src/encoder/nvenc_encoder.cpp
    src/webrtc/webrtc_streamer.cpp
)
target_include_directories(webrtc_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_test PRIVATE ${LIBS} datachannel)
if(MSVC)
    target_compile_options(webrtc_test PRIVATE /utf-8)
endif()

add_executable(pipeline_primitives_test
    test/pipeline_primitives_test.cpp
    src/pipeline/adaptive_bitrate_controller.cpp
)
target_include_directories(pipeline_primitives_test PRIVATE src)
if(MSVC)
    target_compile_options(pipeline_primitives_test PRIVATE /utf-8)
endif()
