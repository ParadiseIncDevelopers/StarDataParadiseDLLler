cmake_minimum_required(VERSION 4.2)
project(SdpNativePackageConsumer LANGUAGES CXX)

find_package(StarDataParadiseDLLler 0.4.0 EXACT CONFIG REQUIRED)

add_executable(sdp_native_consumer main.cpp)
target_compile_features(sdp_native_consumer PRIVATE cxx_std_20)
target_compile_options(sdp_native_consumer PRIVATE /utf-8)
target_compile_definitions(sdp_native_consumer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(sdp_native_consumer PRIVATE StarDataParadise::StarDataParadiseDLLler)
add_custom_command(TARGET sdp_native_consumer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:StarDataParadise::StarDataParadiseDLLler>" "$<TARGET_FILE_DIR:sdp_native_consumer>"
    VERBATIM)

option(SDP_ADDRESS_SANITIZER "Use MSVC AddressSanitizer in the standalone consumer" OFF)
if(SDP_ADDRESS_SANITIZER)
    target_compile_options(sdp_native_consumer PRIVATE /fsanitize=address /Zi)
    target_link_options(sdp_native_consumer PRIVATE /DEBUG /INCREMENTAL:NO)
endif()
