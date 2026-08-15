set(MPSERVER
    main.cpp
    config.hpp
    config.cpp
    shim.hpp
    shim.cpp
)
source_group(apps\\openmw-mp-server FILES ${MPSERVER})

openmw_add_executable(openmw-mp-server
    ${MPSERVER}
)

# components carries both halves this binary is made of: Net::Session (the same
# ENet wrapper the client uses) and LuaUtil's serializer (the same wire and
# campaign codec). Linking the monolithic archive pulls only what is referenced,
# which is how tes3mp's own server is built.
target_link_libraries(openmw-mp-server
    Boost::program_options
    components
)

if (WIN32)
    target_sources(openmw-mp-server PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
    install(TARGETS openmw-mp-server RUNTIME DESTINATION ".")
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-mp-server PRIVATE --coverage)
    target_link_libraries(openmw-mp-server gcov)
endif()
