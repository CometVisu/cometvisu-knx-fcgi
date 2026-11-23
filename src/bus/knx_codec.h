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
 * @file knx_codec.h
 * @brief Transport-neutral KNX helpers: group addresses and group-value APDUs.
 *
 * `KnxGroupAddress`, `KnxAddress` and `parse_apdu()` describe the KNX
 * application layer, which is identical for every transport (knxd's
 * EIB_GROUP_PACKET, KNXnet/IP routing, KNXnet/IP tunnelling).  Their single
 * implementation lives in `knxd/knxd_protocol.{h,cpp}`, which is compiled
 * into the transport-neutral `cvknx_bus` library — the eibd-specific parts of
 * that file are `#ifdef KNX_WITH_KNXD_TRANSPORT` and simply disappear when
 * knxd is not built.
 *
 * New transport code should include this header (it documents intent), not
 * the knxd protocol header.
 */

#pragma once

#include "knxd/knxd_protocol.h"
