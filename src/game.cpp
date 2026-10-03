#include "game.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include "AssistantMath.h"

using namespace std::chrono_literals;

namespace {
constexpr float k_game_scale{10.0F};
// Height of the playing surface (top of the board mesh) in world units.
constexpr float k_board_surface{0.1555F * k_game_scale};
constexpr float k_table_height{-0.19F};
constexpr glm::vec3 k_light_dir{-0.42F, 1.0F, -0.55F};
constexpr glm::vec3 k_light_color{3.3F, 3.12F, 2.9F};
constexpr float k_floor_tile{22.0F};
// Vanishing point of the corridor in resources/textures/gallery_hall.jpg
// (u from the left, v from the top), pinned to the 3D horizon.
constexpr glm::vec2 k_gallery_vanishing_point{0.505F, 0.475F};

std::string lower(std::string s) {
  for (auto& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string trim(const std::string& s) {
  const auto b{s.find_first_not_of(" \t\r\n")};
  if (b == std::string::npos) {
    return {};
  }
  const auto e{s.find_last_not_of(" \t\r\n")};
  return s.substr(b, e - b + 1);
}

const char* color_name(PieceColor c) {
  return c == PieceColor::Black ? "black" : "white";
}

float ease_in_out(float t) {
  t = std::clamp(t, 0.0F, 1.0F);
  return t < 0.5F ? 4.0F * t * t * t : 1.0F - std::pow(-2.0F * t + 2.0F, 3.0F) / 2.0F;
}

char piece_char(Piece p) {
  constexpr std::string_view k_chars{" kqbnrp"};
  const char c{k_chars[to_underlying(get_piece_type(p))]};
  return get_piece_color(p) == PieceColor::White
             ? static_cast<char>(std::toupper(c))
             : c;
}

HttpResponse json_response(int status, std::string body) {
  return {status, "application/json", std::move(body)};
}

std::string result_json(const CommandResult& r) {
  return std::string{"{\"ok\":"} + (r.ok ? "true" : "false") + ",\"message\":\"" +
         json_escape(r.message) + "\",\"san\":\"" + json_escape(r.san) +
         "\",\"lan\":\"" + json_escape(r.lan) + "\"}";
}

std::string read_text_file(const std::string& path) {
  std::ifstream f{path, std::ios::binary};
  if (!f) {
    return {};
  }
  return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}
}  // namespace

// ===========================================================================
// Construction
// ===========================================================================

Game::Game(GLFWwindow* window, AppOptions options)
    : options_{std::move(options)}, window_{window} {
  engine_.start({.executable = options_.stockfish,
                 .threads = 0,
                 .hash_mb = 64,
                 .movetime_ms = options_.movetime_ms});

  if (window_ != nullptr) {
    graphics_ready_ = init_graphics();
  }

  if (options_.scene) {
    hud_options_.scene = *options_.scene;
  }
  hud_options_.show_perf = options_.show_perf;
  if (options_.side) {
    select_side(*options_.side);
  }
  if (options_.camera_pitch) {
    board_.orbit_camera(0.0F, *options_.camera_pitch - board_.camera().target_pitch);
  }
  for (const auto& m : options_.moves) {
    const CommandResult r{play_move_text(m)};
    if (!r.ok) {
      LOGF("GAME", "Startup move '{}' rejected: {}", m, r.message);
      break;
    }
  }
  on_position_changed();
  if (!options_.screenshot_out.empty()) {
    // Deterministic stills: no establishing orbit, no in-flight animations.
    board_.snap_camera();
    board_.clear_events();
  }
  publish_state();

  if (options_.port > 0) {
    server_.start(options_.port,
                  [this](const HttpRequest& req) { return handle_http(req); });
  }
}

Game::~Game() {
  server_.stop();
  // Fail any waiters so HTTP threads never hang on shutdown.
  for (auto& w : screenshot_waiters_) {
    w->reply.set_value(json_response(503, "{\"error\":\"shutting down\"}"));
  }
  engine_.shutdown();
  if (graphics_ready_) {
    hud_.shutdown();
  }
  pipeline_.reset();
  renderer_.reset();
}

bool Game::init_graphics() {
  const std::string res{options_.resource_dir};
  renderer_ = std::make_unique<Renderer>();
  pipeline_ = std::make_unique<RenderPipeline>();

  const auto shader = [&](const char* f) { return res + "/shaders/" + f; };
  const auto model = [&](const char* f) { return res + "/models/" + f; };
  bool ok{pipeline_->init(*renderer_, res + "/shaders")};
  ok = ok && renderer_->load_shader("picking", {shader("basic.vert"), shader("picking.frag")});
  ok = ok && renderer_->load_shader("outlining", {shader("outlining.vert"), shader("outlining.frag")});
  for (const char* m : {"board", "king", "queen", "bishop", "knight", "rook", "pawn", "tile"}) {
    ok = ok && renderer_->load_model(m, model((std::string{m} + ".gltf").c_str()));
  }
  if (!ok) {
    LOG("GAME", "Failed to load graphics resources");
    return false;
  }

  // Gallery backdrop photo (optional: falls back to the studio gradient).
  const std::string backdrop{res + "/textures/gallery_hall.jpg"};
  if (const GLuint tex = renderer_->load_texture(backdrop); tex != 0) {
    int w{};
    int h{};
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    pipeline_->set_backdrop(tex, static_cast<float>(w) / static_cast<float>(std::max(h, 1)),
                            k_gallery_vanishing_point);
  } else {
    LOG("GAME", "Gallery backdrop not found; using the studio gradient");
  }

  // Scroll zoom: registered before ImGui so its backend chains to us.
  glfwSetWindowUserPointer(window_, this);
  glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double dy) {
    auto* g{static_cast<Game*>(glfwGetWindowUserPointer(w))};
    if (!g->hud_.wants_mouse()) {
      g->board_.zoom_camera(static_cast<float>(-dy) * 3.0F);
    }
  });
  hud_.init(window_, res + "/fonts");
  hud_.set_ui_scale(options_.ui_scale);
  hud_.set_demo_mode(options_.demo);
  if (!options_.record_path.empty()) {
    hud_.set_fixed_dt(1.0F / static_cast<float>(options_.record_fps));
  }
  profiler_.init();
  return true;
}

