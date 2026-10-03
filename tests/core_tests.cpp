// core_tests.cpp — dependency-free unit tests for the non-graphical modules:
// SAN <-> LAN conversion, move history, camera flip, win probability and UCI
// `info` parsing. Run with `ctest` or ./bin/chess-assist-tests.

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include "AssistantMath.h"
#include "BoardManager.h"
#include "StockfishEngine.h"

namespace {
int g_failed{};
int g_passed{};

#define EXPECT(cond)                                                   \
  do {                                                                 \
    if (cond) {                                                        \
      ++g_passed;                                                      \
    } else {                                                           \
      ++g_failed;                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

#define EXPECT_EQ(a, b)                                                       \
  do {                                                                        \
    const auto va = (a);                                                      \
    const auto vb = (b);                                                      \
    if (va == vb) {                                                           \
      ++g_passed;                                                             \
    } else {                                                                  \
      ++g_failed;                                                             \
      std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);      \
    }                                                                         \
  } while (0)

std::string lan_of(BoardManager& bm, const std::string& san) {
  const ParseResult r{bm.parse_move(san)};
  return r.ok ? r.lan : "ERR:" + r.error;
}

void test_basic_san_to_lan() {
  BoardManager bm;
  EXPECT_EQ(lan_of(bm, "d4"), std::string{"d2d4"});
  EXPECT_EQ(lan_of(bm, "Nf3"), std::string{"g1f3"});
  EXPECT_EQ(lan_of(bm, "nf3"), std::string{"g1f3"});      // lowercase piece
  EXPECT_EQ(lan_of(bm, "e2e4"), std::string{"e2e4"});     // UCI input
  EXPECT_EQ(lan_of(bm, "1. e4"), std::string{"e2e4"});    // move number
  EXPECT(!bm.parse_move("e5").ok);                        // illegal for White
  EXPECT(!bm.parse_move("Nf6").ok);
  EXPECT(!bm.parse_move("").ok);
}

void test_spec_sequence_and_history() {
  BoardManager bm;
  EXPECT(bm.play("d4").ok);
  EXPECT(bm.play("e6").ok);
  EXPECT_EQ(bm.history_text(), std::string{"1. d4 e6"});
  EXPECT(bm.play("c4").ok);
  EXPECT_EQ(bm.history_text(), std::string{"1. d4 e6 2. c4"});
  const auto lans{bm.lan_moves()};
  EXPECT_EQ(lans.size(), size_t{3});
  EXPECT_EQ(lans[0], std::string{"d2d4"});
  EXPECT_EQ(lans[1], std::string{"e7e6"});
  EXPECT_EQ(lans[2], std::string{"c2c4"});
  EXPECT(bm.side_to_move() == PieceColor::Black);
  EXPECT(bm.undo());
  EXPECT_EQ(bm.history_text(), std::string{"1. d4 e6"});
}

void test_castling_and_check() {
  BoardManager bm;
  for (const char* m : {"e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5"}) {
    EXPECT(bm.play(m).ok);
  }
  EXPECT_EQ(lan_of(bm, "O-O"), std::string{"e1g1"});
  EXPECT_EQ(lan_of(bm, "0-0"), std::string{"e1g1"});
  const ParseResult r{bm.play("O-O")};
  EXPECT_EQ(r.san, std::string{"O-O"});
  // Scholar's-mate style check detection: Bxf7+ is check.
  BoardManager b2;
  for (const char* m : {"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6"}) {
    EXPECT(b2.play(m).ok);
  }
  const ParseResult mate{b2.play("Qxf7")};
  EXPECT(mate.ok);
  EXPECT_EQ(mate.san, std::string{"Qxf7#"});
  EXPECT(b2.is_game_over());
  EXPECT(!b2.parse_move("a6").ok);
}

void test_disambiguation() {
  BoardManager bm;
  // Knights on b1 and f3 can both reach d2 after d4/Nf3 setup.
  for (const char* m : {"Nf3", "a6", "d4", "a5"}) {
    EXPECT(bm.play(m).ok);
  }
  const ParseResult amb{bm.parse_move("Nd2")};
  EXPECT(!amb.ok);
  EXPECT(amb.error.find("Nbd2") != std::string::npos);
  EXPECT_EQ(lan_of(bm, "Nbd2"), std::string{"b1d2"});
  EXPECT_EQ(lan_of(bm, "Nfd2"), std::string{"f3d2"});
  EXPECT_EQ(lan_of(bm, "Nf3d2"), std::string{"f3d2"});  // square-disambiguated
  // Over-specified but unambiguous input is accepted.
  EXPECT_EQ(lan_of(bm, "Ngh4"), std::string{"ERR:Illegal move 'Ngh4' for White"});
  EXPECT_EQ(lan_of(bm, "Nfh4"), std::string{"f3h4"});
}

void test_en_passant_and_promotion() {
  BoardManager bm;
  for (const char* m : {"e4", "a6", "e5", "d5"}) {
    EXPECT(bm.play(m).ok);
  }
  const ParseResult ep{bm.play("exd6")};
  EXPECT(ep.ok);
  EXPECT_EQ(ep.lan, std::string{"e5d6"});
  EXPECT(bm.board().is_empty(square_from_name("d5")));

  BoardManager pr;
  for (const char* m : {"h4", "g5", "hxg5", "Nf6", "gxf6", "Rg8", "fxe7", "Rg7"}) {
    EXPECT(pr.play(m).ok);
  }
  EXPECT_EQ(lan_of(pr, "exd8=Q+"), std::string{"e7d8q"});
  EXPECT_EQ(lan_of(pr, "exd8N"), std::string{"e7d8n"});
  EXPECT_EQ(lan_of(pr, "exf8=R"), std::string{"e7f8r"});
  const ParseResult q{pr.play("exd8")};  // bare promotion defaults to queen
  EXPECT(q.ok);
  EXPECT_EQ(q.san, std::string{"exd8=Q+"});
}

void test_pv_to_san() {
  BoardManager bm;
  const auto san{bm.lan_line_to_san({"e2e4", "e7e5", "g1f3", "b8c6", "f1b5"})};
  EXPECT_EQ(san.size(), size_t{5});
  EXPECT_EQ(san[2], std::string{"Nf3"});
  EXPECT_EQ(san[4], std::string{"Bb5"});
  // Stale PV stops at the first illegal move.
  EXPECT_EQ(bm.lan_line_to_san({"e2e4", "e2e4"}).size(), size_t{1});
}

void test_camera_flip() {
  BoardManager bm;
  bm.select_side(PieceColor::Black);
  for (int i = 0; i < 600; ++i) {
    bm.update(1.0F / 60.0F);
  }
  EXPECT(std::abs(bm.camera_yaw_normalized() - 180.0F) < 0.5F);
  bm.select_side(PieceColor::White);
  for (int i = 0; i < 600; ++i) {
    bm.update(1.0F / 60.0F);
  }
  const float y{bm.camera_yaw_normalized()};
  EXPECT(y < 0.5F || y > 359.5F);
}

void test_win_probability() {
  using assistant::win_probability;
  EXPECT(std::abs(win_probability(0) - 0.5) < 1e-9);
  // eval in pawns: +1.00 -> 1 / (1 + 10^-0.25) = 0.640
  EXPECT(std::abs(win_probability(100) - 0.64006) < 1e-4);
  EXPECT(std::abs(win_probability(-100) - 0.35994) < 1e-4);
  EXPECT(win_probability(2000) > 0.99);
  EXPECT_EQ(assistant::format_eval(34, false, 0), std::string{"+0.34"});
  EXPECT_EQ(assistant::format_eval(-150, false, 0), std::string{"-1.50"});
  EXPECT_EQ(assistant::format_eval(0, true, 3), std::string{"#3"});
  EXPECT_EQ(assistant::format_eval(0, true, -2), std::string{"#-2"});
  // Perspective flip: +1.00 for White is 36% for a Black user.
  EXPECT(std::abs(assistant::user_win_probability(100, false, 0, PieceColor::Black) - 0.35994) < 1e-4);
  EXPECT(assistant::user_win_probability(0, true, 2, PieceColor::White) > 0.999);
}

void test_info_parsing() {
  EngineAnalysis a;
  EXPECT(StockfishEngine::parse_info_line(
      "info depth 18 seldepth 25 multipv 1 score cp 31 nodes 123456 nps 999 "
      "hashfull 10 tbhits 0 time 50 pv d2d4 g8f6 c2c4 e7e6",
      true, a));
  EXPECT_EQ(a.depth, 18);
  EXPECT_EQ(a.score_cp, 31);
  EXPECT_EQ(a.best_move, std::string{"d2d4"});
  EXPECT_EQ(a.pv.size(), size_t{4});
  // Black to move: scores are flipped to White's view.
  EngineAnalysis b;
  EXPECT(StockfishEngine::parse_info_line(
      "info depth 9 score mate 2 nodes 10 pv d8h4 g2g3 h4g3", false, b));
  EXPECT(b.is_mate);
  EXPECT_EQ(b.mate_in, -2);
  EngineAnalysis c;
  EXPECT(!StockfishEngine::parse_info_line("info string NNUE enabled", true, c));
  EXPECT(!StockfishEngine::parse_info_line(
      "info depth 5 score cp 20 lowerbound pv e2e4", true, c));
}

// Talks to a real Stockfish if one is installed; skipped otherwise.
void test_engine_live() {
  const std::string exe{StockfishEngine::locate_executable("")};
  if (exe == "stockfish") {
    std::printf("skip test_engine_live (no stockfish binary found)\n");
    return;
  }
  StockfishEngine engine;
  engine.start({.executable = exe, .threads = 1, .hash_mb = 16, .movetime_ms = 300});
  BoardManager bm;
  bm.play("d4");
  bm.play("e6");
  engine.analyze(bm.lan_moves(), bm.position_id());
  EngineAnalysis a;
  for (int i = 0; i < 300; ++i) {  // up to 6 s
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    a = engine.snapshot();
    if (a.complete && a.position_id == bm.position_id()) {
      break;
    }
  }
  EXPECT(a.complete);
  EXPECT(a.has_score);
  EXPECT(!a.engine_name.empty());
  EXPECT(bm.from_lan(a.best_move).has_value());  // legal for White
  EXPECT(a.depth > 0);
  std::printf("engine: %s  best=%s  eval=%s  depth=%d\n", a.engine_name.c_str(),
              a.best_move.c_str(),
              assistant::format_eval(a.score_cp, a.is_mate, a.mate_in).c_str(),
              a.depth);
  // A newer request must supersede a running search.
  engine.analyze({"e2e4"}, 100);
  engine.analyze({"e2e4", "c7c5"}, 101);
  for (int i = 0; i < 300; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    a = engine.snapshot();
    if (a.complete && a.position_id == 101) {
      break;
    }
  }
  EXPECT_EQ(a.position_id, uint64_t{101});
  EXPECT(a.complete);
  engine.shutdown();
}

void test_perft() {
  Board b;
  EXPECT_EQ(b.perft(3), uint64_t{8902});
}
}  // namespace

int main() {
  test_basic_san_to_lan();
  test_spec_sequence_and_history();
  test_castling_and_check();
  test_disambiguation();
  test_en_passant_and_promotion();
  test_pv_to_san();
  test_camera_flip();
  test_win_probability();
  test_info_parsing();
  test_perft();
  test_engine_live();
  std::printf("%d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
