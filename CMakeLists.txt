cmake_minimum_required(VERSION 3.16)
project(tplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

# Homebrew hint (important on macOS)
if(APPLE)
    enable_language(OBJCXX)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND EXISTS "/usr/local")
        list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    elseif(EXISTS "/opt/homebrew")
        list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    elseif(EXISTS "/usr/local")
        list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    endif()
endif()

find_package(ftxui REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(BLAS REQUIRED)
find_package(TagLib QUIET)
find_package(SQLite3 REQUIRED)

option(TPLAY_ENABLE_ONNXRUNTIME "Enable ONNX Runtime Demucs backend" ON)

if(APPLE AND TPLAY_ENABLE_ONNXRUNTIME)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        PATHS ${CMAKE_PREFIX_PATH}/include/onnxruntime /opt/homebrew/include/onnxruntime /usr/local/include/onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        PATHS ${CMAKE_PREFIX_PATH}/lib /opt/homebrew/lib /usr/local/lib)
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
        set(TPLAY_WITH_ONNXRUNTIME ON)
    else()
        message(STATUS "ONNX Runtime not found; Demucs CoreML backend disabled")
        set(TPLAY_WITH_ONNXRUNTIME OFF)
    endif()
else()
    set(TPLAY_WITH_ONNXRUNTIME OFF)
endif()

set(ESSENTIA_PKG_CONFIG_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/external/essentia-install/lib/pkgconfig"
    "/opt/homebrew/opt/ffmpeg/lib/pkgconfig"
    "/opt/homebrew/opt/libsamplerate/lib/pkgconfig"
    "/opt/homebrew/opt/taglib/lib/pkgconfig"
    "/opt/homebrew/opt/chromaprint/lib/pkgconfig"
    "/opt/homebrew/opt/fftw/lib/pkgconfig"
    "/opt/homebrew/opt/libyaml/lib/pkgconfig"
    "/opt/homebrew/opt/eigen/share/pkgconfig"
    "/opt/homebrew/lib/pkgconfig"
)
string(JOIN ":" ESSENTIA_PKG_CONFIG_PATH_JOINED ${ESSENTIA_PKG_CONFIG_PATH})
if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH} "${ESSENTIA_PKG_CONFIG_PATH_JOINED}:$ENV{PKG_CONFIG_PATH}")
else()
    set(ENV{PKG_CONFIG_PATH} "${ESSENTIA_PKG_CONFIG_PATH_JOINED}")
endif()
pkg_check_modules(ESSENTIA REQUIRED essentia)

