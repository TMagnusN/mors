"""Cross-check MORS against python-chess and exercise a supplied EPD corpus.
Requires python-chess. The tablebase files stay external to the repository.
"""
import argparse
import collections
import json
import random
import shutil
import subprocess
from pathlib import Path
import chess
import chess.engine
import chess.syzygy

parser = argparse.ArgumentParser()
parser.add_argument('--engine', required=True)
parser.add_argument('--probe', required=True)
parser.add_argument('--tables', required=True)
parser.add_argument('--epd', required=True)
parser.add_argument('--report', required=True)
a = parser.parse_args()
rng = random.Random(20260914)
counts = collections.Counter()
by_count = collections.defaultdict(list)
invalid = 0
for line in Path(a.epd).read_text().splitlines():
    if not line.strip(): continue
    fields = line.split()
    fen = ' '.join(fields[:6]) if len(fields) >= 6 and fields[4].isdigit() and fields[5].isdigit() else ' '.join(fields[:4]) + ' 0 1'
    b = chess.Board(fen)
    n = len(b.piece_map())
    counts[n] += 1
    if not b.is_valid(): invalid += 1
    if len(by_count[n]) < (100 if n <= 7 else 2): by_count[n].append(b)
print('Corpus:', sum(counts.values()), 'positions;', dict(sorted(counts.items())), flush=True)

positions = {}
def add(b):
    if b.is_valid() and len(b.piece_map()) <= 5 and not b.castling_rights:
        positions[b.fen(en_passant='fen')] = b.copy(stack=False)

# Actual legal continuations of supplied six-man EPDs, stopping on entry to TB.
for root in by_count[6]:
    frontier = [root]
    for depth in range(3):
        next_frontier = []
        for b in frontier:
            for m in b.legal_moves:
                child = b.copy(stack=False); child.push(m)
                if len(child.piece_map()) <= 5: add(child)
                elif depth < 2: next_frontier.append(child)
        frontier = next_frontier[:200]
derived = len(positions)
# Exercise promotions, en passant, terminal positions and near-rule50 endings.
for fen in [
    '8/8/8/8/8/2K5/3Q4/7k w - - 0 1',
    '8/8/8/8/8/2K5/3Q4/7k w - - 99 1',
    '8/8/8/8/8/2K5/3Q4/7k w - - 100 1',
    '8/8/8/8/8/2K5/3R4/7k b - - 0 1',
    '8/P7/8/8/8/8/2K5/7k w - - 0 1',
    '8/8/8/3pP3/8/8/2K5/7k w - d6 0 1',
    '7k/6Q1/5K2/8/8/8/8/8 b - - 0 1',
    '7k/5K2/6Q1/8/8/8/8/8 b - - 0 1',
    '8/8/8/8/8/2K5/8/7k w - - 0 1',
]: add(chess.Board(fen))
while len(positions) < derived + 600:
    b = chess.Board(None)
    squares = rng.sample(range(64), rng.randint(3, 5))
    b.set_piece_at(squares[0], chess.Piece(chess.KING, True))
    b.set_piece_at(squares[1], chess.Piece(chess.KING, False))
    for sq in squares[2:]:
        pt = rng.choice([chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN])
        b.set_piece_at(sq, chess.Piece(pt, bool(rng.getrandbits(1))))
    b.turn = bool(rng.getrandbits(1)); b.halfmove_clock = rng.choice([0, 0, 0, 90, 99])
    add(b)
boards = list(positions.values())
# Input-rejection coverage, also using actual out-of-range EPD samples.
boards += [chess.Board(), chess.Board('4k3/8/8/8/8/8/8/4K2R w K - 0 1')]
boards += [x for group in by_count.values() for x in group if len(x.piece_map()) > 5]
result = subprocess.run([a.probe, a.tables], input='\n'.join(b.fen(en_passant='fen') for b in boards)+'\n',
                        text=True, capture_output=True, check=True, timeout=180)
lines = [l for l in result.stdout.splitlines() if l.split() and l.split()[0] in ('-9','-2','-1','0','1','2')]
assert len(lines) == len(boards), (len(lines), len(boards), result.stdout[-500:])

failures = []
wdl_checked = root_checked = 0
root_data = {}
with chess.syzygy.open_tablebase(a.tables) as tb:
    for b, line in zip(boards, lines):
        fields = line.split(); wdl, root_wdl, used_dtz = map(int, fields[:3])
        moves = [chess.Move.from_uci(m) for m in fields[3:]]
        eligible = len(b.piece_map()) <= 5 and not b.castling_rights
        if not eligible:
            if wdl != -9 or root_wdl != -9: failures.append(('range/castling', b.fen(), line))
            continue
        if b.halfmove_clock == 0:
            expected = tb.probe_wdl(b)
            wdl_checked += 1
            if wdl != expected: failures.append(('wdl', b.fen(), wdl, expected))
        elif wdl != -9: failures.append(('clock guard', b.fen(), line))
        if b.is_game_over() or b.halfmove_clock >= 100: continue
        if root_wdl == -9 or not moves or not used_dtz:
            failures.append(('root failed', b.fen(), line)); continue
        root_checked += 1
        root_data[b.fen()] = (root_wdl, set(moves))
        # Each returned move must preserve the best independently probed WDL
        # with the actual remaining fifty-move budget.
        values = {}
        for m in b.legal_moves:
            child = b.copy(); child.push(m)
            if child.is_checkmate(): value = 2
            elif child.is_stalemate() or child.is_insufficient_material(): value = 0
            else:
                dtz = -tb.probe_dtz(child)
                if child.halfmove_clock == 0:
                    value = -tb.probe_wdl(child)
                elif dtz == 0: value = 0
                else:
                    distance = abs(dtz) + child.halfmove_clock
                    value = (2 if distance <= 100 else 1) * (1 if dtz > 0 else -1)
            values[m] = value
        best = max(values.values())
        if root_wdl != best: failures.append(('root outcome', b.fen(), root_wdl, best))
        for m in moves:
            if m not in values or values[m] != best:
                failures.append(('root move', b.fen(), m.uci(), values.get(m), best))
