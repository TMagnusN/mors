// MORS - Syzygy regression checks (AGPL-3.0-or-later)
#include "syzygy/syzygy.hpp"
#include "chess/position.hpp"
#include <iostream>

bool run_syzygy_tests() {
    using namespace mors;
    using syzygy::Wdl;
    bool ok = syzygy::init("");
    auto p = Position::from_fen("8/8/8/8/8/2K5/3Q4/7k w - - 0 1");
    ok &= syzygy::max_pieces() == 0;
    ok &= !syzygy::probe_wdl(*p, {}).has_value();
    ok &= !syzygy::probe_root(*p, {}).has_value();
    ok &= syzygy::score(Wdl::Win, 7, true) == VALUE_TB - 7;
    ok &= syzygy::score(Wdl::Loss, 7, true) == -VALUE_TB + 7;
    ok &= syzygy::score(Wdl::CursedWin, 7, true) == VALUE_DRAW;
    ok &= syzygy::score(Wdl::BlessedLoss, 7, true) == VALUE_DRAW;
    ok &= syzygy::score(Wdl::CursedWin, 7, false) == VALUE_TB - 7;
    ok &= syzygy::score(Wdl::BlessedLoss, 7, false) == -VALUE_TB + 7;
    syzygy::shutdown();
    ok &= syzygy::init(""); // reinitialization after free
    syzygy::shutdown();
    std::cout << (ok ? "PASS" : "FAIL") << " Syzygy disabled lifecycle and score semantics\n";
    return ok;
}
