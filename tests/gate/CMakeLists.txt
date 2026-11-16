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

# ---- Coverage gate ----
# Registered in its own directory so it is scheduled *after* every real test:
# the gate only reads the gcov counters those tests produce.
#
# Every source file must stay at or above the value recorded in
# tools/coverage-baseline.json; a file that is not in the baseline (i.e.
# anything new) must be fully covered — every line and every branch arm.
#
# Normal entry point: tools/coverage.sh (builds, runs the tests, then gates).
# A bare `ctest -R coverage_gate` evaluates whatever counters already exist.
if(ENABLE_COVERAGE AND BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME coverage_gate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_coverage.py
                --build-dir ${CMAKE_BINARY_DIR}
                --source-root ${CMAKE_SOURCE_DIR}
                --baseline ${CMAKE_SOURCE_DIR}/tools/coverage-baseline.json)
    set_tests_properties(coverage_gate PROPERTIES RUN_SERIAL TRUE)
endif()