// ===========================================================================
// Commands (shared by HUD terminal and automation API)
// ===========================================================================

void Game::select_side(PieceColor side) {
  board_.select_side(side);
  hud_.log(std::string{"You play "} + (side == PieceColor::White ? "White" : "Black") +
           ". Camera yaw -> " + (side == PieceColor::White ? "0" : "180") + " deg.");
  hud_.focus_terminal();
  autoplay_timer_ = 0.0F;
}

void Game::on_position_changed() {
  selected_tile_ = -1;
  selected_moves_ = {};
  engine_.analyze(board_.lan_moves(), board_.position_id());
  autoplay_timer_ = 0.0F;
}

CommandResult Game::play_move_text(const std::string& text) {
  CommandResult r;
  if (!board_.has_selected_side()) {
    r.message = "Choose a side first (type 'white' or 'black')";
    return r;
  }
  const PieceColor mover{board_.side_to_move()};
  const int move_no{static_cast<int>(board_.history().size()) / 2 + 1};
  const ParseResult p{board_.play(text)};
  if (!p.ok) {
    r.message = p.error;
    return r;
  }
  r.ok = true;
  r.san = p.san;
  r.lan = p.lan;
  r.message = std::to_string(move_no) + (mover == PieceColor::White ? ". " : "... ") +
              p.san + "  (" + p.lan + ")";
  if (board_.is_game_over()) {
    r.message += "  -  " + board_.status_text();
  }
  on_position_changed();
  return r;
}

CommandResult Game::run_command(const std::string& raw) {
  const std::string text{trim(raw)};
  const std::string cmd{lower(text)};
  CommandResult r;
  if (cmd.empty()) {
    r.message = "Type a move, e.g. d4";
  } else if (cmd == "help" || cmd == "?") {
    r.ok = true;
    r.message =
        "Moves: SAN (d4, Nf3, exd5, O-O, e8=Q) or UCI (e2e4). Commands: new / "
        "reset, undo, side (choose side again), flip (rotate view), white / black (switch side), auto (Stockfish "
        "plays the opponent), hint (toggle hints), scene (gallery / studio).";
  } else if (cmd == "undo" || cmd == "takeback") {
    r.ok = board_.undo();
    r.message = r.ok ? "Took back the last move" : "Nothing to undo";
    if (r.ok) {
      on_position_changed();
    }
  } else if (cmd == "reset" || cmd == "new" || cmd == "restart") {
    board_.reset();
    anim_.reset();
    on_position_changed();
    r.ok = true;
    r.message = "New game";
  } else if (cmd == "side" || cmd == "change side" || cmd == "choose side") {
    board_.clear_side();
    hud_options_.autoplay_opponent = false;
    r.ok = true;
    r.message = "Choose your side (the current game is kept)";
  } else if (cmd == "flip") {
    board_.flip_view();
    r.ok = true;
    r.message = "View flipped (press C to return to your side)";
  } else if (cmd == "white" || cmd == "black") {
    select_side(cmd == "white" ? PieceColor::White : PieceColor::Black);
    r.ok = true;
    r.message = "Side set to " + cmd;
  } else if (cmd == "auto" || cmd == "autoplay") {
    hud_options_.autoplay_opponent = !hud_options_.autoplay_opponent;
    r.ok = true;
    r.message = hud_options_.autoplay_opponent ? "Stockfish now plays your opponent"
                                               : "Manual entry for both sides";
  } else if (cmd == "scene" || cmd == "gallery" || cmd == "studio") {
    if (cmd == "scene") {
      hud_options_.scene = hud_options_.scene == SceneStyle::Gallery ? SceneStyle::Studio
                                                                      : SceneStyle::Gallery;
    } else {
      hud_options_.scene = cmd == "gallery" ? SceneStyle::Gallery : SceneStyle::Studio;
    }
    r.ok = true;
    r.message = hud_options_.scene == SceneStyle::Gallery ? "Scene: gallery hall"
                                                          : "Scene: dark studio";
  } else if (cmd == "hint" || cmd == "hints") {
    hud_options_.show_hints = !hud_options_.show_hints;
    r.ok = true;
    r.message = hud_options_.show_hints ? "Hints on" : "Hints off";
  } else {
    r = play_move_text(text);
  }
  hud_.log(r.message, !r.ok);
  last_feedback_ = r;
  return r;
}

// ===========================================================================
// Automation API
// ===========================================================================

HttpResponse Game::handle_http(const HttpRequest& req) {
  // Static console + state are served directly from the server thread.
  if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
    std::string html{read_text_file(options_.web_dir + "/console.html")};
    if (html.empty()) {
      return {404, "text/plain", "console.html not found in " + options_.web_dir};
    }
    return {200, "text/html; charset=utf-8", std::move(html)};
  }
  if (req.method == "GET" && req.path == "/api/state") {
    const std::lock_guard lock{state_mutex_};
    return json_response(200, state_json_);
  }
  if (req.method == "GET" && req.path == "/api/health") {
    return json_response(200, "{\"ok\":true}");
  }

  auto pending{std::make_unique<PendingCommand>()};
  if (req.method == "POST" && req.path == "/api/move") {
    pending->kind = "command";
    pending->arg = json_get(req.body, "san");
    if (pending->arg.empty()) {
      pending->arg = json_get(req.body, "move");
    }
  } else if (req.method == "POST" && req.path == "/api/command") {
    pending->kind = "command";
    pending->arg = json_get(req.body, "command");
  } else if (req.method == "POST" && req.path == "/api/side") {
    pending->kind = "side";
    pending->arg = lower(json_get(req.body, "side"));
  } else if (req.method == "POST" && req.path == "/api/reset") {
    pending->kind = "reset";
    pending->arg = json_get(req.body, "clearSide");
  } else if (req.method == "POST" && req.path == "/api/autoplay") {
    pending->kind = "autoplay";
    pending->arg = json_get(req.body, "enabled");
  } else if (req.method == "GET" && req.path == "/api/screenshot.png") {
    if (!graphics_ready_) {
      return json_response(409, "{\"error\":\"no window (running --headless)\"}");
    }
    pending->kind = "screenshot";
  } else {
    return json_response(404, "{\"error\":\"unknown endpoint\"}");
  }

  auto future{pending->reply.get_future()};
  {
    const std::lock_guard lock{queue_mutex_};
    queue_.push_back(std::move(pending));
  }
  if (future.wait_for(5s) != std::future_status::ready) {
    return json_response(503, "{\"error\":\"main thread busy\"}");
  }
  return future.get();
}

