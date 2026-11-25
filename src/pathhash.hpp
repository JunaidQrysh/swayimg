// SPDX-License-Identifier: MIT
// Fast hash and equality for path keyed containers.

#pragma once

#include <filesystem>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

/**
 * std::hash<path> and path::operator== iterate over path components and
 * create temporary path objects for each one, which is very slow for big
 * containers. Use the native string instead: paths stored in containers are
 * absolute and normalized, so string equality matches path equality.
 */
struct PathHash {
    size_t operator()(const std::filesystem::path& path) const noexcept
    {
        return std::hash<std::string_view> {}(path.native());
    }
};

/** Native string equality for paths, see PathHash. */
struct PathEqual {
    bool operator()(const std::filesystem::path& l,
                    const std::filesystem::path& r) const noexcept
    {
        return l.native() == r.native();
    }
};

template <typename T>
using PathMap = std::unordered_map<std::filesystem::path, T, PathHash, PathEqual>;

using PathSet = std::unordered_set<std::filesystem::path, PathHash, PathEqual>;
