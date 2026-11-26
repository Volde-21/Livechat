cmake_minimum_required(VERSION 3.15)
project(livechat CXX)

# Runtime di-link statis supaya tidak butuh Visual C++ Redistributable di PC pemain
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(livechat SHARED livechat.cpp)
set_target_properties(livechat PROPERTIES PREFIX "" SUFFIX ".asi")
target_link_libraries(livechat PRIVATE gdi32 user32)
