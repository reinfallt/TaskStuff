cmake_minimum_required(VERSION 3.16)

project(task_stuff LANGUAGES CXX)

add_library(task_stuff
    task_stuff.cpp)

set_property(TARGET task_stuff PROPERTY CXX_STANDARD 20)
