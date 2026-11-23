// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file coverage.h
 * @brief Flush gcov counters before a forked child ends through _Exit().
 *
 * `std::_Exit()` skips atexit handlers, and gcov writes its `.gcda` files from
 * one of them.  A forked worker that exits through `_Exit()` would therefore
 * be invisible to `tools/coverage.sh` — every line it executed would look
 * uncovered.
 *
 * `KNX_COVERAGE_DUMP()` flushes the counters explicitly.  It compiles to
 * nothing unless the build was configured with `-DENABLE_COVERAGE=ON`, so
 * production binaries are unaffected.
 */

#pragma once

#if defined(KNX_COVERAGE)
extern "C" void __gcov_dump(void);

/// Write the accumulated coverage counters to the .gcda file (coverage builds only).
#define KNX_COVERAGE_DUMP() __gcov_dump()
#else
#define KNX_COVERAGE_DUMP() \
  do {                      \
  } while (false)
#endif
