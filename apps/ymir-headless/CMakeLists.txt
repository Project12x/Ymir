find_package(tomlplusplus CONFIG REQUIRED)
find_package(Stb REQUIRED)

## Create the emulator-backed debug service shared by the worker and its tests
add_library(ymir-headless-service STATIC
    src/debug_service.cpp
    src/toml_implementation.cpp
    src/video_capture.cpp
)
add_library(ymir::ymir-headless-service ALIAS ymir-headless-service)

target_include_directories(ymir-headless-service
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE "$<BUILD_INTERFACE:${Stb_INCLUDE_DIR}>"
)
target_compile_features(ymir-headless-service PUBLIC cxx_std_20)
target_link_libraries(ymir-headless-service PUBLIC
    ymir::ymir-core
    ymir::ymir-dbg-commons
    tomlplusplus::tomlplusplus
)

## Create the executable target
add_executable(ymir-headless
    src/main.cpp
)
add_executable(ymir::ymir-headless ALIAS ymir-headless)

set_target_properties(ymir-headless PROPERTIES
                      VERSION ${Ymir_VERSION}
                      SOVERSION ${Ymir_VERSION_MAJOR})

target_include_directories(ymir-headless
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

target_compile_features(ymir-headless PUBLIC cxx_std_20)

## Add dependencies
target_link_libraries(ymir-headless PRIVATE
    ymir::ymir-headless-service
    fmt::fmt
)

cmrk_copy_runtime_dlls(ymir-headless)

if (IPO_SUPPORTED AND Ymir_ENABLE_IPO)
    message(STATUS "Enabling IPO / LTO for ymir-headless")
    set_property(TARGET ymir-headless PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

## Apply performance options
if (Ymir_AVX2)
    if (MSVC)
        target_compile_options(ymir-headless PUBLIC "/arch:AVX2")
    else ()
        target_compile_options(ymir-headless PUBLIC "-mavx2" "-mfma" "-mpopcnt" "-mlzcnt" "-mbmi" "-mbmi2")
    endif ()
endif ()

## Configure Visual Studio solution
if (MSVC)
    vs_set_filters(TARGET ymir-headless-service)
    vs_set_filters(TARGET ymir-headless)
    set_target_properties(ymir-headless-service PROPERTIES FOLDER "Ymir")
    set_target_properties(ymir-headless PROPERTIES FOLDER "Ymir")
endif ()
