include(GNUInstallDirs)
set(SKAI_INSTALL_WEB_DIR "${CMAKE_INSTALL_FULL_DATADIR}/skai-edge/web")
configure_file(config/config.install.yaml.in install/config.yaml @ONLY)
file(READ "${PROJECT_SOURCE_DIR}/systemd/skai-edge.service" SKAI_INSTALL_UNIT)
string(REPLACE "/usr/local/bin/skai-edge" "\"${CMAKE_INSTALL_FULL_BINDIR}/skai-edge\""
    SKAI_INSTALL_UNIT "${SKAI_INSTALL_UNIT}")
file(WRITE "${PROJECT_BINARY_DIR}/install/skai-edge.service" "${SKAI_INSTALL_UNIT}")
configure_file(cmake/InstallState.cmake.in install/InstallState.cmake @ONLY)

add_executable(skai-database-seed src/storage/database_seed.cpp)
target_link_libraries(skai-database-seed PRIVATE skai-storage)
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/install/skai-edge.db"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${PROJECT_BINARY_DIR}/install/skai-edge.db"
        "${PROJECT_BINARY_DIR}/install/skai-edge.db-wal" "${PROJECT_BINARY_DIR}/install/skai-edge.db-shm"
    COMMAND skai-database-seed "${PROJECT_BINARY_DIR}/install/skai-edge.db"
    DEPENDS skai-database-seed VERBATIM)
add_custom_target(skai-install-assets DEPENDS "${PROJECT_BINARY_DIR}/install/skai-edge.db")
add_dependencies(skai-edge skai-install-assets)
set_target_properties(skai-edge PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# Paths in YAML and the unit are configured together; reject a late override.
install(CODE "if(NOT CMAKE_INSTALL_PREFIX STREQUAL \"${CMAKE_INSTALL_PREFIX}\")
    message(FATAL_ERROR \"Configure CMAKE_INSTALL_PREFIX before building - install-time --prefix would break configured paths\")
endif()" COMPONENT Runtime)
install(TARGETS skai-edge RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
install(DIRECTORY web/ DESTINATION "${CMAKE_INSTALL_DATADIR}/skai-edge/web"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    COMPONENT Runtime FILES_MATCHING PATTERN "*.html" PATTERN "*.js" PATTERN "*.css")
install(FILES "${PROJECT_BINARY_DIR}/install/skai-edge.service"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system" COMPONENT Runtime)
install(FILES docs/dependency-revisions.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/skai-edge" COMPONENT Runtime)
install(SCRIPT "${PROJECT_BINARY_DIR}/install/InstallState.cmake" COMPONENT Runtime)
