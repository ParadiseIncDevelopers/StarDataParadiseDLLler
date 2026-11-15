cmake_minimum_required(VERSION 4.2)
project(StarDataParadiseDLLler VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(CTest)

add_library(StarDataParadiseDLLler SHARED
    StarDataParadiseDLLler/dllmain.cpp
    StarDataParadiseDLLler/version.cpp
    StarDataParadiseDLLler/api.cpp
    StarDataParadiseDLLler/transport.cpp)
target_compile_definitions(StarDataParadiseDLLler PRIVATE STARDATAPARADISEDLLLER_EXPORTS)
target_include_directories(StarDataParadiseDLLler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

if(BUILD_TESTING)
    add_executable(native_version_smoke tests/version_smoke.c)
    target_link_libraries(native_version_smoke PRIVATE StarDataParadiseDLLler)
    add_test(NAME native_version COMMAND native_version_smoke)
endif()

include(cmake/ProtocolContract.cmake)
if(BUILD_TESTING)
    add_executable(contract_consumer "${sdp_contract_SOURCE_DIR}/tests/wire_smoke.cpp")
    target_link_libraries(contract_consumer PRIVATE sdp_wire)
    target_compile_options(contract_consumer PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME pinned_wire_contract COMMAND contract_consumer
        "${sdp_contract_SOURCE_DIR}/golden/values.json" "${sdp_contract_SOURCE_DIR}/capabilities.json")
endif()

include(cmake/Transport.cmake)
target_link_libraries(StarDataParadiseDLLler PRIVATE sdp_wire sdp_tls)
include(cmake/InstallPackage.cmake)