print('Oracle:', wdl_checked, 'WDL,', root_checked, 'root; failures:', len(failures), flush=True)

# A deliberately WDL-only three-man directory validates the DTZ fallback.
wdl_dir = Path(a.report).resolve().parent / 'syzygy-wdl-only'
wdl_dir.mkdir(exist_ok=True)
shutil.copyfile(Path(a.tables) / 'KQvK.rtbw', wdl_dir / 'KQvK.rtbw')
fallback_fens = ['8/8/8/8/8/2K5/3Q4/7k w - - 0 1',
                 '8/8/8/8/8/2K5/3Q4/7k w - - 90 1']
fallback = subprocess.run([a.probe, str(wdl_dir)], input='\n'.join(fallback_fens)+'\n',
                          text=True, capture_output=True, check=True).stdout.splitlines()
assert fallback[0].split()[:3] == ['2', '2', '0'], fallback
assert fallback[1].split()[:3] == ['-9', '-9', '0'], fallback

engine = chess.engine.SimpleEngine.popen_uci(a.engine, timeout=30)
engine.configure({'SyzygyPath': a.tables, 'Hash': 16, 'NumaPolicy':'none'})
search_checks = 0
sample_hits = 0
try:
    for threads in (1, 4):
        engine.configure({'Threads': threads})
        selected = [b for b in boards if b.fen() in root_data][:60]
        for b in selected:
            played = engine.play(b, chess.engine.Limit(nodes=5000), game=object(), info=chess.engine.INFO_ALL)
            info = played.info
            best, approved = root_data[b.fen()]
            if played.move not in approved:
                failures.append(('search move', threads, b.fen(), str(info)))
            if info.get('depth', 0) > 0 and info.get('tbhits', 0) < 1: failures.append(('missing tbhits', b.fen()))
            search_checks += 1
        # All original six-man samples plus deterministic examples of 7-23 men.
        samples = by_count[6] + [b for n, group in sorted(by_count.items()) if n > 6 for b in group[:2]]
        for b in samples:
            info = engine.analyse(b, chess.engine.Limit(nodes=10000), game=object())
            if not info.get('pv') or info['pv'][0] not in b.legal_moves:
                failures.append(('EPD search', threads, b.fen(), str(info)))
            sample_hits += info.get('tbhits', 0)
            search_checks += 1
        print('Search checks through', threads, 'threads:', search_checks, 'EPD tbhits:', sample_hits, flush=True)
    b = next(b for b in boards if b.fen() in root_data)
    played = engine.play(b, chess.engine.Limit(nodes=1), game=object())
    if played.move not in root_data[b.fen()][1]: failures.append(('early stop fallback', b.fen(), str(played)))
    engine.configure({'SyzygyProbeLimit': 0})
    info = engine.analyse(b, chess.engine.Limit(depth=2), game=object())
    if info.get('tbhits', 0) != 0: failures.append(('disabled probes', str(info)))
    engine.configure({'SyzygyProbeLimit': 7, 'SyzygyPath': '<empty>'})
    info = engine.analyse(b, chess.engine.Limit(depth=2), game=object())
    if info.get('tbhits', 0) != 0: failures.append(('unloaded probes', str(info)))
    engine.configure({'SyzygyPath': a.tables})
    info = engine.analyse(b, chess.engine.Limit(depth=2), game=object())
    if info.get('tbhits', 0) < 1: failures.append(('reload probes', str(info)))
    # Chess960 without castling rights uses the same tablebase position.
    frc = b.copy(); frc.chess960 = True
    played = engine.play(frc, chess.engine.Limit(depth=2), game=object())
    if played.move not in root_data[b.fen()][1]: failures.append(('Chess960 root', str(played)))
    rule_board = chess.Board('8/8/8/8/8/2K5/3R4/7k w - - 99 1')
    info = engine.analyse(rule_board, chess.engine.Limit(depth=2), game=object())
    if info['score'].relative.score() != 0: failures.append(('rule50 draw', str(info)))
    engine.configure({'Syzygy50MoveRule': False})
    info = engine.analyse(rule_board, chess.engine.Limit(depth=2), game=object())
    if info['score'].relative.score(mate_score=32000) <= 0: failures.append(('rule50 disabled win', str(info)))
    engine.configure({'Syzygy50MoveRule': True})
finally:
    engine.quit()
report = dict(corpus=a.epd, tables=a.tables, corpus_count=sum(counts.values()), invalid_corpus=invalid,
    material_histogram=dict(sorted(counts.items())), derived_five_man=derived,
    oracle_wdl=wdl_checked, oracle_roots=root_checked, search_checks=search_checks,
    epd_search_tbhits=sample_hits, failures=failures)
Path(a.report).write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
raise SystemExit(bool(failures))
