#pragma once
// WinningAssistantHUD.h — evaluation calculator + Dear ImGui heads-up display.
//
// build_assistant_report() turns raw engine output into what the user cares
// about: whose move is recommended, the move in SAN, the centipawn advantage
// and the win probability from the *user's* side. The report feeds both the
// in-window HUD and the JSON automation API, so the two always agree.

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "BoardManager.h"
#include "StockfishEngine.h"

struct GLFWwindow;
struct ImFont;

struct AssistantReport {
  // Engine state
  bool engine_ok{};
  bool searching{};
  bool up_to_date{};     // analysis belongs to the current position
  bool complete{};       // engine finished (bestmove received)
  int depth{};
  std::string engine_name;
  std::string engine_error;
  uint64_t position_id{};
  uint64_t analysed_position_id{};

  // Evaluation (valid when has_eval)
  bool has_eval{};
  int score_cp{};        // White's perspective
  bool is_mate{};
  int mate_in{};
  std::string eval_text;       // "+0.34" / "#3"
  int user_cp{};               // the user's perspective, mate as ±10000
  double white_win{0.5};
  double user_win{0.5};

  // Recommendation
  std::string best_move_san;   // "Nf3"
  std::string best_move_lan;   // "g1f3"
  int best_from{-1};
  int best_to{-1};
  bool best_for_user{};        // the user is the side to move
  std::vector<std::string> pv_san;
  std::string headline;        // "Your move: play Nf3"
  std::string status;          // short engine status line
};

AssistantReport build_assistant_report(const EngineAnalysis& analysis,
                                       BoardManager& board);

enum class SceneStyle : uint8_t { Studio, Gallery };

struct HudOptions {
  SceneStyle scene{SceneStyle::Gallery};
  bool autoplay_opponent{};
  bool show_hints{true};
  bool dof{true};
  bool ssao{true};
  bool show_perf{false};             // F3: per-pass GPU timings
  float fps{};
  float cpu_ms{};
  std::array<float, 6> gpu_ms{};     // GpuProfiler::Pass order
};

struct HudActions {
  std::optional<PieceColor> select_side;
  std::optional<std::string> command;  // terminal line (move or command)
  bool toggle_autoplay{};
};

class WinningAssistantHUD {
 public:
  bool init(GLFWwindow* window, const std::string& font_dir);
  void shutdown();

  void begin_frame();
  HudActions draw(const AssistantReport& report, const BoardManager& board,
                  HudOptions& options);
  void end_frame();

  // Terminal output (most recent last).
  void log(std::string text, bool error = false);
  void focus_terminal() { focus_terminal_ = true; }

  // --- Scripted demo / recording support -------------------------------
  void set_fixed_dt(float dt) { fixed_dt_ = dt; }        // 0 = real time
  void set_ui_scale(float scale) { ui_scale_ = scale; }  // logical-to-pixel scale
  void set_demo_mode(bool on) { demo_ = on; }
  void set_demo_input(std::string text) { demo_input_ = std::move(text); }
  // Synthetic mouse cursor in logical HUD coordinates (nullopt = hidden).
  void set_demo_cursor(std::optional<std::array<float, 2>> pos, bool pressed) {
    demo_cursor_ = pos;
    demo_pressed_ = pressed;
  }
  [[nodiscard]] std::array<float, 2> white_button_center() const { return white_btn_; }

  [[nodiscard]] bool wants_mouse() const;
  [[nodiscard]] bool wants_keyboard() const;

 private:
  void apply_style();
  void draw_brand(const BoardManager& board);
  void draw_side_selection(HudActions& actions, float w, float h);
  void draw_history(const BoardManager& board, HudActions& actions, float h);
  void draw_new_game_confirm(const BoardManager& board, HudActions& actions);
  void draw_game_over(const BoardManager& board, HudActions& actions);
  void draw_assistant(const AssistantReport& report, const BoardManager& board,
                      HudOptions& options, HudActions& actions, float w);
  void draw_terminal(HudActions& actions, float w, float h);
  void draw_perf(const HudOptions& options);

  struct LogLine {
    std::string text;
    bool error{};
  };

  bool initialized_{};
  ImFont* ui_font_{};
  ImFont* mono_font_{};
  std::deque<LogLine> log_;
  char input_[128]{};
  bool focus_terminal_{true};
  bool side_chosen_{};
  bool request_new_game_{};
  float fixed_dt_{};
  float ui_scale_{1.0F};
  bool demo_{};
  std::string demo_input_;
  std::optional<std::array<float, 2>> demo_cursor_;
  bool demo_pressed_{};
  std::array<float, 2> white_btn_{};
  std::vector<std::string> input_history_;
  int input_history_pos_{-1};
  size_t last_history_size_{};

  // Smoothed display values so the bar glides instead of jumping.
  float shown_user_win_{0.5F};
  float shown_eval_{0.0F};
  std::string last_best_san_;
  bool last_best_for_user_{};
  float best_flash_{};
};