void Game::process_automation_queue() {
  std::vector<std::unique_ptr<PendingCommand>> work;
  {
    const std::lock_guard lock{queue_mutex_};
    work.swap(queue_);
  }
  for (auto& p : work) {
    if (p->kind == "command") {
      const CommandResult r{run_command(p->arg)};
      publish_state();
      p->reply.set_value(json_response(r.ok ? 200 : 400, result_json(r)));
    } else if (p->kind == "side") {
      if (p->arg != "white" && p->arg != "black") {
        p->reply.set_value(json_response(400, "{\"ok\":false,\"message\":\"side must be white or black\"}"));
        continue;
      }
      const CommandResult r{run_command(p->arg)};
      publish_state();
      p->reply.set_value(json_response(200, result_json(r)));
    } else if (p->kind == "reset") {
      run_command("reset");
      if (p->arg == "true") {
        board_.clear_side();
        hud_options_.autoplay_opponent = false;
      }
      publish_state();
      p->reply.set_value(json_response(200, "{\"ok\":true}"));
    } else if (p->kind == "autoplay") {
      hud_options_.autoplay_opponent = p->arg == "true";
      publish_state();
      p->reply.set_value(json_response(200, "{\"ok\":true}"));
    } else if (p->kind == "screenshot") {
      screenshot_waiters_.push_back(std::move(p));  // served after next frame
    }
  }
}

std::string Game::build_state_json() const {
  const AssistantReport& r{report_};
  const CameraRig& cam{board_.camera()};
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(2);
  o << "{";
  o << "\"app\":{\"name\":\"Chess Assist\",\"version\":\"" CHESS_ASSIST_VERSION "\",\"headless\":" << (graphics_ready_ ? "false" : "true")
    << ",\"fps\":" << fps_ << "},";
  if (board_.has_selected_side()) {
    o << "\"side\":\"" << color_name(board_.user_side()) << "\",";
  } else {
    o << "\"side\":null,";
  }
  o << "\"sideSelected\":" << (board_.has_selected_side() ? "true" : "false") << ",";
  o << "\"camera\":{\"yaw\":" << board_.camera_yaw_normalized()
    << ",\"targetYaw\":" << std::fmod(std::fmod(cam.target_yaw, 360.0F) + 360.0F, 360.0F)
    << ",\"pitch\":" << cam.pitch << ",\"distance\":" << cam.distance
    << ",\"settled\":" << (cam.settled() ? "true" : "false") << "},";
  o << "\"turn\":\"" << color_name(board_.side_to_move()) << "\",";
  o << "\"userTurn\":" << (board_.has_selected_side() && board_.is_user_turn() ? "true" : "false") << ",";
  o << "\"status\":\"" << json_escape(board_.status_text()) << "\",";
  o << "\"gameOver\":" << (board_.is_game_over() ? "true" : "false") << ",";
  o << "\"inCheck\":" << (board_.board().is_in_check() ? "true" : "false") << ",";
  o << "\"positionId\":" << board_.position_id() << ",";
  o << "\"autoplay\":" << (hud_options_.autoplay_opponent ? "true" : "false") << ",";
  o << "\"scene\":\"" << (hud_options_.scene == SceneStyle::Gallery ? "gallery" : "studio") << "\",";
  o << "\"animating\":" << (anim_ ? "true" : "false") << ",";

  o << "\"history\":[";
  for (size_t i = 0; i < board_.history().size(); ++i) {
    const auto& h{board_.history()[i]};
    o << (i ? "," : "") << "{\"ply\":" << i + 1 << ",\"san\":\"" << json_escape(h.san)
      << "\",\"lan\":\"" << h.lan << "\",\"color\":\"" << color_name(h.color) << "\"}";
  }
  o << "],";
  o << "\"historyText\":\"" << json_escape(board_.history_text()) << "\",";
  o << "\"turns\":[";
  const auto turns{board_.history_turns()};
  for (size_t i = 0; i < turns.size(); ++i) {
    o << (i ? "," : "") << "{\"n\":" << turns[i].number << ",\"white\":\""
      << json_escape(turns[i].white) << "\",\"black\":\"" << json_escape(turns[i].black)
      << "\"}";
  }
  o << "],";
  o << "\"board\":\"";
  for (int t = 0; t < 64; ++t) {
    o << (board_.board().is_empty(t) ? '.' : piece_char(board_.board().get_tile(t)));
  }
  o << "\",";
  if (!board_.history().empty()) {
    const auto& m{board_.history().back().move};
    o << "\"lastMove\":{\"from\":\"" << square_name(m.tile) << "\",\"to\":\""
      << square_name(m.target) << "\"},";
  } else {
    o << "\"lastMove\":null,";
  }

  o.precision(4);
  o << "\"assistant\":{"
    << "\"engineOk\":" << (r.engine_ok ? "true" : "false")
    << ",\"engineName\":\"" << json_escape(r.engine_name) << "\""
    << ",\"engineError\":\"" << json_escape(r.engine_error) << "\""
    << ",\"searching\":" << (r.searching ? "true" : "false")
    << ",\"upToDate\":" << (r.up_to_date ? "true" : "false")
    << ",\"complete\":" << (r.complete ? "true" : "false")
    << ",\"depth\":" << r.depth
    << ",\"positionId\":" << r.position_id
    << ",\"analysedPositionId\":" << r.analysed_position_id
    << ",\"hasEval\":" << (r.has_eval ? "true" : "false")
    << ",\"scoreCp\":" << r.score_cp
    << ",\"isMate\":" << (r.is_mate ? "true" : "false")
    << ",\"mateIn\":" << r.mate_in
    << ",\"evalText\":\"" << json_escape(r.eval_text) << "\""
    << ",\"userCp\":" << r.user_cp
    << ",\"whiteWinProbability\":" << r.white_win
    << ",\"userWinProbability\":" << r.user_win
    << ",\"bestMoveSan\":\"" << json_escape(r.best_move_san) << "\""
    << ",\"bestMoveLan\":\"" << json_escape(r.best_move_lan) << "\""
    << ",\"bestForUser\":" << (r.best_for_user ? "true" : "false")
    << ",\"headline\":\"" << json_escape(r.headline) << "\""
    << ",\"status\":\"" << json_escape(r.status) << "\""
    << ",\"pv\":[";
  for (size_t i = 0; i < r.pv_san.size(); ++i) {
    o << (i ? "," : "") << "\"" << json_escape(r.pv_san[i]) << "\"";
  }
  o << "]},";
  o << "\"perf\":{\"fps\":" << fps_ << ",\"cpuMs\":" << hud_options_.cpu_ms << ",\"gpuMs\":{";
  for (int p = 0; p < GpuProfiler::Count; ++p) {
    o << (p ? "," : "") << "\"" << GpuProfiler::k_names[static_cast<size_t>(p)]
      << "\":" << hud_options_.gpu_ms[static_cast<size_t>(p)];
  }
  o << "}},";
  o << "\"lastFeedback\":" << result_json(last_feedback_);
  o << "}";
  return o.str();
}

