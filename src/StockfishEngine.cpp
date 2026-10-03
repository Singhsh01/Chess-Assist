#include "StockfishEngine.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "log.hpp"

using namespace std::chrono_literals;

StockfishEngine::~StockfishEngine() { shutdown(); }

void StockfishEngine::start(EngineOptions options) {
  shutdown();
  options_ = std::move(options);
  {
    const std::lock_guard lock{mutex_};
    stop_requested_ = false;
    analysis_ = {};
    analysis_.status = EngineAnalysis::Status::Starting;
  }
  worker_ = std::thread{&StockfishEngine::worker_main, this};
}

void StockfishEngine::shutdown() {
  {
    const std::lock_guard lock{mutex_};
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void StockfishEngine::analyze(std::vector<std::string> uci_moves,
                              uint64_t position_id) {
  {
    const std::lock_guard lock{mutex_};
    pending_ = Request{std::move(uci_moves), position_id};
  }
  cv_.notify_all();
}

EngineAnalysis StockfishEngine::snapshot() const {
  const std::lock_guard lock{mutex_};
  return analysis_;
}

std::string StockfishEngine::locate_executable(const std::string& hint) {
  namespace fs = std::filesystem;
  std::vector<std::string> candidates;
  if (!hint.empty()) {
    candidates.push_back(hint);
  }
  if (const char* env = std::getenv("STOCKFISH_PATH"); env != nullptr) {
    candidates.emplace_back(env);
  }
#ifdef _WIN32
  candidates.emplace_back("stockfish.exe");
  candidates.emplace_back("C:/Program Files/Stockfish/stockfish.exe");
#else
  candidates.emplace_back("/opt/homebrew/bin/stockfish");  // macOS arm64
  candidates.emplace_back("/usr/local/bin/stockfish");     // macOS x86_64
  candidates.emplace_back("/usr/games/stockfish");         // Debian/Ubuntu
  candidates.emplace_back("/usr/bin/stockfish");
  candidates.emplace_back("./stockfish");
#endif
  for (const auto& c : candidates) {
    std::error_code ec;
    if (fs::exists(c, ec) && !fs::is_directory(c, ec)) {
      return c;
    }
  }
  return "stockfish";  // let posix_spawnp search PATH
}

bool StockfishEngine::parse_info_line(const std::string& line,
                                      bool white_to_move,
                                      EngineAnalysis& out) {
  std::istringstream in{line};
  std::string token;
  in >> token;
  if (token != "info") {
    return false;
  }
  bool got_score{};
  bool bound{};
  int depth{-1};
  bool is_mate{};
  int value{};
  uint64_t nodes{};
  uint64_t nps{};
  int multipv{1};
  std::vector<std::string> pv;
  while (in >> token) {
    if (token == "string") {
      return false;  // free-form engine message
    }
    if (token == "depth") {
      in >> depth;
    } else if (token == "multipv") {
      in >> multipv;
    } else if (token == "nodes") {
      in >> nodes;
    } else if (token == "nps") {
      in >> nps;
    } else if (token == "score") {
      std::string kind;
      in >> kind >> value;
      is_mate = kind == "mate";
      got_score = kind == "mate" || kind == "cp";
    } else if (token == "lowerbound" || token == "upperbound") {
      bound = true;
    } else if (token == "pv") {
      while (in >> token) {
        pv.push_back(token);
      }
    }
  }
  // Only accept the primary line, with a score and a PV; aspiration-window
  // bound updates are noisy, so we skip them for a steadier HUD.
  if (!got_score || pv.empty() || multipv != 1 || bound) {
    return false;
  }
  const int sign{white_to_move ? 1 : -1};
  out.has_score = true;
  out.is_mate = is_mate;
  if (is_mate) {
    out.mate_in = sign * value;
    out.score_cp = 0;
  } else {
    out.score_cp = sign * value;
    out.mate_in = 0;
  }
  if (depth >= 0) {
    out.depth = depth;
  }
  out.nodes = nodes;
  out.nps = nps;
  out.pv = std::move(pv);
  out.best_move = out.pv.front();
  return true;
}

void StockfishEngine::set_status(EngineAnalysis::Status status,
                                 const std::string& error) {
  const std::lock_guard lock{mutex_};
  analysis_.status = status;
  if (!error.empty()) {
    analysis_.error = error;
  }
}

bool StockfishEngine::handshake() {
  pipe_.write_line("uci");
  std::string name;
  const auto deadline{std::chrono::steady_clock::now() + 5s};
  bool ok{};
  while (std::chrono::steady_clock::now() < deadline) {
    auto line{pipe_.read_line(100ms)};
    if (!line) {
      if (!pipe_.alive()) {
        break;
      }
      continue;
    }
    if (line->rfind("id name ", 0) == 0) {
      name = line->substr(8);
    } else if (*line == "uciok") {
      ok = true;
      break;
    }
  }
  if (!ok) {
    return false;
  }

  int threads{options_.threads};
  if (threads <= 0) {
    threads = static_cast<int>(
        std::clamp(std::thread::hardware_concurrency() / 2U, 1U, 4U));
  }
  pipe_.write_line("setoption name Threads value " + std::to_string(threads));
  pipe_.write_line("setoption name Hash value " +
                   std::to_string(options_.hash_mb));
  pipe_.write_line("ucinewgame");
  pipe_.write_line("isready");
  while (std::chrono::steady_clock::now() < deadline + 5s) {
    auto line{pipe_.read_line(100ms)};
    if (line && *line == "readyok") {
      const std::lock_guard lock{mutex_};
      analysis_.engine_name = name;
      LOGF("ENGINE", "{} ready ({} threads)", name, threads);
      return true;
    }
    if (!line && !pipe_.alive()) {
      return false;
    }
  }
  return false;
}

void StockfishEngine::worker_main() {
  const std::string exe{locate_executable(options_.executable)};
  if (!pipe_.spawn(exe)) {
    set_status(EngineAnalysis::Status::Error, pipe_.error());
    LOG("ENGINE", pipe_.error());
  } else if (!handshake()) {
    const std::string msg{"Stockfish not found or did not answer UCI (" + exe +
                          "). Install it or pass --stockfish <path>."};
    set_status(EngineAnalysis::Status::Error, msg);
    LOG("ENGINE", msg);
    pipe_.terminate();
  } else {
    set_status(EngineAnalysis::Status::Idle);
  }

  while (true) {
    Request request;
    {
      std::unique_lock lock{mutex_};
      cv_.wait(lock, [this] { return stop_requested_ || pending_.has_value(); });
      if (stop_requested_) {
        break;
      }
      request = std::move(*pending_);
      pending_.reset();
      if (analysis_.status == EngineAnalysis::Status::Error) {
        // Keep the id moving so the HUD knows the request was seen.
        analysis_.position_id = request.id;
        continue;
      }
    }
    run_search(request);
  }

  if (pipe_.alive()) {
    pipe_.write_line("stop");
    pipe_.write_line("quit");
  }
  pipe_.terminate();
}

void StockfishEngine::run_search(const Request& request) {
  const bool white_to_move{request.moves.size() % 2 == 0};

  std::string position{"position startpos"};
  if (!request.moves.empty()) {
    position += " moves";
    for (const auto& m : request.moves) {
      position += " " + m;
    }
  }

  {
    const std::lock_guard lock{mutex_};
    analysis_.status = EngineAnalysis::Status::Searching;
    analysis_.position_id = request.id;
    analysis_.complete = false;
    analysis_.has_score = false;
    analysis_.depth = 0;
    analysis_.best_move.clear();
    analysis_.pv.clear();
  }

  pipe_.write_line(position);
  pipe_.write_line("go movetime " + std::to_string(options_.movetime_ms));

  bool stop_sent{};
  const auto hard_deadline{std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(options_.movetime_ms) +
                           10s};
  while (true) {
    // Abort early if a newer position arrived or the app is closing.
    if (!stop_sent) {
      const std::lock_guard lock{mutex_};
      if (pending_.has_value() || stop_requested_) {
        stop_sent = true;
      }
    }
    if (stop_sent) {
      pipe_.write_line("stop");
      // Drain until bestmove so the next search starts clean.
      while (auto line = pipe_.read_line(2s)) {
        if (line->rfind("bestmove", 0) == 0) {
          break;
        }
      }
      set_status(EngineAnalysis::Status::Idle);
      return;
    }

    auto line{pipe_.read_line(15ms)};
    if (!line) {
      if (!pipe_.alive() ||
          std::chrono::steady_clock::now() > hard_deadline) {
        set_status(EngineAnalysis::Status::Error,
                   "Stockfish stopped responding");
        pipe_.terminate();
        return;
      }
      continue;
    }

    if (line->rfind("info", 0) == 0) {
      EngineAnalysis parsed;
      if (parse_info_line(*line, white_to_move, parsed)) {
        const std::lock_guard lock{mutex_};
        analysis_.has_score = true;
        analysis_.is_mate = parsed.is_mate;
        analysis_.score_cp = parsed.score_cp;
        analysis_.mate_in = parsed.mate_in;
        analysis_.depth = parsed.depth;
        analysis_.nodes = parsed.nodes;
        analysis_.nps = parsed.nps;
        analysis_.pv = std::move(parsed.pv);
        analysis_.best_move = parsed.best_move;
      }
    } else if (line->rfind("bestmove", 0) == 0) {
      std::istringstream in{*line};
      std::string token;
      std::string best;
      in >> token >> best;
      const std::lock_guard lock{mutex_};
      if (best != "(none)" && !best.empty()) {
        analysis_.best_move = best;
        if (analysis_.pv.empty() || analysis_.pv.front() != best) {
          analysis_.pv = {best};
        }
      } else {
        analysis_.best_move.clear();  // checkmate / stalemate on board
        analysis_.pv.clear();
      }
      analysis_.complete = true;
      analysis_.status = EngineAnalysis::Status::Idle;
      return;
    }
  }
}
