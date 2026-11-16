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
 * @file fork_safety.cpp
 * @brief Implementation of flush_before_fork().
 */

#include "util/fork_safety.h"

#include <cstdio>
#include <iostream>

namespace cvknx {

void flush_before_fork() {
  // The C++ streams may be detached from stdio (std::ios::sync_with_stdio),
  // so both layers have to be flushed explicitly.
  std::cout.flush();
  std::clog.flush();
  std::cerr.flush();

  // Drain every C output stream that still holds pending data.
  (void)::fflush(nullptr);
}

}  // namespace cvknx
