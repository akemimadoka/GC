cmake_minimum_required(VERSION 3.15)

project(GC CXX)

add_executable(GC Main.cpp)

target_compile_features(GC PRIVATE cxx_std_20)
target_compile_options(GC PRIVATE -freflection)
