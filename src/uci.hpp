// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <iosfwd>

namespace mors {

// Runs the blocking UCI command loop. The stream overload keeps the protocol
// frontend testable without coupling it to the process-wide standard streams.
int run_uci(std::istream& input, std::ostream& output);
int run_uci();

} // namespace mors
