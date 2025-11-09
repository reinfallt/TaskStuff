cmake_minimum_required(VERSION 3.16)

project(future_continuation LANGUAGES CXX)

add_library(future_continuation
    future_continuation.cpp)

set_property(TARGET future_continuation PROPERTY CXX_STANDARD 20)