void Game::publish_state() {
  // Recompute so commands processed this frame are reflected immediately.
  analysis_ = engine_.snapshot();
  report_ = build_assistant_report(analysis_, board_);
  std::string json{build_state_json()};
  const std::lock_guard lock{state_mutex_};
  state_json_ = std::move(json);
}

// ===========================================================================
// Frame loop
// ===========================================================================

int Game::run() {
  auto last{std::chrono::steady_clock::now()};
  float time{};
  float fps_acc{};
  int fps_frames{};
  while (!quit_) {
    const auto now{std::chrono::steady_clock::now()};
    float dt{std::chrono::duration<float>(now - last).count()};
    last = now;
    dt = std::min(dt, 0.1F);
    if (!options_.record_path.empty()) {
      dt = 1.0F / static_cast<float>(options_.record_fps);  // fixed timestep
    }
    time += dt;
    fps_acc += dt;
    ++fps_frames;
    if (fps_acc > 0.5F) {
      fps_ = static_cast<float>(fps_frames) / fps_acc;
      hud_options_.fps = fps_;
      fps_acc = 0.0F;
      fps_frames = 0;
    }

    if (window_ != nullptr) {
      glfwPollEvents();
      if (glfwWindowShouldClose(window_) != 0) {
        quit_ = true;
      }
    }

    const auto cpu_start{std::chrono::steady_clock::now()};
    process_automation_queue();
    update(dt);

    if (graphics_ready_) {
      render_frame(time);
      // CPU time spent preparing and submitting the frame (excludes vsync).
      const float cpu_ms{std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - cpu_start).count()};
      hud_options_.cpu_ms = hud_options_.cpu_ms == 0.0F ? cpu_ms
                                                        : hud_options_.cpu_ms * 0.92F + cpu_ms * 0.08F;
      ++frame_count_;
      if (!options_.screenshot_out.empty() && frame_count_ == options_.screenshot_frames) {
        const auto pixels{pipeline_->read_back_buffer()};
        const glm::ivec2 sz{pipeline_->size()};
        stbi_write_png(options_.screenshot_out.c_str(), sz.x, sz.y, 4, pixels.data(), sz.x * 4);
        LOGF("GAME", "Saved screenshot to {}", options_.screenshot_out);
        quit_ = true;
      }
      if (!options_.record_path.empty()) {
        if (record_file_ == nullptr) {
          record_file_ = std::fopen(options_.record_path.c_str(), "wb");
          const glm::ivec2 sz{pipeline_->size()};
          LOGF("GAME", "Recording {}x{} @ {} fps to {}", sz.x, sz.y, options_.record_fps,
               options_.record_path);
        }
        const auto pixels{pipeline_->read_back_buffer()};
        if (record_file_ != nullptr) {
          std::fwrite(pixels.data(), 1, pixels.size(), record_file_);
          std::fflush(record_file_);
        }
        const int total{static_cast<int>(options_.record_seconds *
                                         static_cast<float>(options_.record_fps))};
        if (frame_count_ % options_.record_fps == 0) {
          LOGF("GAME", "Recorded frame {}/{}", frame_count_, total);
        }
        if (frame_count_ >= total) {
          std::fclose(record_file_);
          record_file_ = nullptr;
          quit_ = true;
        }
      }
      glfwSwapBuffers(window_);
    } else {
      std::this_thread::sleep_for(16ms);  // headless: ~60 Hz logic tick
    }
    publish_state();
  }
  return 0;
}

void Game::update(float dt) {
  analysis_ = engine_.snapshot();
  report_ = build_assistant_report(analysis_, board_);
  board_.update(dt);
  advance_animation(dt);
  update_autoplay(dt);
  if (options_.demo) {
    update_demo(dt);
  }
  post_.dof = hud_options_.dof;
  post_.ssao = hud_options_.ssao;
}

// Scripted 15-second showcase (used to record the README promo). Times are
// in seconds of simulated time, so recordings are frame-exact.
void Game::update_demo(float dt) {
  struct Step {
    float t;
    const char* kind;  // "type", "camera"
    const char* arg;
  };
  static constexpr std::array k_script{
      Step{4.0F, "type", "e4"},       Step{5.6F, "type", "c5"},
      Step{7.0F, "type", "Nf3"},      Step{8.6F, "type", "d6"},
      Step{9.3F, "camera", "28,30,90"}, Step{10.6F, "type", "d4"},
      Step{12.0F, "type", "cxd4"},    Step{13.0F, "camera", "-16,40,92"},
  };
  constexpr float k_char_time{0.11F};
  constexpr float k_enter_delay{0.25F};
  constexpr float k_click_time{2.15F};

  demo_time_ += dt;
  const float t{demo_time_};

  // Synthetic cursor glides to "Play White" and clicks it.
  if (!board_.has_selected_side() || t < k_click_time + 0.6F) {
    const auto target{hud_.white_button_center()};
    const float u{glm::smoothstep(0.8F, 2.0F, t)};
    const std::array<float, 2> from{target[0] + 330.0F, target[1] + 300.0F};
    const std::array<float, 2> pos{from[0] + (target[0] - from[0]) * u,
                                   from[1] + (target[1] - from[1]) * u};
    const bool pressed{t > k_click_time - 0.12F && t < k_click_time + 0.15F};
    hud_.set_demo_cursor(t > 0.6F ? std::optional{pos} : std::nullopt, pressed);
    if (t >= k_click_time && !board_.has_selected_side()) {
      run_command("white");
    }
  } else {
    hud_.set_demo_cursor(std::nullopt, false);
  }

  // Typing into the terminal, one character at a time, then Enter.
  std::string typing;
  for (size_t i = 0; i < k_script.size(); ++i) {
    const Step& st{k_script[i]};
    if (std::string_view{st.kind} == "camera") {
      if (t >= st.t && demo_step_ <= i) {
        float yaw{};
        float pitch{};
        float dist{};
        std::sscanf(st.arg, "%f,%f,%f", &yaw, &pitch, &dist);
        board_.set_camera_target(yaw, pitch, dist);
        demo_step_ = i + 1;
      }
      continue;
    }
    const std::string_view text{st.arg};
    const float done{st.t + static_cast<float>(text.size()) * k_char_time + k_enter_delay};
    if (t >= st.t && t < done) {
      const auto n{std::min(text.size(),
                            static_cast<size_t>((t - st.t) / k_char_time) + 1)};
      typing = std::string{text.substr(0, n)};
    } else if (t >= done && demo_step_ <= i) {
      run_command(std::string{text});
      demo_step_ = i + 1;
    }
  }
  hud_.set_demo_input(typing);
}

