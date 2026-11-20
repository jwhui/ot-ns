#
#  Copyright (c) 2026, The OTNS Authors.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#  3. Neither the name of the copyright holder nor the
#     names of its contributors may be used to endorse or promote products
#     derived from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
#  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
#  POSSIBILITY OF SUCH DAMAGE.
#

# Supports both in-tree build (included from root OTNS CMakeLists.txt)
# and standalone build for external OpenThread / Matter integration.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(ot-rfsim-v3 VERSION 3.0.0 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(OTNS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
    set(OT_DIR "${OTNS_ROOT_DIR}/openthread" CACHE PATH "Path to OpenThread repository")

    add_library(otns_protocol STATIC
        ${OTNS_ROOT_DIR}/src/protocol/rfsim_v3_codec.cpp
    )
    target_include_directories(otns_protocol PUBLIC ${OTNS_ROOT_DIR}/src)

    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OT_PLATFORM "external" CACHE STRING "" FORCE)
    set(OT_THREAD_VERSION "1.4" CACHE STRING "" FORCE)
    set(OT_APP_CLI OFF CACHE BOOL "" FORCE)
    set(OT_APP_NCP OFF CACHE BOOL "" FORCE)
    set(OT_APP_RCP OFF CACHE BOOL "" FORCE)
    set(OT_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
    set(OT_FTD ON CACHE BOOL "" FORCE)
    set(OT_MTD OFF CACHE BOOL "" FORCE)
    set(OT_RCP ON CACHE BOOL "" FORCE)
    set(OT_15_4 ON CACHE BOOL "" FORCE)
    set(OT_PING_SENDER ON CACHE BOOL "" FORCE)
    set(OT_BUILTIN_MBEDTLS ON CACHE BOOL "" FORCE)
    set(OT_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
    option(OT_RFSIM_TREL "Enable TREL over OTNS v3 virtual backbone in standalone ot-rfsim build" OFF)
    option(OT_RFSIM_BORDER_ROUTING "Enable Border Routing (Infra-If AIL) in standalone ot-rfsim build" OFF)
    if(OT_RFSIM_TREL)
        set(OT_TREL ON CACHE BOOL "" FORCE)
        set(OT_UPTIME ON CACHE BOOL "" FORCE)
    endif()
    if(OT_RFSIM_BORDER_ROUTING)
        set(OT_BORDER_ROUTER ON CACHE BOOL "" FORCE)
        set(OT_BORDER_ROUTING ON CACHE BOOL "" FORCE)
        set(OT_ECDSA ON CACHE BOOL "" FORCE)
        set(OT_EXTERNAL_HEAP ON CACHE BOOL "" FORCE)
        set(OT_NETDATA_PUBLISHER ON CACHE BOOL "" FORCE)
        set(OT_SERVICE ON CACHE BOOL "" FORCE)
        set(OT_SLAAC ON CACHE BOOL "" FORCE)
        set(OT_SRP_CLIENT ON CACHE BOOL "" FORCE)
        set(OT_SRP_SERVER ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(${OT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/openthread EXCLUDE_FROM_ALL)
else()
    set(OTNS_ROOT_DIR "${CMAKE_SOURCE_DIR}")
    set(OT_DIR "${CMAKE_SOURCE_DIR}/openthread")
endif()

# Library mode target (linkable by Matter, OTBR, or standalone CLI apps)
add_library(openthread-rfsim-v3 STATIC
    src/rfsim_client.cpp
    ${OT_DIR}/examples/platforms/utils/mac_frame.cpp
)
target_include_directories(openthread-rfsim-v3
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OT_DIR}/examples/platforms
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OT_DIR}/include
        ${OT_DIR}/src
        ${OT_DIR}/src/core
        ${OT_DIR}/examples/platforms/utils
)
set_source_files_properties(${OT_DIR}/examples/platforms/utils/mac_frame.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-conversion;-Wno-sign-conversion;-Wno-shadow;-Wno-pedantic"
)
target_link_libraries(openthread-rfsim-v3
    PUBLIC
        otns_protocol
        ot-config
)
if(TARGET otns_build_options)
    target_link_libraries(openthread-rfsim-v3 PRIVATE otns_build_options)
endif()

# Standalone external CLI node executable
add_executable(ot-cli-ftd-rfsim
    src/main_cli.cpp
)
target_include_directories(ot-cli-ftd-rfsim
    PRIVATE
        ${OT_DIR}/include
        ${OT_DIR}/src
        ${OT_DIR}/src/core
        ${OT_DIR}/examples/platforms
)
target_link_libraries(ot-cli-ftd-rfsim
    PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,openthread-rfsim-v3>"
        openthread-cli-ftd
        openthread-ftd
        mbedtls
)
if(TARGET otns_build_options)
    target_link_libraries(ot-cli-ftd-rfsim PRIVATE otns_build_options)
endif()

# RCP platform library & standalone Spinel/HDLC RCP binary (for OTBR / POSIX host)
if(TARGET openthread-rcp AND TARGET openthread-radio)
    add_library(openthread-rfsim-v3-rcp STATIC
        src/rfsim_client.cpp
        ${OT_DIR}/examples/platforms/utils/mac_frame.cpp
    )
    target_include_directories(openthread-rfsim-v3-rcp
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${OT_DIR}/examples/platforms
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${OT_DIR}/include
            ${OT_DIR}/src
            ${OT_DIR}/src/core
            ${OT_DIR}/examples/platforms/utils
    )
    target_link_libraries(openthread-rfsim-v3-rcp
        PUBLIC
            otns_protocol
            ot-config-radio
            ot-config
    )
    target_compile_definitions(openthread-rfsim-v3-rcp
        PUBLIC
            OPENTHREAD_RADIO=1
            OPENTHREAD_FTD=0
            OPENTHREAD_MTD=0
    )
    target_compile_definitions(ot-config-radio
        INTERFACE
            OPENTHREAD_CONFIG_MAC_SOFTWARE_CSMA_BACKOFF_ENABLE=1
            OPENTHREAD_CONFIG_MAC_SOFTWARE_RETRANSMIT_ENABLE=1
    )
    if(TARGET otns_build_options)
        target_link_libraries(openthread-rfsim-v3-rcp PRIVATE otns_build_options)
    endif()

    add_executable(ot-rcp-rfsim
        src/main_rcp.cpp
    )
    target_include_directories(ot-rcp-rfsim
        PRIVATE
            ${OT_DIR}/include
            ${OT_DIR}/src
            ${OT_DIR}/src/core
            ${OT_DIR}/examples/platforms
    )
    target_link_libraries(ot-rcp-rfsim
        PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,openthread-rfsim-v3-rcp>"
            openthread-rcp
            openthread-radio
            ot-config-radio
            ot-config
    )
    target_compile_definitions(ot-rcp-rfsim
        PRIVATE
            OPENTHREAD_RADIO=1
            OPENTHREAD_FTD=0
            OPENTHREAD_MTD=0
    )
    if(TARGET otns_build_options)
        target_link_libraries(ot-rcp-rfsim PRIVATE otns_build_options)
    endif()
endif()
