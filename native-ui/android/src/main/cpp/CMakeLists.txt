cmake_minimum_required(VERSION 3.22)
project(emucorea_xmb CXX)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)
set(AT3_DIR ${CORE_DIR}/ext/at3_standalone)

add_library(emucorea_xmb SHARED
    xmb_jni.cpp
    xmb_renderer.cpp
    png_image.cpp
    pgf_font.cpp
    xmb_extract.cpp
    psp_rlz.cpp
    psp_gim.cpp
    psp_vag.cpp
    at3_decoder.cpp
    third_party/vlnky/psp_wave.cpp
    third_party/vlnky/rco_error.cpp
    third_party/vlnky/rco/rco_reader.cpp
    ${AT3_DIR}/atrac.cpp
    ${AT3_DIR}/atrac3.cpp
    ${AT3_DIR}/atrac3plus.cpp
    ${AT3_DIR}/atrac3plusdec.cpp
    ${AT3_DIR}/atrac3plusdsp.cpp
    ${AT3_DIR}/get_bits.cpp
    ${AT3_DIR}/fft.cpp
    at3_compat.cpp
)

target_include_directories(emucorea_xmb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vlnky
    ${AT3_DIR}
    ${CORE_DIR}
)

target_compile_features(emucorea_xmb PRIVATE cxx_std_17)
target_compile_options(emucorea_xmb PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
    $<$<CONFIG:Debug>:-O2>
    $<$<NOT:$<CONFIG:Debug>>:-O3>
    $<$<NOT:$<CONFIG:Debug>>:-flto=thin>
)
target_link_options(emucorea_xmb PRIVATE
    -Wl,--gc-sections
    $<$<NOT:$<CONFIG:Debug>>:-flto=thin>
)

find_library(android-lib android)
find_library(log-lib log)
find_library(GLESv3-lib GLESv3)
find_library(z-lib z)
target_link_libraries(emucorea_xmb ${android-lib} ${log-lib} ${GLESv3-lib} ${z-lib})