void Game::update_autoplay(float dt) {
  if (!hud_options_.autoplay_opponent || !board_.has_selected_side() ||
      board_.is_user_turn() || board_.is_game_over() || anim_) {
    autoplay_timer_ = 0.0F;
    return;
  }
  autoplay_timer_ += dt;
  if (autoplay_timer_ < 0.5F || !report_.complete || report_.best_move_lan.empty()) {
    return;
  }
  if (auto m = board_.from_lan(report_.best_move_lan)) {
    const std::string san{board_.to_san(*m)};
    const int move_no{static_cast<int>(board_.history().size()) / 2 + 1};
    const bool white{board_.side_to_move() == PieceColor::White};
    board_.play_move(*m);
    hud_.log("Stockfish: " + std::to_string(move_no) + (white ? ". " : "... ") + san);
    on_position_changed();
  }
}

void Game::advance_animation(float dt) {
  if (!anim_) {
    if (auto ev = board_.pop_event()) {
      Animation a;
      a.ev = *ev;
      const glm::vec3 d{tile_position(ev->to) - tile_position(ev->from)};
      a.duration = std::clamp(0.30F + glm::length(d) * 0.010F, 0.34F, 0.62F);
      anim_ = a;
    }
    return;
  }
  // If more moves are queued (fast typing / API), hurry the current one.
  const float speed{board_.has_pending_events() ? 3.0F : 1.0F};
  anim_->t += dt * speed / anim_->duration;
  if (anim_->t >= 1.0F) {
    anim_.reset();
  }
}

// ===========================================================================
// Rendering
// ===========================================================================

glm::vec3 Game::tile_position(int tile) {
  return glm::vec3{(-2.03F + static_cast<float>(7 - get_tile_column(tile)) * 0.58F) * k_game_scale,
                   k_board_surface,
                   (-2.03F + static_cast<float>(get_tile_row(tile)) * 0.58F) * k_game_scale};
}

std::string_view Game::model_for(PieceType type) {
  switch (type) {
    case PieceType::King:
      return "king";
    case PieceType::Queen:
      return "queen";
    case PieceType::Bishop:
      return "bishop";
    case PieceType::Knight:
      return "knight";
    case PieceType::Rook:
      return "rook";
    case PieceType::Pawn:
      return "pawn";
    default:
      return {};
  }
}

std::vector<Game::PieceDraw> Game::collect_piece_draws() const {
  std::vector<PieceDraw> out;
  const Board& b{board_.board()};
  const auto make = [](int tile, Piece p, const glm::vec3& pos) {
    PieceDraw d;
    d.model = model_for(get_piece_type(p));
    d.black = get_piece_color(p) == PieceColor::Black;
    d.transform = {pos, d.black ? 0.0F : -180.0F, k_game_scale};
    d.tile = tile;
    return d;
  };
  for (int tile = 0; tile < 64; ++tile) {
    if (b.is_empty(tile)) {
      continue;
    }
    Piece piece{b.get_tile(tile)};
    glm::vec3 pos{tile_position(tile)};
    if (anim_) {
      const MoveEvent& ev{anim_->ev};
      const float t{anim_->t};
      if (tile == ev.to) {
        const glm::vec3 a{tile_position(ev.from)};
        const glm::vec3 c{tile_position(ev.to)};
        const float e{ease_in_out(t)};
        const bool knight{get_piece_type(ev.piece) == PieceType::Knight};
        const float lift{knight ? 4.5F : 1.2F + glm::length(c - a) * 0.05F};
        pos = glm::mix(a, c, e);
        pos.y += std::sin(e * 3.14159265F) * lift;
        if (!ev.is_undo && ev.promotion != PieceType::None && t < 0.98F) {
          piece = ev.piece;  // show the pawn until it lands
        }
      } else if (tile == ev.rook_to && ev.rook_from >= 0 && !ev.is_undo) {
        const float e{ease_in_out(std::clamp(t * 1.25F - 0.25F, 0.0F, 1.0F))};
        pos = glm::mix(tile_position(ev.rook_from), tile_position(ev.rook_to), e);
        pos.y += std::sin(e * 3.14159265F) * 0.8F;
      }
    }
    out.push_back(make(tile, piece, pos));
  }
  // Captured piece shrinks into the board as the attacker lands.
  if (anim_ && !anim_->ev.is_undo && anim_->ev.captured_tile >= 0) {
    const float s{1.0F - glm::smoothstep(0.55F, 0.92F, anim_->t)};
    if (s > 0.01F) {
      PieceDraw d{make(-1, anim_->ev.captured, tile_position(anim_->ev.captured_tile))};
      d.transform.scale *= s;
      out.push_back(d);
    }
  }
  return out;
}

