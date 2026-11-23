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
 * @file fork_safety.h
 * @brief Fork safety for buffered log output.
 */

#pragma once

namespace cvknx {

/// Flush all buffered output streams before forking.
///
/// A child inherits the contents of its parent's stdio buffers.  If the parent
/// has logged without flushing, the child emits those lines again as soon as it
/// writes its own output — the startup log is then repeated once per worker
/// process.
///
/// Call this immediately before every fork() of a process that logs.
void flush_before_fork();

}  // namespace cvknx
