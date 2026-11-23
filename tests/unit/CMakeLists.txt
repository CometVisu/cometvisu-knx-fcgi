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

# ---- Unit tests ----
add_executable(test_query_string
    test_query_string.cpp
)
target_link_libraries(test_query_string PRIVATE cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_query_string COMMAND test_query_string)

add_executable(test_hex
    test_hex.cpp
)
target_link_libraries(test_hex PRIVATE cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_hex COMMAND test_hex)

if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_knxd_protocol
    test_knxd_protocol.cpp
)
target_link_libraries(test_knxd_protocol PRIVATE cvknx_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_protocol COMMAND test_knxd_protocol)
endif()

if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_knxd_address
    test_knxd_address.cpp
)
target_link_libraries(test_knxd_address PRIVATE cvknx_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_address COMMAND test_knxd_address)
endif()

add_executable(test_json_builder
    test_json_builder.cpp
)
target_link_libraries(test_json_builder PRIVATE cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_json_builder COMMAND test_json_builder)

add_executable(test_session_store
    test_session_store.cpp
)
target_link_libraries(test_session_store PRIVATE cvknx_state GTest::gtest GTest::gtest_main)
add_test(NAME test_session_store COMMAND test_session_store)

add_executable(test_login_handler
    test_login_handler.cpp
)
target_link_libraries(test_login_handler PRIVATE cvknx_handlers cvknx_state GTest::gtest GTest::gtest_main)
add_test(NAME test_login_handler COMMAND test_login_handler)

add_executable(test_debug_log
    test_debug_log.cpp
)
target_link_libraries(test_debug_log PRIVATE cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_debug_log COMMAND test_debug_log)

add_executable(test_version
    test_version.cpp
)
target_link_libraries(test_version PRIVATE cvknx_version GTest::gtest GTest::gtest_main)
add_test(NAME test_version COMMAND test_version)

add_executable(test_fork_safety
    test_fork_safety.cpp
)
target_link_libraries(test_fork_safety PRIVATE cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fork_safety COMMAND test_fork_safety)

add_executable(test_fcgi_server
    test_fcgi_server.cpp
)
target_link_libraries(test_fcgi_server PRIVATE cvknx_fcgi cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_server COMMAND test_fcgi_server)

add_executable(test_fcgi_request
    test_fcgi_request.cpp
)
target_link_libraries(test_fcgi_request PRIVATE cvknx_fcgi GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_request COMMAND test_fcgi_request)

add_executable(test_fcgi_server_run
    test_fcgi_server_run.cpp
)
target_link_libraries(test_fcgi_server_run PRIVATE cvknx_fcgi cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_server_run COMMAND test_fcgi_server_run)

add_executable(test_bus_connector
    test_bus_connector.cpp
)
target_link_libraries(test_bus_connector PRIVATE cvknx_bus cvknx_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_bus_connector COMMAND test_bus_connector)

if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_knxd_client_socket
    test_knxd_client_socket.cpp
)
target_link_libraries(test_knxd_client_socket PRIVATE cvknx_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_client_socket COMMAND test_knxd_client_socket)

add_executable(test_knxd_client_protocol
    test_knxd_client_protocol.cpp
)
target_link_libraries(test_knxd_client_protocol PRIVATE cvknx_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_client_protocol COMMAND test_knxd_client_protocol)
endif()

add_executable(test_cache_reader
    test_cache_reader.cpp
)
target_link_libraries(test_cache_reader PRIVATE cvknx_cache_reader cvknx_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_cache_reader COMMAND test_cache_reader)

add_executable(test_sigpipe
    test_sigpipe.cpp
)
target_link_libraries(test_sigpipe PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME test_sigpipe COMMAND test_sigpipe)

add_executable(test_shared_group_cache
    test_shared_group_cache.cpp
)
target_link_libraries(test_shared_group_cache PRIVATE cvknx_state GTest::gtest GTest::gtest_main)
add_test(NAME test_shared_group_cache COMMAND test_shared_group_cache)

add_executable(test_bus_config
    test_bus_config.cpp
)
target_link_libraries(test_bus_config PRIVATE cvknx_bus GTest::gtest GTest::gtest_main)
add_test(NAME test_bus_config COMMAND test_bus_config)

add_executable(test_cemi
    test_cemi.cpp
)
target_link_libraries(test_cemi PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_cemi COMMAND test_cemi)

add_executable(test_knxnetip_frame
    test_knxnetip_frame.cpp
)
target_link_libraries(test_knxnetip_frame PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_knxnetip_frame COMMAND test_knxnetip_frame)

add_executable(test_shared_tx_queue
    test_shared_tx_queue.cpp
)
target_link_libraries(test_shared_tx_queue PRIVATE cvknx_state GTest::gtest GTest::gtest_main)
add_test(NAME test_shared_tx_queue COMMAND test_shared_tx_queue)

add_executable(test_shared_tx_bus_link
    test_shared_tx_bus_link.cpp
)
target_link_libraries(test_shared_tx_bus_link PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_shared_tx_bus_link COMMAND test_shared_tx_bus_link)

add_executable(test_knxnetip_routing
    test_knxnetip_routing.cpp
)
target_link_libraries(test_knxnetip_routing PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_knxnetip_routing COMMAND test_knxnetip_routing)

add_executable(test_knxnetip_tunnel
    test_knxnetip_tunnel.cpp
)
target_link_libraries(test_knxnetip_tunnel PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_knxnetip_tunnel COMMAND test_knxnetip_tunnel)

add_executable(test_discovery
    test_discovery.cpp
)
target_link_libraries(test_discovery PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_discovery COMMAND test_discovery)

add_executable(test_knxnetip_frame_negative
    test_knxnetip_frame_negative.cpp
)
target_link_libraries(test_knxnetip_frame_negative PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_knxnetip_frame_negative COMMAND test_knxnetip_frame_negative)

add_executable(test_transport_edge_cases
    test_transport_edge_cases.cpp
)
target_link_libraries(test_transport_edge_cases PRIVATE cvknx_transport GTest::gtest GTest::gtest_main)
add_test(NAME test_transport_edge_cases COMMAND test_transport_edge_cases)

add_executable(test_cache_reader_backpressure
    test_cache_reader_backpressure.cpp
)
target_link_libraries(test_cache_reader_backpressure PRIVATE
    cvknx_cache_reader cvknx_handlers cvknx_transport cvknx_state
    GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_cache_reader_backpressure COMMAND test_cache_reader_backpressure)
