// MORS - Syzygy integration verification tool (AGPL-3.0-or-later)
#include "chess/attacks.hpp"
#include "chess/position.hpp"
#include "syzygy/syzygy.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    mors::initialize_attacks();
    if (!mors::syzygy::init(argv[1])) return 3;
    std::string fen;
    while (std::getline(std::cin, fen)) {
        auto p = mors::Position::from_fen(fen);
        if (!p) { std::cout << "invalid\n"; continue; }
        const auto wdl = mors::syzygy::probe_wdl(*p, {});
        const auto root = mors::syzygy::probe_root(*p, {});
        std::cout << (wdl ? static_cast<int>(*wdl) : -9) << ' '
                  << (root ? static_cast<int>(root->wdl) : -9) << ' '
                  << (root && root->used_dtz);
        if (root) for (auto m : root->moves) {
            std::cout << ' ' << char('a' + mors::file_of(m.from()))
                      << char('1' + mors::rank_of(m.from()))
                      << char('a' + mors::file_of(m.to()))
                      << char('1' + mors::rank_of(m.to()));
            if (m.type() == mors::PROMOTION)
                std::cout << "  nbrq"[m.promotion_type()];
        }
        std::cout << '\n';
    }
    mors::syzygy::shutdown();
}
