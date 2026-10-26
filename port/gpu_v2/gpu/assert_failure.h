// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdexcept>

namespace iss {
/// Thrown by a failed ASSERT/UNREACHABLE while iss::assert_throws is set (host_hooks.cpp), so a
/// catch takes the assertions only and not an unrelated error (bad_alloc, a library's own throw).
/// A std::runtime_error: catches of std::exception (the AOT threads) take it too.
struct AssertFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
} // namespace iss
