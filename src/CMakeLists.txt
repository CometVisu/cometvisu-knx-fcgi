# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# ---- Utility library ----
add_library(cvknx_util STATIC
    util/query_string.cpp
    util/json_builder.cpp
    util/hex.cpp
    util/debug_log.cpp
    util/fork_safety.cpp
)

target_include_directories(cvknx_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_util PUBLIC)

# ---- Version library ----
add_library(cvknx_version STATIC
    version.cpp
)

target_include_directories(cvknx_version PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cvknx_version PUBLIC
    APP_VERSION="${PROJECT_VERSION}"
    GIT_HASH="${GIT_HASH}"
)
if(KNXD_BUILD_VERSION)
    target_compile_definitions(cvknx_version PUBLIC KNXD_BUILD_VERSION="${KNXD_BUILD_VERSION}")
endif()
if(KNXD_BUILD_GIT_HASH)
    target_compile_definitions(cvknx_version PUBLIC KNXD_BUILD_GIT_HASH="${KNXD_BUILD_GIT_HASH}")
endif()
target_link_libraries(cvknx_version PUBLIC)

# ---- Bus abstraction: configuration + transport-neutral KNX codec ----
# knxd_protocol.cpp holds the address/APDU helpers shared by every transport;
# its eibd-specific parts are #ifdef'd out when knxd is not built.
add_library(cvknx_bus STATIC
    bus/bus_config.cpp
    bus/bus_connector.cpp
    knxd/knxd_protocol.cpp
)

target_include_directories(cvknx_bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${KNXD_INCLUDE_DIR})
target_link_libraries(cvknx_bus PUBLIC cvknx_util)

# ---- knxd client library (optional: -DKNX_BUILD_KNXD_TRANSPORT=OFF) ----
if(KNX_BUILD_KNXD_TRANSPORT)
    add_library(cvknx_knxd STATIC
        knxd/knxd_client.cpp
    )

    target_include_directories(cvknx_knxd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        ${KNXD_INCLUDE_DIR})
    target_link_libraries(cvknx_knxd PUBLIC cvknx_bus cvknx_util)
else()
    # Nothing from src/knxd/ is compiled; the transport is refused at runtime
    # by BusFactory.
    add_library(cvknx_knxd INTERFACE)
    target_include_directories(cvknx_knxd INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cvknx_knxd INTERFACE cvknx_bus cvknx_util)
endif()

# ---- Transport implementations + factory ----
add_library(cvknx_transport STATIC
    bus/bus_factory.cpp
    bus/shared_tx_bus_link.cpp
    knxnetip/cemi.cpp
    knxnetip/discovery.cpp
    knxnetip/knxnetip_frame.cpp
    knxnetip/routing_bus_link.cpp
    knxnetip/tunneling_bus_link.cpp
)

target_include_directories(cvknx_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_transport PUBLIC cvknx_bus cvknx_knxd cvknx_state)

# ---- State library (sessions + shared group cache + shared TX ring) ----
add_library(cvknx_state STATIC
    state/session_store.cpp
    state/shared_group_cache.cpp
    state/shared_tx_queue.cpp
)

target_include_directories(cvknx_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_state PUBLIC cvknx_util)

# ---- Cache reader library (bus -> SharedGroupCache pump, RX + TX) ----
add_library(cvknx_cache_reader STATIC
    bus/cache_reader.cpp
)

target_include_directories(cvknx_cache_reader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_cache_reader PUBLIC cvknx_bus cvknx_state cvknx_util)

# ---- FCGI server library ----
add_library(cvknx_fcgi STATIC
    fcgi/fcgi_server.cpp
    fcgi/fcgi_request.cpp
)

target_include_directories(cvknx_fcgi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_fcgi PUBLIC ${FCGI_LIBRARY})

# ---- Router library ----
add_library(cvknx_router STATIC
    router/router.cpp
)

target_include_directories(cvknx_router PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_router PUBLIC cvknx_handlers cvknx_fcgi)

# ---- Handlers library ----
add_library(cvknx_handlers STATIC
    handlers/login_handler.cpp
    handlers/read_handler.cpp
    handlers/write_handler.cpp
    handlers/bus_status_json.cpp
)

target_include_directories(cvknx_handlers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknx_handlers PUBLIC cvknx_bus cvknx_state cvknx_util cvknx_version)

# ---- Main executable ----
add_executable(cometvisu-knx-fcgi
    main.cpp
)

target_link_libraries(cometvisu-knx-fcgi PRIVATE
    cvknx_version
    cvknx_fcgi
    cvknx_router
    cvknx_handlers
    cvknx_cache_reader
    cvknx_transport
    cvknx_bus
    cvknx_knxd
    cvknx_state
    cvknx_util
    ${FCGI_LIBRARY}
)
