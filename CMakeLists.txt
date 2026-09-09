cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(NOT NEO_HOSTED_BUILD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")
endif()

set(NEOSHARED_ROOT "" CACHE PATH
    "Path to the neoshared source repository; defaults to ../neoshared")

if(NOT NEOSHARED_ROOT AND DEFINED ENV{NEOSHARED_ROOT}
   AND NOT "$ENV{NEOSHARED_ROOT}" STREQUAL "")
    set(NEOSHARED_ROOT "$ENV{NEOSHARED_ROOT}" CACHE PATH
        "Path to the neoshared source repository" FORCE)
endif()

if(NOT NEOSHARED_ROOT)
    get_filename_component(NEOSHARED_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../neoshared" ABSOLUTE)
else()
    get_filename_component(NEOSHARED_ROOT "${NEOSHARED_ROOT}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(NEOSHARED_ROOT "${NEOSHARED_ROOT}" CACHE PATH
    "Path to the neoshared source repository" FORCE)

if(NOT EXISTS "${NEOSHARED_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "neoshared was not found at '${NEOSHARED_ROOT}'. "
        "Clone the neoshared repository beside this repository, or configure "
        "with -DNEOSHARED_ROOT=/path/to/neoshared.")
endif()

include("${NEOSHARED_ROOT}/cmake/NeoVersion.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoWasm.cmake")
neo_read_version_header(NEOGFF_VERSION
    "${CMAKE_CURRENT_LIST_DIR}/src/core/Version.hpp"
    NEOGFF_VERSION_STRING)
project(NeoGFFCpp VERSION ${NEOGFF_VERSION} DESCRIPTION "C++17/wxWidgets BioWare GFF editor" LANGUAGES CXX)

if(MSVC AND NOT NEO_HOSTED_BUILD)
    foreach(_neo_runtime_flag_var IN ITEMS
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_neo_runtime_flag_var})
            string(REGEX REPLACE "(^|[ /-])MDd?($|[ ]|/)" "\\1MT\\2" ${_neo_runtime_flag_var} "${${_neo_runtime_flag_var}}")
        endif()
    endforeach()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(neo_force_static_msvc_runtime target_name)
    if(MSVC AND NOT NEO_HOSTED_BUILD AND TARGET ${target_name})
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/MTd>
            $<$<NOT:$<CONFIG:Debug>>:/MT>
        )
    endif()
endfunction()

function(neo_force_static_msvc_runtime_for_directory_targets)
    if(MSVC AND NOT NEO_HOSTED_BUILD)
        get_property(_neo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_neo_target IN LISTS _neo_targets)
            get_target_property(_neo_type ${_neo_target} TYPE)
            if(NOT _neo_type STREQUAL "INTERFACE_LIBRARY" AND NOT _neo_type STREQUAL "UTILITY")
                neo_force_static_msvc_runtime(${_neo_target})
            endif()
        endforeach()
    endif()
endfunction()

include(GNUInstallDirs)

message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()



option(NEOGFF_BUILD_CLI "Build the NeoGFF command-line utility" ON)

