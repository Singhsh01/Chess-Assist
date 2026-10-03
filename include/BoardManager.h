#pragma once
// BoardManager.h — game state, side selection, camera flipping and SAN parsing.
//
// Wraps the rules engine (Board) with everything the assistant needs on top:
//  * the user's chosen side and the matching camera yaw (0° White, 180° Black),
//  * Standard Algebraic Notation <-> UCI long algebraic conversion,
//  * a structured move history ("1. d4 e6", "2. c4 ..."),
//  * a queue of MoveEvents the renderer consumes to animate pieces.

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "board.hpp"

// Square helpers. Tiles are row * 8 + column with a1 = 0, h8 = 63.
std::string square_name(int tile);
int square_from_name(std::string_view name);  // -1 if invalid

struct HistoryEntry {
  std::string san;   // "Nf3", "O-O", "exd5", "e8=Q#"
  std::string lan;   // "g1f3", "e1g1", "e5d6", "e7e8q"
  PieceColor color{};
  Move move;
};

struct HistoryTurn {
  int number{};
  std::string white;
  std::string black;
};

// Everything the renderer needs to animate one move in world space.
struct MoveEvent {
  int from{-1};
  int to{-1};
  Piece piece{};
  int captured_tile{-1};       // en passant captures land on another square
  Piece captured{};
  int rook_from{-1};           // castling: secondary rook slide
  int rook_to{-1};
  PieceType promotion{};
  bool is_undo{};
};

struct ParseResult {
  bool ok{};
  Move move;
  std::string san;    // canonical SAN of the parsed move
  std::string lan;    // UCI form
  std::string error;  // human-readable reason when !ok
};

// Smoothly-animated orbit camera parameters. The renderer turns these into a
// view matrix; BoardManager owns them because the side choice drives the flip.
inline constexpr float k_default_pitch{54.0F};
inline constexpr float k_default_distance{94.0F};
inline constexpr float k_establishing_pitch{19.0F};

struct CameraRig {
  float yaw{0.0F};             // degrees; 0 = White at bottom, 180 = Black
  float pitch{k_default_pitch};  // degrees above the board plane
  float distance{k_default_distance};
  float target_yaw{0.0F};
  float target_pitch{k_default_pitch};
  float target_distance{k_default_distance};

  void update(float dt);
  void snap() {
    yaw = target_yaw;
    pitch = target_pitch;
    distance = target_distance;
  }
  [[nodiscard]] bool settled() const;
};

class BoardManager {
 public:
  BoardManager();

  // ---- Side selection & camera ---------------------------------------
  void select_side(PieceColor side);
  void clear_side();  // back to the establishing shot
  [[nodiscard]] bool has_selected_side() const { return user_side_.has_value(); }
  [[nodiscard]] PieceColor user_side() const {
    return user_side_.value_or(PieceColor::White);
  }
  [[nodiscard]] static float yaw_for_side(PieceColor side) {
    return side == PieceColor::Black ? 180.0F : 0.0F;
  }
  void reset_camera();                         // snap target back to side view
  void orbit_camera(float d_yaw, float d_pitch);
  void set_camera_target(float yaw, float pitch, float distance) {
    camera_.target_yaw = yaw;
    camera_.target_pitch = pitch;
    camera_.target_distance = distance;
  }
  void flip_view() { camera_.target_yaw += 180.0F; }  // smooth 180° turn
  void zoom_camera(float d_distance);
  void update(float dt);
  void snap_camera() { camera_.snap(); }
  void clear_events() { events_.clear(); }
  [[nodiscard]] const CameraRig& camera() const { return camera_; }
  [[nodiscard]] float camera_yaw_normalized() const;  // [0, 360)

  // ---- Moves ----------------------------------------------------------
  // Parses SAN ("d4", "Nf3", "exd5", "O-O", "e8=Q") or UCI ("e2e4") against
  // the current position. Does not modify the board.
  [[nodiscard]] ParseResult parse_move(std::string_view input);
  // Parses and plays a move; returns the parse result.
  ParseResult play(std::string_view input);
  // Plays an already-legal move (e.g. from mouse picking or the engine).
  void play_move(const Move& move);
  bool undo();
  void reset();  // new game, keeps the selected side

  [[nodiscard]] std::string to_san(const Move& move);
  [[nodiscard]] static std::string to_lan(const Move& move);
  [[nodiscard]] std::optional<Move> from_lan(std::string_view lan);
  // Converts a UCI principal variation into SAN from the current position.
  [[nodiscard]] std::vector<std::string> lan_line_to_san(
      const std::vector<std::string>& lan_moves, size_t max_moves = 8) const;

  // ---- Queries ----------------------------------------------------------
  [[nodiscard]] const Board& board() const { return board_; }
  [[nodiscard]] Board& board() { return board_; }
  [[nodiscard]] const std::vector<HistoryEntry>& history() const {
    return history_;
  }
  [[nodiscard]] std::vector<HistoryTurn> history_turns() const;
  [[nodiscard]] std::string history_text() const;  // "1. d4 e6 2. c4"
  [[nodiscard]] std::vector<std::string> lan_moves() const;
  [[nodiscard]] PieceColor side_to_move() const { return board_.get_turn(); }
  [[nodiscard]] bool is_user_turn() const {
    return board_.get_turn() == user_side();
  }
  [[nodiscard]] uint64_t position_id() const { return position_id_; }
  [[nodiscard]] std::string status_text() const;  // "Check", "Checkmate"...
  [[nodiscard]] bool is_game_over() const {
    return board_.is_in_checkmate() || board_.is_in_draw();
  }

  // Renderer pulls animation events from here.
  std::optional<MoveEvent> pop_event();
  [[nodiscard]] bool has_pending_events() const { return !events_.empty(); }

 private:
  void legal_moves(Moves& out);

  Board board_;
  std::optional<PieceColor> user_side_;
  CameraRig camera_;
  std::vector<HistoryEntry> history_;
  std::deque<MoveEvent> events_;
  uint64_t position_id_{1};
};
