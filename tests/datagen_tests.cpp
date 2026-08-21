// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "tools/bullet_format.hpp"

#include <cstdint>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL datagen: " << message << '\n';
    return condition;
}

bool test_white_record() {
    auto parsed = mors::Position::from_fen(mors::START_FEN);
    if (!expect(parsed.has_value(), "start FEN must parse"))
        return false;
    const mors::tools::BulletChessBoard record =
        mors::tools::encode_bullet_record(*parsed, 123, 2);
    return expect(sizeof(record) == 32, "record must be 32 bytes")
        && expect(record.occupancy == 0xFFFF'0000'0000'FFFFULL,
                  "white occupancy must retain A1-H8 coordinates")
        && expect(record.score == 123, "white score must remain STM-relative")
        && expect(record.result == 2, "white win must remain a win for white STM")
        && expect(record.king_square == mors::E1,
                  "white king must be the side-to-move king")
        && expect(record.opponent_king_square == mors::E1,
                  "opponent king must use Bullet's mirrored coordinate")
        && expect((record.pieces[0] & 0x0F) == 3,
                  "A1 rook must use Bullet piece index 3")
        && expect((record.pieces[0] >> 4) == 1,
                  "B1 knight must use Bullet piece index 1");
}

bool test_black_normalisation() {
    auto parsed = mors::Position::from_fen(
        "4k3/8/8/8/8/8/8/4K3 b - - 0 1"
    );
    if (!expect(parsed.has_value(), "black-to-move FEN must parse"))
        return false;
    const mors::tools::BulletChessBoard record =
        mors::tools::encode_bullet_record(*parsed, 75, 2);
    return expect(record.occupancy == ((std::uint64_t{1} << mors::E1)
                                     | (std::uint64_t{1} << mors::E8)),
                  "black STM must vertically normalise the board")
        && expect(record.score == 75,
                  "score must already be side-to-move relative")
        && expect(record.result == 0,
                  "absolute white win must become black-STM loss")
        && expect(record.king_square == mors::E1,
                  "black king must normalise to E1")
        && expect(record.opponent_king_square == mors::E1,
                  "white opponent king must use mirrored coordinate")
        && expect((record.pieces[0] & 0x0F) == 5,
                  "normalised side-to-move king must be ours")
        && expect((record.pieces[0] >> 4) == 13,
                  "normalised opponent king must carry opposite colour");
}

} // namespace

bool run_datagen_tests() {
    const bool passed = test_white_record() && test_black_normalisation();
    if (passed)
        std::cout << "PASS BulletFormat datagen records\n";
    return passed;
}
