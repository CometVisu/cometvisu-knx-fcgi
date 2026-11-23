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

if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_full_flow
    test_full_flow.cpp
)
target_link_libraries(test_full_flow PRIVATE
    cvknx_handlers
    cvknx_knxd
    cvknx_state
    cvknx_router
    cvknx_mocks
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME test_full_flow COMMAND test_full_flow)
endif()

# Real E2E test — requires a running knxd (e.g. via knxd-start-ipt task).
# Uses the real KnxdClient, no mocks. Tests the full path from Router
# down through the eibd protocol to the knxd Unix socket.
if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_real_e2e
    test_real_e2e.cpp
)
target_link_libraries(test_real_e2e PRIVATE
    cvknx_handlers
    cvknx_knxd
    cvknx_state
    cvknx_router
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME test_real_e2e COMMAND test_real_e2e)
endif()

# Set KNXD_SOCKET for the real E2E test to use the IPT knxd socket.
# The test itself also checks the KNXD_SOCKET env var and defaults to
# /tmp/knxd-ipt, but setting it here ensures VS Code test runners use
# the right socket without manual env var configuration.
if(KNX_BUILD_KNXD_TRANSPORT)
    set_tests_properties(test_real_e2e PROPERTIES
        ENVIRONMENT "KNXD_SOCKET=/tmp/knxd-ipt"
    )
endif()

# main(): command line, configuration validation and the fork-based supervisor.
# The worker pool only exists as processes, so the binary is driven directly.
add_executable(test_main_e2e
    test_main_e2e.cpp
)
target_link_libraries(test_main_e2e PRIVATE cvknx_transport GTest::gtest GTest::gtest_main pthread)
target_compile_definitions(test_main_e2e PRIVATE CVKNX_BINARY="$<TARGET_FILE:cometvisu-knx-fcgi>")
add_dependencies(test_main_e2e cometvisu-knx-fcgi)
add_test(NAME test_main_e2e COMMAND test_main_e2e)
set_tests_properties(test_main_e2e PROPERTIES TIMEOUT 180)

# ---------------------------------------------------------------------------
# Black-box tests of the direct KNXnet/IP transports.
#
# These start a real knxd (KNXnet/IP router and/or tunnel server), inject
# telegrams with knxtool, monitor the multicast backbone from a test process
# and drive the deployed binary over a direct FastCGI socket.  They need no
# knxd headers, so they are built in every configuration; at run time they
# skip themselves when knxd/knxtool are not installed.
# ---------------------------------------------------------------------------
add_library(cvknx_blackbox STATIC knxnetip_blackbox.cpp)
target_include_directories(cvknx_blackbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cvknx_blackbox PUBLIC CVKNX_BINARY="$<TARGET_FILE:cometvisu-knx-fcgi>")
target_link_libraries(cvknx_blackbox PUBLIC GTest::gtest pthread)
add_dependencies(cvknx_blackbox cometvisu-knx-fcgi)

add_executable(test_ip_routing_blackbox
    test_ip_routing_blackbox.cpp
)
target_link_libraries(test_ip_routing_blackbox PRIVATE cvknx_blackbox GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_ip_routing_blackbox COMMAND test_ip_routing_blackbox)
set_tests_properties(test_ip_routing_blackbox PROPERTIES TIMEOUT 300)
set_tests_properties(test_ip_routing_blackbox PROPERTIES LABELS "blackbox")

add_executable(test_ip_tunnel_blackbox
    test_ip_tunnel_blackbox.cpp
)
target_link_libraries(test_ip_tunnel_blackbox PRIVATE cvknx_blackbox GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_ip_tunnel_blackbox COMMAND test_ip_tunnel_blackbox)
set_tests_properties(test_ip_tunnel_blackbox PROPERTIES TIMEOUT 300)
set_tests_properties(test_ip_tunnel_blackbox PROPERTIES LABELS "blackbox")
