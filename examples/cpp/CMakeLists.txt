cmake_minimum_required(VERSION 4.2)
project(native_example LANGUAGES CXX)

find_package(StarDataParadiseDLLler 0.4.1 EXACT CONFIG REQUIRED)
add_executable(native_example main.cpp)
target_compile_features(native_example PRIVATE cxx_std_20)
target_link_libraries(native_example PRIVATE StarDataParadise::StarDataParadiseDLLler)
add_custom_command(TARGET native_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:StarDataParadise::StarDataParadiseDLLler>"
            "$<TARGET_FILE_DIR:native_example>")