set(DEMUCS_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/demucs.cpp")
if(EXISTS "${DEMUCS_CPP_DIR}/src/model.hpp")
    file(GLOB DEMUCS_CPP_SOURCES "${DEMUCS_CPP_DIR}/src/*.cpp")
    add_library(tplay_demucs STATIC ${DEMUCS_CPP_SOURCES})
    target_compile_features(tplay_demucs PUBLIC cxx_std_17)
    target_compile_definitions(tplay_demucs PUBLIC EIGEN_USE_BLAS PRIVATE NDEBUG)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(tplay_demucs
            PRIVATE
                -O3
                -march=native
                -Wno-deprecated-anon-enum-enum-conversion
                -Wno-nan-infinity-disabled
        )
    endif()
    target_include_directories(tplay_demucs
        PUBLIC
            "${DEMUCS_CPP_DIR}/src"
            "${DEMUCS_CPP_DIR}/vendor/eigen"
    )
    target_link_libraries(tplay_demucs PUBLIC ${BLAS_LIBRARIES})

    set(LIBNYQUIST_BUILD_EXAMPLE OFF CACHE BOOL "Disable libnyquist example" FORCE)
    add_subdirectory("${DEMUCS_CPP_DIR}/vendor/libnyquist" "${CMAKE_BINARY_DIR}/libnyquist-build")
    if(TARGET libnyquist AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libnyquist PRIVATE -w)
    endif()
else()
    message(FATAL_ERROR "external/demucs.cpp is missing. Clone sevagh/demucs.cpp into external/demucs.cpp.")
endif()

add_executable(tplay
    src/main.cpp
    src/core/TrackStore.cpp
    src/core/AppController.cpp
    src/core/Config.cpp
    src/core/AudioEngine.mm
    src/core/MacActivity.mm
    src/core/DownloadManager.cpp
    src/core/StemSeparator.cpp
    src/core/AudioProcessor.cpp
    src/core/ProcessRunner.cpp
    src/core/AudioAnalyzer.cpp
    src/core/MetadataWriter.cpp
    src/Library/LibraryDatabase.cpp
    src/Library/TrackId.cpp
    src/Library/TrackRepository.cpp
    src/Library/CueRepository.cpp
    src/Library/LoopRepository.cpp
    src/Library/PlaylistRepository.cpp
    src/Library/LibraryJson.cpp
    src/ui/ManualCueEditor.cpp
    src/ui/TrimEditor.cpp
    src/AutoCue/AutoCueMarker.cpp
    src/AutoCue/CueDetector.cpp
    src/AutoCue/CuePreview.cpp
    src/AutoCue/FolderProcessor.cpp
    src/Serato/SeratoCueWriter.cpp
    src/Serato/SeratoMarkers2.cpp
    src/Export/SeratoExportProvider.cpp
    src/Export/RekordboxExportProvider.cpp
    src/Export/TraktorExportProvider.cpp
    src/Export/LibraryExporter.cpp
    src/Export/ExportValidator.cpp
    src/Sync/JsonSync.cpp
    src/Telegram/TelegramBotClient.cpp
    src/Telegram/TelegramInboxService.cpp
    src/Telegram/TelegramRepository.cpp
    src/Traktor/TraktorMetadataWriter.cpp
)

if(NOT TagLib_FOUND)
    find_path(TAGLIB_INCLUDE_DIR taglib/tag.h PATHS /opt/homebrew/include /usr/local/include)
    find_library(TAGLIB_LIBRARY tag PATHS /opt/homebrew/lib /usr/local/lib)
    if(NOT TAGLIB_INCLUDE_DIR OR NOT TAGLIB_LIBRARY)
        message(FATAL_ERROR "TagLib not found")
    endif()
endif()

target_include_directories(tplay
    PRIVATE /opt/homebrew/include
)

if(NOT TagLib_FOUND)
    target_include_directories(tplay PRIVATE ${TAGLIB_INCLUDE_DIR})
endif()

target_include_directories(tplay
    SYSTEM PRIVATE ${ESSENTIA_INCLUDE_DIRS}
)

target_link_directories(tplay
    PRIVATE ${ESSENTIA_LIBRARY_DIRS}
)

target_link_libraries(tplay
    PRIVATE ftxui::screen
    PRIVATE ftxui::dom
    PRIVATE ftxui::component
    PRIVATE Threads::Threads
    PRIVATE SQLite3::SQLite3
    PRIVATE tplay_demucs
    PRIVATE libnyquist
    PRIVATE ${ESSENTIA_LIBRARIES}
)

if(TagLib_FOUND)
    target_link_libraries(tplay PRIVATE TagLib::TagLib)
else()
    target_link_libraries(tplay PRIVATE ${TAGLIB_LIBRARY})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(tplay PRIVATE m dl)
endif()

if(APPLE)
    set_source_files_properties(
        src/core/AudioEngine.mm
        src/core/MacActivity.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(tplay PRIVATE "-framework AVFoundation" "-framework Foundation" "-framework IOKit" "-framework CoreFoundation" "-framework CoreAudio" "-framework Accelerate")
    if(TPLAY_WITH_ONNXRUNTIME)
        target_compile_definitions(tplay PRIVATE TPLAY_WITH_ONNXRUNTIME=1)
        target_include_directories(tplay PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(tplay PRIVATE ${ONNXRUNTIME_LIBRARY})
    endif()
endif()

# Copy config.toml to build directory
configure_file(config.toml config.toml COPYONLY)

if(APPLE)
    add_custom_target(package_macos
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_macos.sh" "${CMAKE_BINARY_DIR}"
        DEPENDS tplay
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Create portable macOS zip package"
    )
endif()

add_executable(tplay_library_export_smoke
    tests/library_export_smoke.cpp
    src/Export/LibraryExporter.cpp
    src/Export/RekordboxExportProvider.cpp
    src/Export/TraktorExportProvider.cpp
    src/Export/SeratoExportProvider.cpp
    src/Export/ExportValidator.cpp
    src/Sync/JsonSync.cpp
    src/Library/LibraryDatabase.cpp
    src/Library/TrackRepository.cpp
    src/Library/CueRepository.cpp
    src/Library/TrackId.cpp
    src/Library/LibraryJson.cpp
    src/Telegram/TelegramRepository.cpp
    src/Traktor/TraktorMetadataWriter.cpp
    src/Serato/SeratoCueWriter.cpp
    src/Serato/SeratoMarkers2.cpp
)
target_link_libraries(tplay_library_export_smoke
    PRIVATE SQLite3::SQLite3
)
if(TagLib_FOUND)
    target_link_libraries(tplay_library_export_smoke PRIVATE TagLib::TagLib)
else()
    target_include_directories(tplay_library_export_smoke PRIVATE ${TAGLIB_INCLUDE_DIR})
    target_link_libraries(tplay_library_export_smoke PRIVATE ${TAGLIB_LIBRARY})
endif()
add_test(NAME library_export_smoke COMMAND tplay_library_export_smoke)