add_library(neogff_core INTERFACE)
target_include_directories(neogff_core INTERFACE src)
target_link_libraries(neogff_core INTERFACE neoshared::gff_app neoshared::tslpatcher)
target_compile_features(neogff_core INTERFACE cxx_std_17)
if(NEOGFF_BUILD_CLI)
    add_executable(neogff-cli src/cli/main.cpp)
    target_link_libraries(neogff-cli PRIVATE neogff_core)
    if(MSVC)
        target_compile_options(neogff-cli PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neogff-cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS neogff-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()


option(NEOGFF_BUILD_WX_GUI "Build the wxWidgets desktop GUI when wxWidgets is available" ON)
option(NEOGFF_REQUIRE_WX_GUI "Fail configure if wxWidgets is unavailable while NEOGFF_BUILD_WX_GUI is ON" OFF)

function(neogff_link_wx target_name)
    target_compile_definitions(${target_name} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

    set(_wx_targets)
    foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wx::aui wxWidgets::wxWidgets)
        if(TARGET ${_wx_target})
            list(APPEND _wx_targets ${_wx_target})
        endif()
    endforeach()

    if(_wx_targets)
        target_link_libraries(${target_name} PUBLIC ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PUBLIC ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PUBLIC ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PUBLIC ${wxWidgets_LIBRARIES})
    endif()

    neo_force_static_msvc_runtime(${target_name})
endfunction()

option(NEOGFF_BUILD_WX_MODULE "Build reusable editor panel" OFF)

if(NEOGFF_BUILD_WX_GUI OR NEOGFF_BUILD_WX_MODULE)
    set(NEOGFF_WX_COMPONENTS core base adv aui)
    # vcpkg provides CMake package targets such as wx::core and wx::base.
    # Prefer CONFIG mode on Windows/vcpkg, but keep FindwxWidgets/wx-config as
    # the first choice on Unix-like systems with traditional wxWidgets installs.
    set(_neo_prefer_wx_config_package OFF)
    if(WIN32 OR DEFINED VCPKG_TARGET_TRIPLET OR CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
        set(_neo_prefer_wx_config_package ON)
    endif()

    if(_neo_prefer_wx_config_package)
        find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOGFF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOGFF_WX_COMPONENTS})
        endif()
    else()
        find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOGFF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOGFF_WX_COMPONENTS})
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
    endif()

    if(wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets)
        if(NOT EXISTS "${NEOSHARED_ROOT}/include/neoshared/GffResourceDocument.hpp")
            message(FATAL_ERROR "Update NeoShared: hosted GFF resource API required")
        endif()
        add_library(neogff_wx STATIC src/wx/EditorPanel.cpp)
        add_library(neogff::wx ALIAS neogff_wx)
        target_include_directories(neogff_wx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(neogff_wx PUBLIC neogff_core neoshared::wx)
        neogff_link_wx(neogff_wx)
        if(MSVC)
            target_compile_options(neogff_wx PRIVATE /W4 /permissive-)
        else()
            target_compile_options(neogff_wx PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        if(NEOGFF_BUILD_WX_GUI)
        set(NEOGFF_WX_SOURCES src/wx/main.cpp)
        if(WIN32)
            neo_configure_windows_version_resource(
                NEOGFF_WX_SOURCES_RC
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogff.rc.in"
                "neogff.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogff.ico")
            list(APPEND NEOGFF_WX_SOURCES "${NEOGFF_WX_SOURCES_RC}")
        endif()

        if(WIN32)
            add_executable(NeoGFF WIN32 ${NEOGFF_WX_SOURCES})
        elseif(APPLE)
            add_executable(NeoGFF MACOSX_BUNDLE ${NEOGFF_WX_SOURCES})
        else()
            add_executable(NeoGFF ${NEOGFF_WX_SOURCES})
        endif()
        neo_configure_macos_bundle(NeoGFF
            NAME "NeoGFF"
            IDENTIFIER "com.vrifftech.neogff"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogff.icns"
        )
        neo_configure_wasm_target(NeoGFF
            NAME "NeoGFF"
            SLUG "neogff"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogff.svg"
        )

        target_link_libraries(NeoGFF PRIVATE neogff_wx)
        target_include_directories(NeoGFF PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
        target_link_libraries(NeoGFF PRIVATE neoshared::wx)
        neogff_link_wx(NeoGFF)
        target_compile_features(NeoGFF PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(NeoGFF PRIVATE /W4 /permissive-)
        else()
            target_compile_options(NeoGFF PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        install(TARGETS NeoGFF
            BUNDLE DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        if(UNIX AND NOT APPLE)
            foreach(NEO_ICON_SIZE 16 24 32 48 64 128 256 512)
                install(FILES resources/icons/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/neogff.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/apps
                    RENAME neogff.png
                )
            endforeach()
            install(FILES resources/neogff.svg
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
                RENAME neogff.svg
            )
            configure_file(resources/neogff.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/neogff.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neogff.desktop
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
            )
        endif()
        endif() # standalone wrapper
    elseif(NEOGFF_REQUIRE_WX_GUI)
        message(FATAL_ERROR "wxWidgets was not found, but NEOGFF_REQUIRE_WX_GUI is ON. On Windows, install the pinned wxWidgets 3.3.3 overlay with ../neoshared/scripts/install-wxwidgets.ps1 and configure with the vcpkg toolchain. On Debian or Ubuntu, install libwxgtk3.2-dev.")
    else()
        message(WARNING "wxWidgets was not found. Core and CLI targets will still build; install wxWidgets to build NeoGFF.")
    endif()
endif()

neo_force_static_msvc_runtime_for_directory_targets()
neo_apply_minimal_release_to_directory_targets()