void Game::set_material(int kind, float rough_scale, float rough_bias,
                        float clearcoat, float cc_rough, float reflection,
                        float contact_ao, float base_height,
                        const glm::vec4& tint) {
  Renderer& r{*renderer_};
  r.set_shader_uniform("material_kind", kind);
  r.set_shader_uniform("roughness_scale", rough_scale);
  r.set_shader_uniform("roughness_bias", rough_bias);
  r.set_shader_uniform("clearcoat", clearcoat);
  r.set_shader_uniform("clearcoat_roughness", cc_rough);
  r.set_shader_uniform("reflection_strength", reflection);
  r.set_shader_uniform("receive_contact_ao", contact_ao);
  r.set_shader_uniform("piece_base_height", base_height);
  r.set_shader_uniform("tint", tint);
}

void Game::set_pbr_frame_uniforms() {
  Renderer& r{*renderer_};
  r.set_shader_uniform("view_pos", camera_.get_position());
  r.set_shader_uniform("light_dir", normalize(k_light_dir));
  r.set_shader_uniform("light_color", k_light_color);
  r.set_shader_uniform("shadow_softness", 2.6F);
  const bool gallery{hud_options_.scene == SceneStyle::Gallery};
  r.set_shader_uniform("env_mode", gallery ? 1.0F : 0.0F);
  r.set_shader_uniform("floor_tile", k_floor_tile);
  r.set_shader_uniform("fog_start", gallery ? 70.0F : 38.0F);
  r.set_shader_uniform("fog_end", gallery ? 175.0F : 95.0F);
  r.set_shader_uniform("clip_plane", glm::vec4{0.0F, 0.0F, 0.0F, 1.0F});

  // Contact-AO occluders: every piece's base position and footprint radius.
  std::array<glm::vec4, 32> occ{};
  int n{};
  for (const PieceDraw& d : collect_piece_draws()) {
    if (n >= 32) {
      break;
    }
    const float radius{(d.model == "pawn" ? 1.7F : 2.2F) * d.transform.scale / k_game_scale};
    occ[static_cast<size_t>(n++)] = glm::vec4{d.transform.position, radius};
  }
  r.set_shader_uniform_array("occluders", occ.data(), n);
  r.set_shader_uniform("occluder_count", n);
}

void Game::draw_scene_objects(bool reflection_pass) {
  Renderer& r{*renderer_};
  const auto pieces{collect_piece_draws()};

  if (!reflection_pass) {
    // Ground: polished black/white marble hall floor (gallery) or a dark
    // slate table (studio) - one huge plane under the board.
    if (hud_options_.scene == SceneStyle::Gallery) {
      set_material(2, 1.0F, 0.0F, 0.55F, 0.05F, 0.0F, 1.0F, -1000.0F);
    } else {
      set_material(1, 1.0F, 0.35F, 0.0F, 0.1F, 0.0F, 0.8F, -1000.0F);
    }
    const glm::mat4 table{glm::scale(
        glm::translate(glm::mat4{1.0F}, {0.0F, k_table_height, 0.0F}),
        glm::vec3{520.0F, 1.0F, 520.0F})};
    r.draw_model_matrix("tile", table);

    // Board: lacquered wood with planar reflections of the pieces.
    set_material(0, 0.85F, 0.0F, 0.9F, 0.05F, 0.85F, 1.0F, -1000.0F);
    r.draw_model("board", {.rotation = -90.0F, .scale = k_game_scale});
  }

  for (const PieceDraw& d : pieces) {
    const bool hovered{!reflection_pass && d.tile >= 0 && d.tile == hover_id_};
    const bool selected{!reflection_pass && d.tile >= 0 && d.tile == selected_tile_};
    const glm::vec4 tint{selected ? glm::vec4{0.28F, 0.88F, 0.63F, 0.10F}
                         : hovered ? glm::vec4{1.0F, 1.0F, 1.0F, 0.035F}
                                   : glm::vec4{0.0F}};
    // Pieces: glossy lacquer, black slightly glossier for a high-contrast set.
    set_material(0, d.black ? 0.45F : 0.55F, 0.0F, d.black ? 1.0F : 0.75F,
                 d.black ? 0.06F : 0.09F, 0.0F, 0.0F, k_board_surface, tint);
    const bool outline{hovered || selected};
    if (outline) {
      Renderer::begin_outlining();
    }
    r.draw_model(d.model, d.transform, d.black);
    if (outline) {
      Renderer::end_outlining();
      r.install_shader("outlining");
      r.draw_model_outline(d.model, d.transform, selected ? 0.012F : 0.008F,
                           selected ? glm::vec4{0.30F, 1.6F, 1.05F, 1.0F}
                                    : glm::vec4{1.2F, 1.2F, 1.2F, 1.0F});
      r.install_shader("pbr");
    }
  }
}

