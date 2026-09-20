cmake_minimum_required(VERSION 3.20)
project(StructureBlueprint C CXX)

# ArkApi is a Windows/MSVC target: the SDK calls into the server binary by
# symbol name and ships only MSVC import libraries.
if(NOT MSVC)
    message(FATAL_ERROR "StructureBlueprint must be built with MSVC (Visual Studio).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(StructureBlueprint SHARED
    src/Main.cpp
    src/Capture.cpp
    src/Catalog.cpp
    src/Coverage.cpp
    src/Fill.cpp
    src/Paste.cpp
    src/Preview.cpp
    src/Storage.cpp
    src/Gzip.cpp
    third_party/miniz/miniz.c
)

target_include_directories(StructureBlueprint PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/Public
    ${CMAKE_SOURCE_DIR}/sdk/include/Poco
    ${CMAKE_SOURCE_DIR}/third_party/miniz
)

target_link_directories(StructureBlueprint PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/out_lib
    ${CMAKE_SOURCE_DIR}/sdk/lib
)

target_link_libraries(StructureBlueprint PRIVATE ArkApi)

# The SDK defines TCHAR as WIDECHAR and calls the _tcs* macros, so it only
# compiles as a Unicode target; a narrow build collides on TCHAR itself.
target_compile_definitions(StructureBlueprint PRIVATE
    ARK_GAME
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# ArkApi loads plugins as <PluginName>/<PluginName>.dll, so keep the name exact.
set_target_properties(StructureBlueprint PROPERTIES
    OUTPUT_NAME "StructureBlueprint"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/StructureBlueprint"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist/StructureBlueprint"
)

# The SDK's import libs are /MD, so the plugin must match.
set_property(TARGET StructureBlueprint PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
