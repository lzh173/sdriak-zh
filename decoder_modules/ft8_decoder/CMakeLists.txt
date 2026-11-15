cmake_minimum_required(VERSION 3.16...3.24)
project(ft8_decoder)

# Vendored ft8_lib (MIT): FT8/FT4 decoder, CRC/LDPC/message packing and a
# kissfft copy used by the STFT in common/monitor.c.
set(FT8LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/vendor/ft8_lib")

file(GLOB SRC "src/*.cpp")
file(GLOB FT8LIB_SRC
    "${FT8LIB_DIR}/ft8/*.c"
    "${FT8LIB_DIR}/common/*.c"
    "${FT8LIB_DIR}/fft/*.c")

include(${SDRPP_MODULE_CMAKE})

target_sources(${PROJECT_NAME} PRIVATE ${FT8LIB_SRC})
target_include_directories(${PROJECT_NAME} PRIVATE "${FT8LIB_DIR}")
# monitor.c uses M_PI from <math.h>; MSVC needs this define to expose it.
target_compile_definitions(${PROJECT_NAME} PRIVATE _USE_MATH_DEFINES)
