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

add_executable(test_knxd_client
    test_knxd_client.cpp
)
target_link_libraries(test_knxd_client PRIVATE cvknx_knxd cvknx_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_client COMMAND test_knxd_client)

add_executable(test_read_handler
    test_read_handler.cpp
)
target_link_libraries(test_read_handler PRIVATE cvknx_handlers cvknx_knxd cvknx_state cvknx_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_read_handler COMMAND test_read_handler)

add_executable(test_write_handler
    test_write_handler.cpp
)
target_link_libraries(test_write_handler PRIVATE cvknx_handlers cvknx_knxd cvknx_state cvknx_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_write_handler COMMAND test_write_handler)

add_executable(test_fcgi_server_socket
    test_fcgi_server_socket.cpp
)
target_link_libraries(test_fcgi_server_socket PRIVATE cvknx_fcgi cvknx_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_server_socket COMMAND test_fcgi_server_socket)

add_executable(test_concurrent_clients
    test_concurrent_clients.cpp
)
target_link_libraries(test_concurrent_clients PRIVATE
    cvknx_fcgi cvknx_util cvknx_router cvknx_transport cvknx_cache_reader cvknx_state
    GTest::gtest GTest::gtest_main ${FCGI_LIBRARY} pthread)
add_test(NAME test_concurrent_clients COMMAND test_concurrent_clients)

if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_knxd_client_concurrency
    test_knxd_client_concurrency.cpp
)
target_link_libraries(test_knxd_client_concurrency PRIVATE cvknx_knxd cvknx_mocks GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_knxd_client_concurrency COMMAND test_knxd_client_concurrency)
endif()

add_executable(test_fork_workers
    test_fork_workers.cpp
)
target_link_libraries(test_fork_workers PRIVATE cvknx_fcgi cvknx_util GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_fork_workers COMMAND test_fork_workers)

# Supervision test: runs the real binary and SIGKILLs its children to verify
# that dead workers and a dead cache reader are replaced.
if(KNX_BUILD_KNXD_TRANSPORT)
add_executable(test_worker_supervision
    test_worker_supervision.cpp
)
target_compile_definitions(test_worker_supervision PRIVATE
    CVKNX_BINARY="$<TARGET_FILE:cometvisu-knx-fcgi>"
)
add_dependencies(test_worker_supervision cometvisu-knx-fcgi)
target_link_libraries(test_worker_supervision PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME test_worker_supervision COMMAND test_worker_supervision)
endif()

add_executable(test_tx_queue_multiprocess
    test_tx_queue_multiprocess.cpp
)
target_link_libraries(test_tx_queue_multiprocess PRIVATE cvknx_state GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_tx_queue_multiprocess COMMAND test_tx_queue_multiprocess)

add_executable(test_transport_recovery
    test_transport_recovery.cpp
)
target_link_libraries(test_transport_recovery PRIVATE
    cvknx_transport cvknx_cache_reader cvknx_state GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_transport_recovery COMMAND test_transport_recovery)
