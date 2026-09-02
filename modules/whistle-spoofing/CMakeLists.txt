cmake_minimum_required(VERSION 3.21)
project(whistle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dumper-7 output: copy C:/Dumper-7/<Game>-<Ver>/CppSDK  ->  tools/whistle/CppSDK
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/CppSDK")

if(NOT EXISTS "${SDK_DIR}/SDK/Basic.hpp")
    message(FATAL_ERROR
        "CppSDK not found at ${SDK_DIR}. "
        "Inject Dumper-7 into the game first, then copy "
        "C:/Dumper-7/<Game>-<Ver>/CppSDK here.")
endif()

# Minimal set per UsingTheSDK.md.
# Add more *_functions.cpp here if you start calling more SDK functions.
set(SDK_SOURCES
    "${SDK_DIR}/SDK/Basic.cpp"
    "${SDK_DIR}/SDK/CoreUObject_functions.cpp"
    # Basic.cpp's GetStaticName() calls UKismetStringLibrary::Conv_StringToName.
    # Omitting this file causes LNK2019.
    "${SDK_DIR}/SDK/Engine_functions.cpp"
)

add_library(whistle SHARED
    src/main.cpp
    ${SDK_SOURCES}
)

target_include_directories(whistle PRIVATE "${SDK_DIR}")

if(MSVC)
    # /utf-8  : source has Korean comments/strings; without this MSVC reads them
    #           as the system codepage (cp949) and fails with C2001.
    # /EHa    : required to mix __try/__except with C++ exceptions.
    # /bigobj : SDK headers are large enough to hit the object section limit.
    target_compile_options(whistle PRIVATE /utf-8 /EHa /bigobj /permissive- /W3)
    target_compile_definitions(whistle PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Version-suffixed output name.
#
# A DLL already injected into the game stays mapped, so the linker cannot
# overwrite it (LNK1104). END unhooks but does not always unload the module.
# Bump this number on each code change to get a fresh file, so repeated
# experiments do not require restarting the game.
set(WHISTLE_VERSION "15")

set_target_properties(whistle PROPERTIES
    OUTPUT_NAME "whistle_v${WHISTLE_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)