void Game::draw_highlights(float time) {
  Renderer& r{*renderer_};
  r.install_shader("highlight");
  r.set_frame(projection_, view_);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  r.set_shader_uniform("highlight_time", time);

  const float half{0.29F * k_game_scale};
  const auto square = [&](int tile, const glm::vec3& color, int mode, float intensity,
                          float size = 1.0F) {
    if (!is_valid_tile(tile)) {
      return;
    }
    glm::vec3 p{tile_position(tile)};
    p.y += 0.03F;
    r.set_shader_uniform("highlight_center", p);
    r.set_shader_uniform("highlight_half", half * size);
    r.set_shader_uniform("highlight_color", color);
    r.set_shader_uniform("highlight_mode", mode);
    r.set_shader_uniform("highlight_intensity", intensity);
    r.draw_model("tile", {p, 0.0F, k_game_scale * size});
  };

  // Last move: soft warm fill on both squares.
  if (!board_.history().empty()) {
    const Move& m{board_.history().back().move};
    square(m.tile, {0.85F, 0.62F, 0.30F}, 2, 0.55F);
    square(m.target, {0.85F, 0.62F, 0.30F}, 2, 0.75F);
  }

  // King in check: red pulse.
  if (board_.board().is_in_check()) {
    for (int t = 0; t < 64; ++t) {
      if (board_.board().is_piece(t, board_.side_to_move(), PieceType::King)) {
        square(t, {1.0F, 0.16F, 0.12F}, 0, 1.6F);
      }
    }
  }

  // Engine recommendation: glowing frames + dotted path from -> to.
  const bool show{report_.best_from >= 0 && report_.best_to >= 0 && !anim_ &&
                  board_.has_selected_side() &&
                  (hud_options_.show_hints || !report_.best_for_user)};
  if (show) {
    const glm::vec3 c{report_.best_for_user ? glm::vec3{1.0F, 0.56F, 0.10F}
                                            : glm::vec3{0.55F, 0.70F, 1.0F}};
    square(report_.best_from, c, 0, 1.0F);
    square(report_.best_to, c, 0, 1.8F);
    const glm::vec3 a{tile_position(report_.best_from)};
    const glm::vec3 b{tile_position(report_.best_to)};
    const float len{glm::length(b - a)};
    const int dots{std::max(1, static_cast<int>(len / 1.7F))};
    for (int i = 1; i < dots; ++i) {
      const float f{static_cast<float>(i) / static_cast<float>(dots)};
      glm::vec3 p{glm::mix(a, b, f)};
      p.y += 0.04F;
      // Dots travel along the path for a sense of direction.
      const float wave{0.55F + 0.45F * std::sin(time * 6.0F - f * 9.0F)};
      r.set_shader_uniform("highlight_center", p);
      r.set_shader_uniform("highlight_half", 0.45F);
      r.set_shader_uniform("highlight_color", c);
      r.set_shader_uniform("highlight_mode", 1);
      r.set_shader_uniform("highlight_intensity", 1.2F * wave);
      r.draw_model("tile", {p, 0.0F, 0.45F / 0.29F});
    }
  }

  // Legal targets of the selected piece.
  for (int i = 0; i < selected_moves_.size; ++i) {
    const int t{selected_moves_.data[i].target};
    const bool capture{!board_.board().is_empty(t)};
    square(t, {0.20F, 0.80F, 0.55F}, capture ? 0 : 1, capture ? 1.2F : 1.0F,
           capture ? 1.0F : 0.32F);
  }

  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glEnable(GL_CULL_FACE);
}

void Game::draw_picking() {
  double mx{};
  double my{};
  glfwGetCursorPos(window_, &mx, &my);
  // The id pass is only needed when something under the cursor can change:
  // cursor moved, a button is down, pieces/camera are moving, or the
  // position changed. Otherwise reuse last frame's result.
  const bool clicking{glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS};
  const bool dirty{mx != pick_cursor_.x || my != pick_cursor_.y || clicking || anim_ ||
                   !board_.camera().settled() || board_.position_id() != pick_position_};
  if (!dirty) {
    return;
  }
  pick_cursor_ = {mx, my};
  pick_position_ = board_.position_id();

  Renderer& r{*renderer_};
  pipeline_->begin_picking_pass();
  r.install_shader("picking");
  r.set_frame(projection_, view_);
  for (int t = 0; t < 64; ++t) {
    r.set_shader_uniform("color", t);
    r.draw_model("tile", {tile_position(t), 0.0F, k_game_scale});
  }
  for (const PieceDraw& d : collect_piece_draws()) {
    if (d.tile < 0) {
      continue;
    }
    r.set_shader_uniform("color", d.tile);
    r.draw_model(d.model, d.transform);
  }
  int ww{};
  int wh{};
  glfwGetWindowSize(window_, &ww, &wh);
  const glm::ivec2 fb{pipeline_->size()};
  const float sx{static_cast<float>(fb.x) / static_cast<float>(std::max(ww, 1))};
  const float sy{static_cast<float>(fb.y) / static_cast<float>(std::max(wh, 1))};
  const glm::ivec2 px{static_cast<int>(static_cast<float>(mx) * sx),
                      fb.y - 1 - static_cast<int>(static_cast<float>(my) * sy)};
  const int picked{pipeline_->end_picking_pass(px)};
  // No hover outlines in --screenshot stills (the cursor sits mid-window).
  hover_id_ = hud_.wants_mouse() || !options_.screenshot_out.empty() ? -1 : picked;
}

