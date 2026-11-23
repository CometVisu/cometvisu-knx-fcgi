#!/usr/bin/env bash
#
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
#
# Container post-create step: configure and build the project.
#
# `build/` is mounted as a container-private Docker volume (see
# devcontainer.json), so it never contains a cache generated on the host.  As a
# safety net any cache that still points at a different source directory is
# discarded, because CMake refuses to reuse it:
#
#   CMake Error: The current CMakeCache.txt directory ... is different than
#   the directory ... where CMakeCache.txt was created.
#
# Only generated files are removed.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
workspace="$(pwd)"

# ~/.gitconfig is bind-mounted from the host.  If that file does not exist,
# Docker creates a directory at the mount point instead; warn here rather than
# letting the first `git commit` fail with a confusing error.
if [[ -d "$HOME/.gitconfig" ]]; then
  echo "[post-create] WARNING: ~/.gitconfig is a directory, the host has no" \
       "~/.gitconfig. Set the identity manually:"
  echo "[post-create]          git config --global user.name \"...\""
  echo "[post-create]          git config --global user.email \"...\""
fi

# Docker creates the build volume owned by root; hand it to the workspace user.
if [[ -d build && ! -w build ]]; then
  echo "[post-create] Giving build volume to $(id -un):$(id -gn)"
  sudo -n chown -R "$(id -u):$(id -g)" build
fi

for cache in build*/CMakeCache.txt; do
  # No match at all: the glob stays literal.
  [[ -f "$cache" ]] || continue
  dir="$(dirname "$cache")"
  cached_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | head -n 1)"
  if [[ "$cached_src" != "$workspace" ]]; then
    echo "[post-create] Discarding stale CMake cache in '$dir'" \
         "(generated for '${cached_src:-unknown}')"
    rm -rf "$dir"
  fi
done

cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
