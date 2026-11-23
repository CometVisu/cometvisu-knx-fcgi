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
 * @file bus_factory.h
 * @brief The single place that knows which transport implementation exists.
 */

#pragma once

#include <memory>

#include "bus/bus_config.h"
#include "bus/bus_link.h"

namespace cvknx {

/// Create the bus link for @p config.
/// @return the link, or nullptr when the requested transport is not compiled
///         in (e.g. `knxd` with -DKNX_BUILD_KNXD_TRANSPORT=OFF).
[[nodiscard]] std::unique_ptr<BusLink> create_bus_link(const BusConfig& config);

}  // namespace cvknx
