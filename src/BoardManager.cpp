#include "BoardManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

// ---------------------------------------------------------------------------
// Square helpers
// ---------------------------------------------------------------------------

std::string square_name(int tile) {
  if (!is_valid_tile(tile)) {
    return "--";
  }
  return {static_cast<char>('a' + get_tile_column(tile)),
          static_cast<char>('1' + get_tile_row(tile))};
}

int square_from_name(std::string_view name) {
  if (name.size() < 2) {
    return -1;
  }
  const char f{static_cast<char>(std::tolower(name[0]))};
  const char r{name[1]};
  if (f < 'a' || f > 'h' || r < '1' || r > '8') {
    return -1;
  }
  return (r - '1') * 8 + (f - 'a');
}

namespace {

char piece_letter(PieceType type) {
  switch (type) {
    case PieceType::King:
      return 'K';
    case PieceType::Queen:
      return 'Q';
    case PieceType::Rook:
      return 'R';
    case PieceType::Bishop:
      return 'B';
    case PieceType::Knight:
      return 'N';
    default:
      return '\0';
  }
}

PieceType promotion_from_char(char c) {
  switch (std::tolower(c)) {
    case 'q':
      return PieceType::Queen;
    case 'r':
      return PieceType::Rook;
    case 'b':
      return PieceType::Bishop;
    case 'n':
      return PieceType::Knight;
    default:
      return PieceType::None;
  }
}

void all_legal(Board& board, Moves& out) {
  out.size = 0;
  board.generate_all_legal_moves(out);
}

bool same_move(const Move& a, const Move& b) {
  return a.tile == b.tile && a.target == b.target && a.promotion == b.promotion;
}

bool is_castle(const Board& board, const Move& m) {
  return board.get_type(m.tile) == PieceType::King &&
         std::abs(m.target - m.tile) == 2;
}

bool is_capture(const Board& board, const Move& m) {
  if (!board.is_empty(m.target)) {
    return true;
  }
  // En passant: pawn changes file onto an empty square.
  return board.get_type(m.tile) == PieceType::Pawn &&
         get_tile_column(m.tile) != get_tile_column(m.target);
}

enum class Disambiguation : uint8_t { Minimal, None, File, Rank, Square };

// SAN without check suffix, with explicit control of disambiguation so the
// parser can accept over-specified input such as "Ngf3" or "R1e2".
std::string san_body(const Board& board, const Moves& legal, const Move& m,
                     Disambiguation mode) {
  if (is_castle(board, m)) {
    return m.target > m.tile ? "O-O" : "O-O-O";
  }
  const PieceType type{board.get_type(m.tile)};
  std::string s;
  const bool capture{is_capture(board, m)};

  if (type == PieceType::Pawn) {
    if (capture) {
      s += static_cast<char>('a' + get_tile_column(m.tile));
      s += 'x';
    }
    s += square_name(m.target);
    if (m.promotion != PieceType::None) {
      s += '=';
      s += piece_letter(m.promotion);
    }
    return s;
  }

  s += piece_letter(type);

  if (mode == Disambiguation::Minimal) {
    bool clash{};
    bool same_file{};
    bool same_rank{};
    for (int i = 0; i < legal.size; ++i) {
      const Move& o{legal.data[i]};
      if (o.tile == m.tile || o.target != m.target ||
          board.get_type(o.tile) != type) {
        continue;
      }
      clash = true;
      same_file |= get_tile_column(o.tile) == get_tile_column(m.tile);
      same_rank |= get_tile_row(o.tile) == get_tile_row(m.tile);
    }
    if (!clash) {
      mode = Disambiguation::None;
    } else if (!same_file) {
      mode = Disambiguation::File;
    } else if (!same_rank) {
      mode = Disambiguation::Rank;
    } else {
      mode = Disambiguation::Square;
    }
  }

  const std::string from{square_name(m.tile)};
  switch (mode) {
    case Disambiguation::File:
      s += from[0];
      break;
    case Disambiguation::Rank:
      s += from[1];
      break;
    case Disambiguation::Square:
      s += from;
      break;
    default:
      break;
  }
  if (capture) {
    s += 'x';
  }
  s += square_name(m.target);
  return s;
}

std::string check_suffix(Board& board, const Move& m) {
  board.make_move(m);
  std::string suffix;
  if (board.is_in_checkmate()) {
    suffix = "#";
  } else if (board.is_in_check()) {
    suffix = "+";
  }
  board.undo();
  return suffix;
}

std::string full_san(Board& board, const Move& m) {
  Moves legal;
  all_legal(board, legal);
  return san_body(board, legal, m, Disambiguation::Minimal) +
         check_suffix(board, m);
}

// Normalises user input and SAN for comparison: drops capture/check/annotation
// marks, '=' and '-', maps zeros to 'O' for castling.
std::string normalize(std::string_view in) {
  std::string out;
  for (const char c : in) {
    if (c == 'x' || c == 'X' || c == '+' || c == '#' || c == '=' ||
        c == '!' || c == '?' || c == '-' || c == ':' ||
        std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    out += (c == '0' || c == 'o') ? 'O' : c;
  }
  // Strip a trailing "e.p." annotation.
  if (out.size() > 4 && out.ends_with("e.p.")) {
    out.resize(out.size() - 4);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// CameraRig
// ---------------------------------------------------------------------------

void CameraRig::update(float dt) {
  // Frame-rate independent exponential smoothing (critically damped feel).
  const float k{1.0F - std::exp(-dt * 5.5F)};
  yaw += (target_yaw - yaw) * k;
  pitch += (target_pitch - pitch) * k;
  distance += (target_distance - distance) * k;
}

bool CameraRig::settled() const {
  return std::abs(target_yaw - yaw) < 0.25F &&
         std::abs(target_pitch - pitch) < 0.25F &&
         std::abs(target_distance - distance) < 0.1F;
}

// ---------------------------------------------------------------------------
// BoardManager
// ---------------------------------------------------------------------------

BoardManager::BoardManager() {
  // Establishing shot: low over the hall floor, slowly orbiting until the
  // user picks a side, then the camera rises into the playing view.
  camera_.yaw = camera_.target_yaw = -30.0F;
  camera_.pitch = camera_.target_pitch = k_establishing_pitch;
  camera_.distance = camera_.target_distance = 104.0F;
}

void BoardManager::update(float dt) {
  if (!user_side_) {
    camera_.target_yaw += dt * 4.0F;  // gentle idle orbit behind the prompt
  }
  camera_.update(dt);
}

void BoardManager::clear_side() {
  user_side_.reset();
  camera_.target_pitch = k_establishing_pitch;
  camera_.target_distance = 104.0F;
}

void BoardManager::select_side(PieceColor side) {
  user_side_ = side;
  reset_camera();
}

void BoardManager::reset_camera() {
  const float side_yaw{yaw_for_side(user_side())};
  // Pick the equivalent target angle nearest to the current yaw so the flip
  // takes the short way round, and always lands on exactly 0° / 180° mod 360.
  const float turns{std::round((camera_.yaw - side_yaw) / 360.0F)};
  camera_.target_yaw = side_yaw + turns * 360.0F;
  camera_.target_pitch = k_default_pitch;
  camera_.target_distance = k_default_distance;
}

void BoardManager::orbit_camera(float d_yaw, float d_pitch) {
  camera_.target_yaw += d_yaw;
  camera_.yaw += d_yaw;
  camera_.target_pitch = std::clamp(camera_.target_pitch + d_pitch, 12.0F, 88.0F);
  camera_.pitch = std::clamp(camera_.pitch + d_pitch, 12.0F, 88.0F);
}

void BoardManager::zoom_camera(float d_distance) {
  camera_.target_distance =
      std::clamp(camera_.target_distance + d_distance, 50.0F, 140.0F);
}

float BoardManager::camera_yaw_normalized() const {
  float y{std::fmod(camera_.yaw, 360.0F)};
  if (y < 0.0F) {
    y += 360.0F;
  }
  return y >= 359.995F ? 0.0F : y;  // avoid reporting 360.0 for ~0
}

void BoardManager::legal_moves(Moves& out) { all_legal(board_, out); }

std::string BoardManager::to_lan(const Move& move) {
  std::string s{square_name(move.tile) + square_name(move.target)};
  if (move.promotion != PieceType::None) {
    s += static_cast<char>(std::tolower(piece_letter(move.promotion)));
  }
  return s;
}

std::optional<Move> BoardManager::from_lan(std::string_view lan) {
  if (lan.size() < 4 || lan.size() > 5) {
    return std::nullopt;
  }
  const int from{square_from_name(lan.substr(0, 2))};
  const int to{square_from_name(lan.substr(2, 2))};
  if (from < 0 || to < 0) {
    return std::nullopt;
  }
  PieceType promo{};
  if (lan.size() == 5) {
    promo = promotion_from_char(lan[4]);
    if (promo == PieceType::None) {
      return std::nullopt;
    }
  }
  Moves legal;
  legal_moves(legal);
  for (int i = 0; i < legal.size; ++i) {
    const Move& m{legal.data[i]};
    if (m.tile == from && m.target == to && m.promotion == promo) {
      return m;
    }
  }
  return std::nullopt;
}

std::string BoardManager::to_san(const Move& move) {
  return full_san(board_, move);
}

ParseResult BoardManager::parse_move(std::string_view raw) {
  ParseResult result;
  std::string input{raw};
  // Trim and drop a leading move number ("12." / "12...").
  input.erase(0, input.find_first_not_of(" \t"));
  input.erase(input.find_last_not_of(" \t\r\n") + 1);
  if (const auto dot = input.find_last_of('.');
      dot != std::string::npos && dot + 1 < input.size() &&
      std::all_of(input.begin(), input.begin() + static_cast<long>(dot),
                  [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; })) {
    input = input.substr(dot + 1);
    input.erase(0, input.find_first_not_of(' '));
  }
  if (input.empty()) {
    result.error = "Type a move, e.g. d4, Nf3, exd5 or O-O";
    return result;
  }
  if (is_game_over()) {
    result.error = "The game is over (" + status_text() + ")";
    return result;
  }

  auto accept = [&](const Move& m) {
    result.ok = true;
    result.move = m;
    result.san = to_san(m);
    result.lan = to_lan(m);
    return result;
  };

  // 1) UCI / long algebraic: e2e4, g1f3, e7e8q (also "e2-e4").
  {
    std::string lan;
    for (const char c : input) {
      if (c != '-' && c != 'x') {
        lan += static_cast<char>(std::tolower(c));
      }
    }
    if (lan.size() >= 4 && square_from_name(lan.substr(0, 2)) >= 0 &&
        square_from_name(lan.substr(2, 2)) >= 0) {
      if (auto m = from_lan(lan)) {
        return accept(*m);
      }
    }
  }

  Moves legal;
  legal_moves(legal);

  const auto try_match = [&](const std::string& norm,
                             std::vector<Move>& exact,
                             std::vector<Move>& loose) {
    for (int i = 0; i < legal.size; ++i) {
      const Move& m{legal.data[i]};
      bool hit{};
      for (const auto mode : {Disambiguation::Minimal, Disambiguation::File,
                              Disambiguation::Rank, Disambiguation::Square}) {
        if (normalize(san_body(board_, legal, m, mode)) == norm) {
          hit = true;
          break;
        }
      }
      // "e8" with no piece letter promotes to a queen by convention.
      if (!hit && m.promotion == PieceType::Queen) {
        std::string body{san_body(board_, legal, m, Disambiguation::Minimal)};
        body.resize(body.find('='));
        hit = normalize(body) == norm;
      }
      if (hit) {
        exact.push_back(m);
        continue;
      }
      if (normalize(san_body(board_, legal, m, Disambiguation::None)) == norm) {
        loose.push_back(m);  // matches only without disambiguation -> ambiguous
      }
    }
  };

  std::vector<std::string> variants{normalize(input)};
  // Friendly fallbacks for lowercase piece letters ("nf3", "Bxc4" vs "bxc4").
  if (const char c0 = input[0]; std::string_view{"nrqk"}.find(c0) != std::string_view::npos) {
    std::string up{input};
    up[0] = static_cast<char>(std::toupper(c0));
    variants.push_back(normalize(up));
  } else if (c0 == 'b' && input.size() >= 3) {
    std::string up{input};
    up[0] = 'B';
    variants.push_back(normalize(up));
  }

  std::vector<Move> ambiguous;
  for (const auto& norm : variants) {
    std::vector<Move> exact;
    std::vector<Move> loose;
    try_match(norm, exact, loose);
    if (exact.size() == 1) {
      return accept(exact.front());
    }
    if (exact.size() > 1) {
      ambiguous = exact;
      break;
    }
    if (loose.size() > 1 && ambiguous.empty()) {
      ambiguous = loose;
    }
  }

  if (!ambiguous.empty()) {
    result.error = "Ambiguous move '" + input + "': could be ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i > 0) {
        result.error += i + 1 == ambiguous.size() ? " or " : ", ";
      }
      result.error += to_san(ambiguous[i]);
    }
    return result;
  }

  result.error = "Illegal move '" + input + "' for " +
                 (side_to_move() == PieceColor::White ? "White" : "Black");
  if (board_.is_in_check()) {
    result.error += " (you are in check)";
  }
  return result;
}

ParseResult BoardManager::play(std::string_view input) {
  ParseResult r{parse_move(input)};
  if (r.ok) {
    play_move(r.move);
  }
  return r;
}

void BoardManager::play_move(const Move& move) {
  HistoryEntry entry;
  entry.san = to_san(move);
  entry.lan = to_lan(move);
  entry.color = board_.get_turn();
  entry.move = move;

  MoveEvent ev;
  ev.from = move.tile;
  ev.to = move.target;
  ev.piece = board_.get_tile(move.tile);
  ev.promotion = move.promotion;
  if (!board_.is_empty(move.target)) {
    ev.captured_tile = move.target;
    ev.captured = board_.get_tile(move.target);
  } else if (is_capture(board_, move)) {  // en passant
    ev.captured_tile =
        move.target + (board_.get_turn() == PieceColor::White ? -8 : 8);
    ev.captured = board_.get_tile(ev.captured_tile);
  }
  if (is_castle(board_, move)) {
    ev.rook_from = move.tile + (move.target > move.tile ? 3 : -4);
    ev.rook_to = (move.tile + move.target) / 2;
  }

  board_.make_move(move);
  history_.push_back(std::move(entry));
  events_.push_back(ev);
  ++position_id_;
}

bool BoardManager::undo() {
  if (history_.empty()) {
    return false;
  }
  const HistoryEntry last{history_.back()};
  history_.pop_back();
  board_.undo();
  MoveEvent ev;
  ev.from = last.move.target;
  ev.to = last.move.tile;
  ev.piece = board_.get_tile(last.move.tile);
  ev.is_undo = true;
  events_.push_back(ev);
  ++position_id_;
  return true;
}

void BoardManager::reset() {
  board_.load_fen();
  history_.clear();
  events_.clear();
  ++position_id_;
}

std::vector<std::string> BoardManager::lan_line_to_san(
    const std::vector<std::string>& lan_moves, size_t max_moves) const {
  Board scratch{board_};
  std::vector<std::string> out;
  for (const auto& lan : lan_moves) {
    if (out.size() >= max_moves || lan.size() < 4) {
      break;
    }
    const int from{square_from_name(lan.substr(0, 2))};
    const int to{square_from_name(lan.substr(2, 2))};
    const PieceType promo{lan.size() == 5 ? promotion_from_char(lan[4])
                                          : PieceType::None};
    Moves legal;
    all_legal(scratch, legal);
    const Move* found{};
    for (int i = 0; i < legal.size; ++i) {
      if (same_move(legal.data[i], {from, to, promo})) {
        found = &legal.data[i];
        break;
      }
    }
    if (found == nullptr) {
      break;  // stale PV for an older position
    }
    const Move m{*found};
    out.push_back(full_san(scratch, m));
    scratch.make_move(m);
  }
  return out;
}

std::vector<HistoryTurn> BoardManager::history_turns() const {
  std::vector<HistoryTurn> turns;
  for (size_t i = 0; i < history_.size(); ++i) {
    if (history_[i].color == PieceColor::White || turns.empty()) {
      turns.push_back({static_cast<int>(turns.size()) + 1, {}, {}});
    }
    if (history_[i].color == PieceColor::White) {
      turns.back().white = history_[i].san;
    } else {
      turns.back().black = history_[i].san;
    }
  }
  return turns;
}

std::string BoardManager::history_text() const {
  std::ostringstream out;
  bool first{true};
  for (const auto& t : history_turns()) {
    if (!first) {
      out << ' ';
    }
    first = false;
    out << t.number << ". " << (t.white.empty() ? "..." : t.white);
    if (!t.black.empty()) {
      out << ' ' << t.black;
    }
  }
  return out.str();
}

std::vector<std::string> BoardManager::lan_moves() const {
  std::vector<std::string> out;
  out.reserve(history_.size());
  for (const auto& h : history_) {
    out.push_back(h.lan);
  }
  return out;
}

std::string BoardManager::status_text() const {
  if (board_.is_in_checkmate()) {
    return board_.get_turn() == PieceColor::White ? "Checkmate - Black wins"
                                                  : "Checkmate - White wins";
  }
  if (board_.is_in_draw()) {
    return "Stalemate";
  }
  if (board_.is_in_check()) {
    return "Check";
  }
  return board_.get_turn() == PieceColor::White ? "White to move"
                                                : "Black to move";
}

std::optional<MoveEvent> BoardManager::pop_event() {
  if (events_.empty()) {
    return std::nullopt;
  }
  MoveEvent ev{events_.front()};
  events_.pop_front();
  return ev;
}
