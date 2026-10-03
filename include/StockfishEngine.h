#pragma once
// StockfishEngine.h — asynchronous UCI bridge to a Stockfish subprocess.
//
// All pipe I/O happens on a dedicated worker thread, so the render loop never
// blocks on the engine. The main thread posts the current move list with
// `analyze()` (non-blocking, newest request wins) and polls `snapshot()` once
// per frame. Intermediate `info` lines are published while the engine is still
// searching, so the HUD evaluation updates live as depth increases.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ProcessPipe.h"

struct EngineAnalysis {
  enum class Status : uint8_t { Offline, Starting, Idle, Searching, Error };

  Status status{Status::Offline};
  std::string engine_name;
  std::string error;

  uint64_t position_id{};  // echoes the id passed to analyze()
  bool complete{};         // engine sent `bestmove` for this position
  int depth{};
  uint64_t nodes{};
  uint64_t nps{};

  // Scores are normalised to WHITE's point of view (positive = White better).
  bool has_score{};
  bool is_mate{};
  int score_cp{};  // centipawns, valid when !is_mate
  int mate_in{};   // moves to mate, >0 White mates, <0 Black mates

  std::string best_move;           // UCI long algebraic, e.g. "g1f3"
  std::vector<std::string> pv;     // principal variation (UCI)
};

struct EngineOptions {
  std::string executable;  // empty = auto-detect
  int threads{0};          // 0 = auto
  int hash_mb{64};
  int movetime_ms{1500};
};

class StockfishEngine {
 public:
  StockfishEngine() = default;
  ~StockfishEngine();

  StockfishEngine(const StockfishEngine&) = delete;
  StockfishEngine& operator=(const StockfishEngine&) = delete;
  StockfishEngine(StockfishEngine&&) = delete;
  StockfishEngine& operator=(StockfishEngine&&) = delete;

  // Starts the worker thread (which spawns and handshakes with the engine).
  void start(EngineOptions options);
  void shutdown();

  // Queue analysis of `startpos` + `uci_moves`. Supersedes any pending or
  // running search (the running one is stopped with the UCI `stop` command).
  void analyze(std::vector<std::string> uci_moves, uint64_t position_id);

  [[nodiscard]] EngineAnalysis snapshot() const;

  // Locates a Stockfish binary: explicit path, $STOCKFISH_PATH, common install
  // locations (Homebrew, apt), then plain "stockfish" on PATH.
  static std::string locate_executable(const std::string& hint);

  // Parses one UCI `info` line into `out` (exposed for unit tests).
  // `white_to_move` converts side-to-move scores to White's perspective.
  static bool parse_info_line(const std::string& line, bool white_to_move,
                              EngineAnalysis& out);

 private:
  struct Request {
    std::vector<std::string> moves;
    uint64_t id{};
  };

  void worker_main();
  bool handshake();
  void run_search(const Request& request);
  void set_status(EngineAnalysis::Status status, const std::string& error = {});

  EngineOptions options_;
  ProcessPipe pipe_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<Request> pending_;
  EngineAnalysis analysis_;
  bool stop_requested_{};

  std::thread worker_;
};
