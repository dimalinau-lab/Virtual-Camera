cmake_minimum_required(VERSION 3.20)
project(VirtualCamNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN CPPHTTPLIB_NO_OPENSSL)

if (MSVC)
    add_compile_options(/utf-8)
endif()

# Папка вывода по умолчанию
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

set(FFMPEG_ROOT "D:/ffmpeg-master-latest-win64-gpl-shared" CACHE PATH "FFmpeg directory")

include(FetchContent)

# 1. Microsoft Edge WebView2 SDK
FetchContent_Declare(
    webview2_sdk
    URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2592.51"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(webview2_sdk)

set(WEBVIEW2_INCLUDE_DIR "${webview2_sdk_SOURCE_DIR}/build/native/include")
set(WEBVIEW2_LIB_PATH "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2Loader.dll.lib")
set(WEBVIEW2_LOADER_DLL "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2Loader.dll")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_ROOT}/include
    ${WEBVIEW2_INCLUDE_DIR}
)

link_directories(
    ${FFMPEG_ROOT}/lib
)

# 1. COM DLL драйвер виртуальной камеры
add_library(NativeMFVirtualCam SHARED
    src/dllmain.cpp
    src/virtual_camera.cpp
    src/virtual_camera.hpp
    src/dshow_cam.hpp
    src/mf_shared_mem.hpp
)

target_link_libraries(NativeMFVirtualCam PRIVATE
    mfplat
    mfuuid
    mf
    mfcore
    ole32
    advapi32
    shlwapi
    strmiids
)

# 2. Исполняемый файл приложения (GUI + стример + локальный HTTP)
add_executable(VirtualCamNative
    src/main.cpp
    src/gui_window.hpp
    src/gui_window.cpp
    src/http_server.hpp
    src/http_server.cpp
    src/tcp_receiver.cpp
    src/tcp_receiver.hpp
    src/nvdec_decoder.cpp
    src/nvdec_decoder.hpp
    src/obs_vcam.cpp
    src/obs_vcam.hpp
    src/mf_vcam_writer.hpp
    src/mf_shared_mem.hpp
)

target_link_libraries(VirtualCamNative PRIVATE
    ${WEBVIEW2_LIB_PATH}
    avcodec
    avutil
    swscale
    ws2_32
    winmm
    advapi32
    shell32
    ole32
    oleaut32
    dwmapi
)

# Автоматическое копирование файлов в рабочую папку
add_custom_command(TARGET VirtualCamNative POST_BUILD
    # 1. Копируем WebView2Loader.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WEBVIEW2_LOADER_DLL}"
    "$<TARGET_FILE_DIR:VirtualCamNative>/WebView2Loader.dll"
    
    # 2. Копируем index.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/src/index.html"
    "$<TARGET_FILE_DIR:VirtualCamNative>/index.html"
    
    # 3. Безопасное копирование redist
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/redist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/redist"
    "$<TARGET_FILE_DIR:VirtualCamNative>"
    
    COMMENT "Копирование ресурсов и зависимостей в папку вывода..."
)