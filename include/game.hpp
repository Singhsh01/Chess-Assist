#pragma once
// game.hpp — application orchestrator: owns the board, engine, renderer, HUD
// and automation bridge, and runs the frame loop.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "AutomationServer.h"
#include "GpuProfiler.h"
#include "BoardManager.h"
#include "RenderPipeline.h"
#include "StockfishEngine.h"
#include "WinningAssistantHUD.h"
#include "renderer.hpp"

inline constexpr glm::vec2 k_window_size{1440.0F, 900.0F};

struct AppOptions {
  bool headless{};                 // no window: logic + engine + automation only
  int port{8765};                  // automation console port (0 = off)
  std::string stockfish;           // explicit engine path
  int movetime_ms{1500};
  bool gl_debug{};
  std::string screenshot_out;      // save a PNG after `screenshot_frames` and exit
  int screenshot_frames{90};
  std::optional<PieceColor> side;  // preselect side (skips the prompt)
  std::vector<std::string> moves;  // moves to play on startup
  std::optional<SceneStyle> scene;  // --scene studio|gallery
  std::optional<float> camera_pitch;  // --pitch degrees
  bool show_perf{};                   // --perf
  bool demo{};                        // --demo: scripted 15 s showcase
  std::string record_path;            // --record FILE: raw RGBA frames
  int record_fps{30};
  float record_seconds{15.0F};
  float ui_scale{1.0F};               // --ui-scale (HUD + blur radii)
  std::string resource_dir;
  std::string web_dir;
};

struct CommandResult {
  bool ok{};
  std::string message;
  std::string san;
  std::string lan;
};

class Game {
 public:
  Game(GLFWwindow* window, AppOptions options);
  ~Game();
  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;
  Game(Game&&) = delete;
  Game& operator=(Game&&) = delete;

  int run();

 private:
  // ---- logic ---------------------------------------------------------------
  CommandResult run_command(const std::string& text);
  CommandResult play_move_text(const std::string& text);
  void select_side(PieceColor side);
  void on_position_changed();
  void update(float dt);
  void update_autoplay(float dt);
  void update_demo(float dt);
  void process_automation_queue();
  void publish_state();
  [[nodiscard]] std::string build_state_json() const;

  // ---- automation ------------------------------------------------------------
  HttpResponse handle_http(const HttpRequest& req);
  struct PendingCommand {
    std::string kind;   // "command" | "side" | "reset" | "screenshot"
    std::string arg;
    std::promise<HttpResponse> reply;
  };

  // ---- rendering -------------------------------------------------------------
  bool init_graphics();
  void render_frame(float time);
  void draw_scene_objects(bool reflection_pass);
  void draw_highlights(float time);
  void draw_picking();
  void handle_mouse_and_keys();
  void set_pbr_frame_uniforms();
  void set_material(int kind, float rough_scale, float rough_bias, float clearcoat,
                    float cc_rough, float reflection, float contact_ao,
                    float base_height, const glm::vec4& tint = {});

  struct PieceDraw {
    std::string_view model;
    Transform transform;
    bool black{};
    int tile{-1};
  };
  [[nodiscard]] std::vector<PieceDraw> collect_piece_draws() const;
  static glm::vec3 tile_position(int tile);
  static std::string_view model_for(PieceType type);

  // ---- animation ---------------------------------------------------------------
  struct Animation {
    MoveEvent ev;
    float t{};
    float duration{0.45F};
  };
  void advance_animation(float dt);

  AppOptions options_;
  GLFWwindow* window_{};

  BoardManager board_;
  StockfishEngine engine_;
  EngineAnalysis analysis_;
  AssistantReport report_;
  AutomationServer server_;

  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<RenderPipeline> pipeline_;
  Camera camera_;
  glm::mat4 projection_{1.0F};
  glm::mat4 view_{1.0F};
  PostSettings post_;
  GpuProfiler profiler_;
  WinningAssistantHUD hud_;
  HudOptions hud_options_;
  bool graphics_ready_{};

  std::optional<Animation> anim_;
  float autoplay_timer_{};
  float demo_time_{};
  size_t demo_step_{};
  FILE* record_file_{};

  // Mouse picking & selection
  int hover_id_{-1};
  int selected_tile_{-1};
  Moves selected_moves_{};
  glm::dvec2 last_mouse_{};
  glm::dvec2 pick_cursor_{-1.0, -1.0};
  uint64_t pick_position_{};
  bool orbiting_{};
  bool key_c_down_{};
  bool key_f_down_{};
  bool is_fullscreen_{};
  glm::ivec4 windowed_rect_{};

  // Automation plumbing
  std::mutex queue_mutex_;
  std::vector<std::unique_ptr<PendingCommand>> queue_;
  std::vector<std::unique_ptr<PendingCommand>> screenshot_waiters_;
  mutable std::mutex state_mutex_;
  std::string state_json_;
  CommandResult last_feedback_;

  float fps_{};
  int frame_count_{};
  bool quit_{};
};
