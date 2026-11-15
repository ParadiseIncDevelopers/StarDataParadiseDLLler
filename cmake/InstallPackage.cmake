include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(SDP_ADDRESS_SANITIZER "Instrument native code and tests with MSVC AddressSanitizer" OFF)
if(SDP_ADDRESS_SANITIZER)
    target_compile_options(StarDataParadiseDLLler PRIVATE /fsanitize=address /Zi)
    target_link_options(StarDataParadiseDLLler PRIVATE /DEBUG /INCREMENTAL:NO)
    if(TARGET native_version_smoke)
        target_compile_options(native_version_smoke PRIVATE /fsanitize=address /Zi)
        target_link_options(native_version_smoke PRIVATE /DEBUG /INCREMENTAL:NO)
    endif()
endif()

install(TARGETS StarDataParadiseDLLler EXPORT StarDataParadiseDLLlerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/StarDataParadiseDLLler.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(package_directory "${CMAKE_INSTALL_LIBDIR}/cmake/StarDataParadiseDLLler")
install(EXPORT StarDataParadiseDLLlerTargets NAMESPACE StarDataParadise::
    DESTINATION "${package_directory}")
configure_package_config_file(cmake/StarDataParadiseDLLlerConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/StarDataParadiseDLLlerConfig.cmake"
    INSTALL_DESTINATION "${package_directory}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/StarDataParadiseDLLlerConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/StarDataParadiseDLLlerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/StarDataParadiseDLLlerConfigVersion.cmake"
    DESTINATION "${package_directory}")
install(FILES ABI.md contracts/README.md contracts/sdp-contract-v1.zip
    DESTINATION "${CMAKE_INSTALL_DATADIR}/StarDataParadiseDLLler")
install(FILES licenses/OpenSSL.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/StarDataParadiseDLLler/licenses")
install(FILES "${asio_root}/LICENSE_1_0.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/StarDataParadiseDLLler/licenses" RENAME Asio.txt)
install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/StarDataParadiseDLLler/licenses" RENAME nlohmann-json.txt)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "StarDataParadiseDLLler-${PROJECT_VERSION}-win-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
