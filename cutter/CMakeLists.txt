cmake_minimum_required(VERSION 3.13)


add_executable(cutter cutter.cpp)
target_link_options(cutter PUBLIC "-pthread")
add_definitions(-std=c++14)
set_property(TARGET cutter PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
target_link_libraries(cutter avformat avcodec avfilter avutil swscale swresample
    z ass)