void Game::handle_mouse_and_keys() {
  // Orbit with middle or right mouse drag.
  double mx{};
  double my{};
  glfwGetCursorPos(window_, &mx, &my);
  const bool orbit_btn{glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS ||
                       glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS};
  if (orbit_btn && (orbiting_ || !hud_.wants_mouse())) {
    if (orbiting_) {
      board_.orbit_camera(static_cast<float>(mx - last_mouse_.x) * 0.25F,
                          static_cast<float>(my - last_mouse_.y) * 0.2F);
    }
    orbiting_ = true;
  } else {
    orbiting_ = false;
  }
  last_mouse_ = {mx, my};

  if (!hud_.wants_keyboard()) {
    const bool c{glfwGetKey(window_, GLFW_KEY_C) == GLFW_PRESS};
    if (c && !key_c_down_) {
      board_.reset_camera();
    }
    key_c_down_ = c;
    const bool f{glfwGetKey(window_, GLFW_KEY_F11) == GLFW_PRESS};
    if (f && !key_f_down_) {
      if (is_fullscreen_) {
        glfwSetWindowMonitor(window_, nullptr, windowed_rect_.x, windowed_rect_.y,
                             windowed_rect_.z, windowed_rect_.w, 0);
      } else {
        glfwGetWindowPos(window_, &windowed_rect_.x, &windowed_rect_.y);
        glfwGetWindowSize(window_, &windowed_rect_.z, &windowed_rect_.w);
        GLFWmonitor* mon{glfwGetPrimaryMonitor()};
        const GLFWvidmode* mode{glfwGetVideoMode(mon)};
        glfwSetWindowMonitor(window_, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
      }
      is_fullscreen_ = !is_fullscreen_;
    }
    key_f_down_ = f;
  }

  // Click-to-move (works alongside the terminal).
  static bool was_down{};
  const bool down{glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS};
  if (down && !was_down && !hud_.wants_mouse() && board_.has_selected_side() && !anim_) {
    const int t{hover_id_};
    bool moved{};
    if (selected_tile_ >= 0 && is_valid_tile(t)) {
      for (int i = 0; i < selected_moves_.size; ++i) {
        const Move& m{selected_moves_.data[i]};
        if (m.target == t && (m.promotion == PieceType::None || m.promotion == PieceType::Queen)) {
          run_command(BoardManager::to_lan(m));
          moved = true;
          break;
        }
      }
    }
    if (!moved) {
      const bool own{is_valid_tile(t) && !board_.board().is_empty(t) &&
                     board_.board().get_color(t) == board_.side_to_move() &&
                     (!hud_options_.autoplay_opponent || board_.is_user_turn())};
      if (own && t != selected_tile_) {
        selected_tile_ = t;
        selected_moves_ = {};
        board_.board().generate_legal_moves(selected_moves_, t);
      } else {
        selected_tile_ = -1;
        selected_moves_ = {};
      }
    }
  }
  was_down = down;
}

void Game::render_frame(float time) {
  int fbw{};
  int fbh{};
  glfwGetFramebufferSize(window_, &fbw, &fbh);
  if (fbw <= 0 || fbh <= 0) {
    return;  // minimised
  }
  pipeline_->resize(fbw, fbh);
  Renderer& r{*renderer_};

  const CameraRig& rig{board_.camera()};
  camera_.set_orbit(rig.yaw, rig.pitch, rig.distance);
  projection_ = camera_.calculate_projection_matrix(static_cast<float>(fbw) / static_cast<float>(fbh));
  // Lens shift: lift the board's image so it sits between the HUD panels
  // and above the move terminal instead of dead-centre.
  projection_ = glm::translate(glm::mat4{1.0F}, {-0.085F, 0.15F, 0.0F}) * projection_;
  view_ = camera_.calculate_view_matrix();
  post_.focus_distance = camera_.get_distance();
  // Blur radii are in pixels: scale them for Retina / HiDPI framebuffers.
  int win_w{};
  glfwGetWindowSize(window_, &win_w, nullptr);
  const float dpi{static_cast<float>(fbw) / static_cast<float>(std::max(win_w, 1)) *
                  options_.ui_scale};
  post_.max_coc = 14.0F * dpi;
  post_.dof_scale = 55.0F * dpi;

  // Backdrop: project the horizon (a horizontal direction at infinity) to
  // the screen so the photo's vanishing point lines up with the 3D floor.
  const bool gallery{hud_options_.scene == SceneStyle::Gallery};
  post_.backdrop_mode = gallery ? 1 : 0;
  post_.vignette = gallery ? 0.86F : 0.72F;
  post_.exposure = gallery ? 0.95F : 1.05F;
  {
    glm::vec3 fwd{camera_.get_target() - camera_.get_position()};
    fwd.y = 0.0F;
    const glm::vec4 clip{projection_ * view_ * glm::vec4{normalize(fwd), 0.0F}};
    post_.horizon_y = std::abs(clip.w) > 1e-5F ? clip.y / clip.w * 0.5F + 0.5F : 2.0F;
    // Pan the photo with the orbit (mirrored-repeat makes it seamless).
    post_.backdrop_shift_u = (board_.camera().yaw - BoardManager::yaw_for_side(board_.user_side())) / 120.0F;
  }

  profiler_.begin(GpuProfiler::Picking);
  draw_picking();
  profiler_.end();
  handle_mouse_and_keys();

  // 1) Shadow map.
  profiler_.begin(GpuProfiler::Shadow);
  pipeline_->begin_shadow_pass();
  r.install_shader("shadow");
  r.set_frame(pipeline_->light_projection(), pipeline_->light_view());
  r.draw_model("board", {.rotation = -90.0F, .scale = k_game_scale});
  for (const PieceDraw& d : collect_piece_draws()) {
    r.draw_model(d.model, d.transform);
  }
  pipeline_->end_shadow_pass();
  profiler_.end();

  // 2) Planar reflection: mirror the pieces about the board surface.
  profiler_.begin(GpuProfiler::Reflection);
  pipeline_->begin_reflection_pass();
  r.install_shader("pbr");
  const glm::mat4 mirror{glm::translate(glm::mat4{1.0F}, {0.0F, k_board_surface, 0.0F}) *
                         glm::scale(glm::mat4{1.0F}, {1.0F, -1.0F, 1.0F}) *
                         glm::translate(glm::mat4{1.0F}, {0.0F, -k_board_surface, 0.0F})};
  r.set_frame(projection_, view_ * mirror);
  pipeline_->bind_scene_inputs(r, false);
  set_pbr_frame_uniforms();
  r.set_shader_uniform("view_pos", glm::vec3{mirror * glm::vec4{camera_.get_position(), 1.0F}});
  r.set_shader_uniform("clip_plane", glm::vec4{0.0F, 1.0F, 0.0F, -k_board_surface + 0.01F});
  draw_scene_objects(true);
  pipeline_->end_reflection_pass();
  profiler_.end();

  // 3) Main HDR scene.
  profiler_.begin(GpuProfiler::Scene);
  pipeline_->begin_scene_pass();
  r.install_shader("pbr");
  r.set_frame(projection_, view_);
  pipeline_->bind_scene_inputs(r, true);
  set_pbr_frame_uniforms();
  draw_scene_objects(false);
  draw_highlights(time);
  pipeline_->end_scene_pass();
  profiler_.end();

  // 4) Post-processing to the backbuffer.
  profiler_.begin(GpuProfiler::Post);
  pipeline_->run_post(r, projection_, time, post_);
  profiler_.end();

  // 5) HUD.
  for (int p = 0; p < GpuProfiler::Count; ++p) {
    hud_options_.gpu_ms[static_cast<size_t>(p)] = profiler_.ms(p);
  }
  profiler_.begin(GpuProfiler::Hud);
  hud_.begin_frame();
  const HudActions actions{hud_.draw(report_, board_, hud_options_)};
  hud_.end_frame();
  profiler_.end();
  profiler_.end_frame();
  if (actions.select_side) {
    run_command(*actions.select_side == PieceColor::White ? "white" : "black");
  }
  if (actions.command) {
    run_command(*actions.command);
  }
  if (actions.toggle_autoplay) {
    run_command("auto");
  }

  // 6) Serve pending screenshot requests with the finished frame.
  if (!screenshot_waiters_.empty()) {
    const auto pixels{pipeline_->read_back_buffer()};
    const glm::ivec2 sz{pipeline_->size()};
    std::string png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
          static_cast<std::string*>(ctx)->append(static_cast<const char*>(data),
                                                 static_cast<size_t>(size));
        },
        &png, sz.x, sz.y, 4, pixels.data(), sz.x * 4);
    for (auto& w : screenshot_waiters_) {
      w->reply.set_value({200, "image/png", png});
    }
    screenshot_waiters_.clear();
  }
}
