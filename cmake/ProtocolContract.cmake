include(FetchContent)

set(SDP_CONTRACT_SHA256 "e6500900a3fa34910781ea92d5e41fe8716511fac6f6d6c689ff9cce15d578ab")
set(SDP_CONTRACT_ARCHIVE "${CMAKE_CURRENT_LIST_DIR}/../contracts/sdp-contract-v1.zip"
    CACHE FILEPATH "Pinned StarDataParadise Server protocol v1 archive")
file(SHA256 "${SDP_CONTRACT_ARCHIVE}" contract_hash)
if(NOT contract_hash STREQUAL SDP_CONTRACT_SHA256)
    message(FATAL_ERROR "Protocol v1 archive differs from the pinned SHA-256.")
endif()

FetchContent_Declare(sdp_contract
    URL "${SDP_CONTRACT_ARCHIVE}"
    URL_HASH "SHA256=${SDP_CONTRACT_SHA256}")
FetchContent_MakeAvailable(sdp_contract)

include("${CMAKE_CURRENT_LIST_DIR}/ProtocolJson.cmake")
add_library(sdp_wire INTERFACE)
target_include_directories(sdp_wire INTERFACE "${sdp_contract_SOURCE_DIR}/cpp")
target_link_libraries(sdp_wire INTERFACE nlohmann_json::nlohmann_json)
