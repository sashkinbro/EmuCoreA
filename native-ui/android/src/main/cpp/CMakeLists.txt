cmake_minimum_required(VERSION 3.22)
project(emucorea_xmb CXX)

add_library(emucorea_xmb SHARED
    xmb_jni.cpp
    xmb_renderer.cpp
    png_image.cpp
    pgf_font.cpp
)

target_compile_features(emucorea_xmb PRIVATE cxx_std_17)
target_compile_options(emucorea_xmb PRIVATE -Wall -Wextra)

find_library(android-lib android)
find_library(log-lib log)
find_library(GLESv3-lib GLESv3)
find_library(z-lib z)
target_link_libraries(emucorea_xmb ${android-lib} ${log-lib} ${GLESv3-lib} ${z-lib})
