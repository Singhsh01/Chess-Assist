#include "WinningAssistantHUD.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "AssistantMath.h"

// ===========================================================================
// Evaluation calculator
// ===========================================================================

AssistantReport build_assistant_report(const EngineAnalysis& a,
                                       BoardManager& board) {
  AssistantReport r;
  const PieceColor user{board.user_side()};
  r.position_id = board.position_id();
  r.analysed_position_id = a.position_id;
  r.engine_name = a.engine_name;
  r.engine_error = a.error;
  r.engine_ok = a.status != EngineAnalysis::Status::Error &&
                a.status != EngineAnalysis::Status::Offline;
  r.up_to_date = a.position_id == board.position_id();
  r.searching = r.up_to_date && a.status == EngineAnalysis::Status::Searching;
  r.complete = r.up_to_date && a.complete;
  r.depth = r.up_to_date ? a.depth : 0;
  r.best_for_user = board.has_selected_side() && board.is_user_turn();

  if (board.board().is_in_checkmate()) {
    // Side to move is mated.
    const bool white_mated{board.side_to_move() == PieceColor::White};
    r.has_eval = true;
    r.eval_text = white_mated ? "0-1" : "1-0";
    r.white_win = white_mated ? 0.0 : 1.0;
    r.user_win = user == PieceColor::White ? r.white_win : 1.0 - r.white_win;
    r.user_cp = r.user_win > 0.5 ? 10000 : -10000;
    r.headline = board.status_text();
    r.status = "Game over";
    return r;
  }
  if (board.board().is_in_draw()) {
    r.has_eval = true;
    r.eval_text = "1/2";
    r.white_win = r.user_win = 0.5;
    r.headline = "Stalemate - draw";
    r.status = "Game over";
    return r;
  }

  if (r.up_to_date && a.has_score) {
    r.has_eval = true;
    r.score_cp = a.score_cp;
    r.is_mate = a.is_mate;
    r.mate_in = a.mate_in;
    r.eval_text = assistant::format_eval(a.score_cp, a.is_mate, a.mate_in);
    r.white_win = assistant::white_win_probability(a.score_cp, a.is_mate, a.mate_in);
    r.user_win = assistant::user_win_probability(a.score_cp, a.is_mate,
                                                 a.mate_in, user);
    const int white_cp{a.is_mate ? (a.mate_in > 0 ? 10000 : -10000) : a.score_cp};
    r.user_cp = user == PieceColor::Black ? -white_cp : white_cp;

    if (!a.best_move.empty()) {
      r.best_move_lan = a.best_move;
      r.pv_san = board.lan_line_to_san(a.pv.empty() ? std::vector{a.best_move} : a.pv, 10);
      r.best_move_san = r.pv_san.empty() ? a.best_move : r.pv_san.front();
      r.best_from = square_from_name(std::string_view{a.best_move}.substr(0, 2));
      r.best_to = square_from_name(std::string_view{a.best_move}.substr(2, 2));
    }
  }

  if (!board.has_selected_side()) {
    r.headline = "Choose your side to start";
  } else if (!r.best_move_san.empty()) {
    r.headline = r.best_for_user ? "Your move: play " + r.best_move_san
                                 : "Opponent's best reply: " + r.best_move_san;
  } else {
    r.headline = r.best_for_user ? "Your move" : "Waiting for opponent";
  }

  if (!r.engine_ok) {
    r.status = a.error.empty() ? "Engine offline" : a.error;
  } else if (a.status == EngineAnalysis::Status::Starting) {
    r.status = "Starting engine...";
  } else if (r.searching) {
    r.status = "Analysing - depth " + std::to_string(a.depth);
  } else if (r.complete) {
    r.status = "Depth " + std::to_string(a.depth) + " - " + a.engine_name;
  } else {
    r.status = "Analysing...";
  }
  return r;
}

// ===========================================================================
// HUD
// ===========================================================================

namespace {
// Palette: graphite glass panels, warm ivory text, mint for "winning",
// amber for the recommended move, coral for errors.
constexpr ImVec4 k_text{0.925F, 0.910F, 0.890F, 1.0F};
constexpr ImVec4 k_muted{0.545F, 0.560F, 0.590F, 1.0F};
constexpr ImVec4 k_panel{0.050F, 0.053F, 0.062F, 0.84F};
constexpr ImVec4 k_border{1.0F, 1.0F, 1.0F, 0.07F};
constexpr ImVec4 k_mint{0.275F, 0.878F, 0.627F, 1.0F};
constexpr ImVec4 k_amber{0.957F, 0.757F, 0.306F, 1.0F};
constexpr ImVec4 k_coral{1.0F, 0.420F, 0.420F, 1.0F};

ImU32 col(const ImVec4& c, float alpha_mul = 1.0F) {
  return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * alpha_mul});
}

constexpr ImGuiWindowFlags k_panel_flags{
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing};

void label(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, k_muted);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

// Letter-spaced caps, used for section labels.
void caps(const char* text, const ImVec4& color = k_muted) {
  ImDrawList* dl{ImGui::GetWindowDrawList()};
  ImVec2 p{ImGui::GetCursorScreenPos()};
  const float size{ImGui::GetFontSize() * 0.78F};
  const float spacing{size * 0.16F};
  ImFont* font{ImGui::GetFont()};
  float x{p.x};
  for (const char* c = text; *c != '\0'; ++c) {
    const char s[2]{*c, '\0'};
    dl->AddText(font, size, {x, p.y}, col(color), s);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0F, s).x + spacing;
  }
  ImGui::Dummy({x - p.x, size + 2.0F});
}
}  // namespace

bool WinningAssistantHUD::init(GLFWwindow* window, const std::string& font_dir) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io{ImGui::GetIO()};
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  const std::string ui{font_dir + "/Roboto-Medium.ttf"};
  const std::string mono{font_dir + "/Cousine-Regular.ttf"};
  if (std::filesystem::exists(ui)) {
    ui_font_ = io.Fonts->AddFontFromFileTTF(ui.c_str(), 16.0F);
  }
  if (std::filesystem::exists(mono)) {
    mono_font_ = io.Fonts->AddFontFromFileTTF(mono.c_str(), 15.0F);
  }
  if (ui_font_ == nullptr) {
    ui_font_ = io.Fonts->AddFontDefault();
  }
  if (mono_font_ == nullptr) {
    mono_font_ = ui_font_;
  }
  io.FontDefault = ui_font_;

  apply_style();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");
  initialized_ = true;
  log("Chess Assist ready. Choose a side, then type moves like d4, Nf3, O-O.");
  log("Commands: new, undo, side, flip, auto, scene, help  (Cmd/Ctrl+N = new game)");
  return true;
}

void WinningAssistantHUD::shutdown() {
  if (!initialized_) {
    return;
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  initialized_ = false;
}

void WinningAssistantHUD::apply_style() {
  ImGuiStyle& s{ImGui::GetStyle()};
  s.WindowRounding = 14.0F;
  s.ChildRounding = 10.0F;
  s.FrameRounding = 9.0F;
  s.PopupRounding = 10.0F;
  s.GrabRounding = 8.0F;
  s.ScrollbarRounding = 8.0F;
  s.WindowBorderSize = 1.0F;
  s.FrameBorderSize = 0.0F;
  s.WindowPadding = {18.0F, 16.0F};
  s.FramePadding = {12.0F, 8.0F};
  s.ItemSpacing = {10.0F, 8.0F};
  s.ScrollbarSize = 8.0F;
  s.WindowMinSize = {10.0F, 10.0F};

  ImVec4* c{s.Colors};
  c[ImGuiCol_Text] = k_text;
  c[ImGuiCol_TextDisabled] = k_muted;
  c[ImGuiCol_WindowBg] = k_panel;
  c[ImGuiCol_ChildBg] = {0.0F, 0.0F, 0.0F, 0.0F};
  c[ImGuiCol_PopupBg] = {0.06F, 0.064F, 0.074F, 0.97F};
  c[ImGuiCol_Border] = k_border;
  c[ImGuiCol_FrameBg] = {1.0F, 1.0F, 1.0F, 0.045F};
  c[ImGuiCol_FrameBgHovered] = {1.0F, 1.0F, 1.0F, 0.075F};
  c[ImGuiCol_FrameBgActive] = {1.0F, 1.0F, 1.0F, 0.095F};
  c[ImGuiCol_Button] = {1.0F, 1.0F, 1.0F, 0.06F};
  c[ImGuiCol_ButtonHovered] = {1.0F, 1.0F, 1.0F, 0.11F};
  c[ImGuiCol_ButtonActive] = {1.0F, 1.0F, 1.0F, 0.16F};
  c[ImGuiCol_CheckMark] = k_mint;
  c[ImGuiCol_SliderGrab] = k_mint;
  c[ImGuiCol_Header] = {1.0F, 1.0F, 1.0F, 0.05F};
  c[ImGuiCol_HeaderHovered] = {1.0F, 1.0F, 1.0F, 0.08F};
  c[ImGuiCol_Separator] = k_border;
  c[ImGuiCol_ScrollbarBg] = {0.0F, 0.0F, 0.0F, 0.0F};
  c[ImGuiCol_ScrollbarGrab] = {1.0F, 1.0F, 1.0F, 0.10F};
  c[ImGuiCol_TableRowBgAlt] = {1.0F, 1.0F, 1.0F, 0.025F};
  c[ImGuiCol_TableBorderLight] = {0.0F, 0.0F, 0.0F, 0.0F};
  c[ImGuiCol_NavCursor] = {0.0F, 0.0F, 0.0F, 0.0F};
  c[ImGuiCol_ModalWindowDimBg] = {0.0F, 0.0F, 0.0F, 0.55F};
}

void WinningAssistantHUD::begin_frame() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGuiIO& io{ImGui::GetIO()};
  if (fixed_dt_ > 0.0F) {
    io.DeltaTime = fixed_dt_;  // deterministic animation when recording
  }
  if (ui_scale_ != 1.0F) {
    // Lay the HUD out at a logical resolution and render it at ui_scale x,
    // exactly like a Retina display (used for 4K captures on plain X11).
    io.DisplaySize.x /= ui_scale_;
    io.DisplaySize.y /= ui_scale_;
    io.DisplayFramebufferScale = {io.DisplayFramebufferScale.x * ui_scale_,
                                  io.DisplayFramebufferScale.y * ui_scale_};
  }
  ImGui::NewFrame();
}

void WinningAssistantHUD::end_frame() {
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool WinningAssistantHUD::wants_mouse() const {
  return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool WinningAssistantHUD::wants_keyboard() const {
  return initialized_ && ImGui::GetIO().WantTextInput;
}

void WinningAssistantHUD::log(std::string text, bool error) {
  log_.push_back({std::move(text), error});
  while (log_.size() > 60) {
    log_.pop_front();
  }
}

HudActions WinningAssistantHUD::draw(const AssistantReport& report,
                                     const BoardManager& board,
                                     HudOptions& options) {
  HudActions actions;
  const ImGuiIO& io{ImGui::GetIO()};
  const float w{io.DisplaySize.x};
  const float h{io.DisplaySize.y};
  const float dt{io.DeltaTime};

  // Ease displayed values towards the latest evaluation.
  if (report.has_eval) {
    const float k{1.0F - std::exp(-dt * 6.0F)};
    shown_user_win_ += (static_cast<float>(report.user_win) - shown_user_win_) * k;
    shown_eval_ += (static_cast<float>(std::clamp(report.user_cp, -1500, 1500)) - shown_eval_) * k;
  }
  if (!report.best_move_san.empty() &&
      (report.best_move_san != last_best_san_ ||
       report.best_for_user != last_best_for_user_)) {
    last_best_san_ = report.best_move_san;
    last_best_for_user_ = report.best_for_user;
    best_flash_ = 1.0F;
  }
  best_flash_ = std::max(0.0F, best_flash_ - dt * 1.6F);

  side_chosen_ = board.has_selected_side();
  draw_brand(board);
  const float right_w{std::clamp(w * 0.25F, 300.0F, 360.0F)};
  draw_history(board, actions, h);
  draw_assistant(report, board, options, actions, right_w);
  draw_terminal(actions, w - right_w - 60.0F, h);
  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F3)) {
    options.show_perf = !options.show_perf;
  }
  if (options.show_perf) {
    draw_perf(options);
  }
  if (board.has_selected_side() && board.is_game_over()) {
    draw_game_over(board, actions);
  }
  if (board.has_selected_side()) {
    draw_new_game_confirm(board, actions);
  }
  if (!board.has_selected_side()) {
    draw_side_selection(actions, w, h);
  }
  if (demo_cursor_) {
    // Synthetic arrow cursor for recorded demos.
    ImDrawList* fg{ImGui::GetForegroundDrawList()};
    const ImVec2 c{(*demo_cursor_)[0], (*demo_cursor_)[1]};
    const float k{demo_pressed_ ? 0.88F : 1.0F};
    const ImVec2 pts[7]{{c.x, c.y},
                        {c.x, c.y + 22.0F * k},
                        {c.x + 5.5F * k, c.y + 17.0F * k},
                        {c.x + 9.5F * k, c.y + 26.0F * k},
                        {c.x + 13.0F * k, c.y + 24.5F * k},
                        {c.x + 9.0F * k, c.y + 15.5F * k},
                        {c.x + 16.0F * k, c.y + 15.5F * k}};
    fg->AddPolyline(pts, 7, col({0.0F, 0.0F, 0.0F, 0.9F}), ImDrawFlags_Closed, 3.0F);
    fg->AddConcavePolyFilled(pts, 7, col({1.0F, 1.0F, 1.0F, 1.0F}));
    fg->AddPolyline(pts, 7, col({0.05F, 0.05F, 0.06F, 1.0F}), ImDrawFlags_Closed, 1.4F);
    if (demo_pressed_) {
      fg->AddCircle(c, 18.0F, col(k_mint, 0.9F), 32, 2.5F);
    }
  }
  return actions;
}

void WinningAssistantHUD::draw_brand(const BoardManager& board) {
  ImGui::SetNextWindowPos({22.0F, 14.0F});
  ImGui::SetNextWindowSizeConstraints({236.0F, 0.0F}, {640.0F, FLT_MAX});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0F, 10.0F});
  ImGui::Begin("##brand", nullptr,
               k_panel_flags | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
  ImGui::PopStyleVar();
  ImGui::PushFont(ui_font_, 22.0F);
  ImGui::TextUnformatted("Chess Assist");
  ImGui::PopFont();
  ImGui::SameLine(0.0F, 1.0F);
  ImGui::PushStyleColor(ImGuiCol_Text, k_mint);
  ImGui::PushFont(ui_font_, 22.0F);
  ImGui::TextUnformatted(".");
  ImGui::PopFont();
  ImGui::PopStyleColor();

  std::string sub{board.status_text()};
  if (board.has_selected_side()) {
    sub += board.user_side() == PieceColor::White ? "  |  You: White" : "  |  You: Black";
  }
  ImGui::PushStyleColor(ImGuiCol_Text,
                        board.board().is_in_check() ? k_coral : k_muted);
  ImGui::TextUnformatted(sub.c_str());
  ImGui::PopStyleColor();
  ImGui::End();
}

void WinningAssistantHUD::draw_side_selection(HudActions& actions, float w,
                                              float h) {
  // Full-screen dim + centred card.
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({w, h});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.0F, 0.0F, 0.0F, 0.45F});
  ImGui::Begin("##dim", nullptr, k_panel_flags | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  const ImVec2 card{440.0F, 300.0F};
  ImGui::SetNextWindowPos({(w - card.x) * 0.5F, (h - card.y) * 0.5F});
  ImGui::SetNextWindowSize(card);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32.0F, 28.0F});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.06F, 0.064F, 0.075F, 0.97F});
  ImGui::Begin("##side", nullptr, k_panel_flags | ImGuiWindowFlags_NoScrollbar);
  caps("NEW GAME", k_mint);
  ImGui::PushFont(ui_font_, 30.0F);
  ImGui::TextUnformatted("Choose your side");
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, k_muted);
  ImGui::PushTextWrapPos(card.x - 32.0F);
  ImGui::TextUnformatted(
      "The board turns so your pieces are at the bottom. Enter both "
      "players' moves and the assistant suggests your best reply.");
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Dummy({0, 10});

  const float bw{(card.x - 64.0F - 14.0F) * 0.5F};
  const auto side_button = [&](const char* id, const char* text, bool white) {
    ImGui::PushID(id);
    const ImVec2 p{ImGui::GetCursorScreenPos()};
    const bool clicked{ImGui::InvisibleButton("btn", {bw, 92.0F})};
    const ImVec2 q{p.x + bw, p.y + 92.0F};
    const bool demo_hover{demo_cursor_ && (*demo_cursor_)[0] >= p.x && (*demo_cursor_)[0] <= q.x &&
                          (*demo_cursor_)[1] >= p.y && (*demo_cursor_)[1] <= q.y};
    const bool hovered{ImGui::IsItemHovered() || demo_hover};
    if (white) {
      white_btn_ = {(p.x + q.x) * 0.5F, (p.y + q.y) * 0.5F};
    }
    ImDrawList* dl{ImGui::GetWindowDrawList()};
    const ImVec4 face{white ? ImVec4{0.93F, 0.91F, 0.87F, 1.0F}
                            : ImVec4{0.10F, 0.105F, 0.12F, 1.0F}};
    dl->AddRectFilled(p, q, col(face), 12.0F);
    dl->AddRect(p, q, hovered ? col(k_mint) : col(k_border, 2.0F), 12.0F, 0,
                hovered ? 2.0F : 1.0F);
    // A minimal king glyph: circle + cross, drawn with primitives.
    const ImVec2 c{p.x + bw * 0.5F, p.y + 36.0F};
    const ImU32 ink{white ? col({0.10F, 0.10F, 0.12F, 1.0F}) : col(k_text)};
    dl->AddCircle(c, 13.0F, ink, 32, 2.0F);
    dl->AddLine({c.x, c.y - 9.0F}, {c.x, c.y + 6.0F}, ink, 2.0F);
    dl->AddLine({c.x - 6.0F, c.y - 3.0F}, {c.x + 6.0F, c.y - 3.0F}, ink, 2.0F);
    const ImVec2 ts{ImGui::CalcTextSize(text)};
    dl->AddText({c.x - ts.x * 0.5F, p.y + 62.0F}, ink, text);
    ImGui::PopID();
    return clicked;
  };
  if (side_button("white", "Play White", true)) {
    actions.select_side = PieceColor::White;
  }
  ImGui::SameLine(0.0F, 14.0F);
  if (side_button("black", "Play Black", false)) {
    actions.select_side = PieceColor::Black;
  }
  ImGui::Dummy({0, 4});
  ImGui::PushStyleColor(ImGuiCol_Text, k_muted);
  ImGui::TextUnformatted("Shortcut: press W or B");
  ImGui::PopStyleColor();
  if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_W)) {
    actions.select_side = PieceColor::White;
  }
  if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_B)) {
    actions.select_side = PieceColor::Black;
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::End();
}

void WinningAssistantHUD::draw_history(const BoardManager& board, HudActions& actions, float h) {
  const float top{92.0F};
  const float bottom_reserve{196.0F};
  ImGui::SetNextWindowPos({22.0F, top});
  ImGui::SetNextWindowSize({236.0F, std::max(160.0F, h - top - bottom_reserve)});
  ImGui::Begin("##history", nullptr, k_panel_flags);
  caps("MOVES");
  ImGui::Separator();
  const auto turns{board.history_turns()};
  // Leave room for the game-control buttons at the bottom of the panel.
  const float controls_h{ImGui::GetFrameHeight() * 2.0F + ImGui::GetStyle().ItemSpacing.y * 2.0F + 8.0F};
  ImGui::BeginChild("##hist_scroll", {0, -controls_h});
  if (turns.empty()) {
    ImGui::Dummy({0, 6});
    label("No moves yet.");
  }
  if (ImGui::BeginTable("##moves", 3, ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0F);
    ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
    const size_t total{board.history().size()};
    for (size_t i = 0; i < turns.size(); ++i) {
      ImGui::TableNextRow(0, 26.0F);
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, k_muted);
      ImGui::Text("%d.", turns[i].number);
      ImGui::PopStyleColor();
      const bool last_row{i + 1 == turns.size()};
      for (int c = 0; c < 2; ++c) {
        ImGui::TableNextColumn();
        const std::string& m{c == 0 ? turns[i].white : turns[i].black};
        const bool is_latest{last_row && ((c == 1) == (total % 2 == 0))};
        ImGui::PushStyleColor(ImGuiCol_Text, is_latest ? k_mint : k_text);
        ImGui::PushFont(mono_font_, 15.0F);
        ImGui::TextUnformatted(m.empty() ? (c == 0 ? "..." : "") : m.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndTable();
  }
  if (board.history().size() != last_history_size_) {
    last_history_size_ = board.history().size();
    ImGui::SetScrollHereY(1.0F);
  }
  ImGui::EndChild();

  // --- Game controls -------------------------------------------------------
  ImGui::Separator();
  const float bw{(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F};
  ImGui::PushStyleColor(ImGuiCol_Button, {k_mint.x, k_mint.y, k_mint.z, 0.85F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_mint);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.20F, 0.70F, 0.50F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_Text, {0.03F, 0.07F, 0.05F, 1.0F});
  if (ImGui::Button("New game", {bw, 0})) {
    request_new_game_ = true;
  }
  ImGui::PopStyleColor(4);
  ImGui::SameLine();
  ImGui::BeginDisabled(board.history().empty());
  if (ImGui::Button("Undo", {bw, 0})) {
    actions.command = "undo";
  }
  ImGui::EndDisabled();
  if (ImGui::Button("Change side", {-1.0F, 0})) {
    actions.command = "side";
  }
  ImGui::End();
}

void WinningAssistantHUD::draw_new_game_confirm(const BoardManager& board, HudActions& actions) {
  const ImGuiIO& io{ImGui::GetIO()};
  // Cmd/Ctrl+N also starts a new game.
  if (!io.WantTextInput && io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_N)) {
    request_new_game_ = true;
  }
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
    request_new_game_ = true;
  }
  if (request_new_game_) {
    request_new_game_ = false;
    // Nothing to lose (no moves or game over): restart straight away.
    if (board.history().empty() || board.is_game_over()) {
      actions.command = "reset";
    } else {
      ImGui::OpenPopup("##confirm_new");
    }
  }
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5F, io.DisplaySize.y * 0.42F}, ImGuiCond_Always,
                          {0.5F, 0.5F});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0F, 20.0F});
  if (ImGui::BeginPopupModal("##confirm_new", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    caps("NEW GAME", k_mint);
    ImGui::PushFont(ui_font_, 22.0F);
    ImGui::TextUnformatted("Start a new game?");
    ImGui::PopFont();
    label("The current moves will be cleared. Your side stays the same.");
    ImGui::Dummy({0, 6});
    if (ImGui::Button("New game", {150, 0}) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      actions.command = "reset";
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {150, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void WinningAssistantHUD::draw_game_over(const BoardManager& board, HudActions& actions) {
  const ImGuiIO& io{ImGui::GetIO()};
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5F, 22.0F}, ImGuiCond_Always, {0.5F, 0.0F});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22.0F, 14.0F});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.06F, 0.064F, 0.075F, 0.95F});
  ImGui::PushStyleColor(ImGuiCol_Border, {k_amber.x, k_amber.y, k_amber.z, 0.6F});
  ImGui::Begin("##game_over", nullptr, k_panel_flags | ImGuiWindowFlags_AlwaysAutoResize);
  caps("GAME OVER", k_amber);
  ImGui::PushFont(ui_font_, 24.0F);
  ImGui::TextUnformatted(board.status_text().c_str());
  ImGui::PopFont();
  ImGui::Dummy({0, 2});
  if (ImGui::Button("New game", {140, 0})) {
    actions.command = "reset";
  }
  ImGui::SameLine();
  if (ImGui::Button("Undo last move", {140, 0})) {
    actions.command = "undo";
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void WinningAssistantHUD::draw_assistant(const AssistantReport& r,
                                         const BoardManager& board,
                                         HudOptions& options,
                                         HudActions& actions, float w) {
  const ImGuiIO& io{ImGui::GetIO()};
  ImGui::SetNextWindowPos({io.DisplaySize.x - w - 22.0F, 18.0F});
  ImGui::SetNextWindowSize({w, 0.0F});
  ImGui::Begin("##assistant", nullptr,
               k_panel_flags | ImGuiWindowFlags_AlwaysAutoResize);
  caps("WINNING ASSISTANT", k_mint);
  ImGui::Dummy({0, 2});

  // --- Win probability, big number + glide bar.
  label(board.has_selected_side()
            ? (board.user_side() == PieceColor::White ? "Your win chance (White)"
                                                      : "Your win chance (Black)")
            : "Win chance");
  const float pct{shown_user_win_ * 100.0F};
  const ImVec4 pct_col{shown_user_win_ >= 0.55F   ? k_mint
                       : shown_user_win_ <= 0.45F ? k_coral
                                                  : k_text};
  ImGui::PushFont(ui_font_, 46.0F);
  ImGui::PushStyleColor(ImGuiCol_Text, pct_col);
  ImGui::Text("%.1f%%", static_cast<double>(pct));
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Dummy({0, 8});
  ImGui::PushFont(mono_font_, 17.0F);
  ImGui::TextUnformatted(r.has_eval ? r.eval_text.c_str() : "--");
  ImGui::PopFont();
  label("eval (White)");
  ImGui::EndGroup();

  {
    ImDrawList* dl{ImGui::GetWindowDrawList()};
    const ImVec2 p{ImGui::GetCursorScreenPos()};
    const float bw{ImGui::GetContentRegionAvail().x};
    const float bh{8.0F};
    dl->AddRectFilled(p, {p.x + bw, p.y + bh}, col({1, 1, 1, 0.07F}), 4.0F);
    const float fill{std::clamp(shown_user_win_, 0.0F, 1.0F) * bw};
    dl->AddRectFilledMultiColor(p, {p.x + fill, p.y + bh}, col(k_mint, 0.55F),
                                col(k_mint), col(k_mint), col(k_mint, 0.55F));
    dl->AddLine({p.x + bw * 0.5F, p.y - 3.0F}, {p.x + bw * 0.5F, p.y + bh + 3.0F},
                col(k_text, 0.35F), 1.0F);
    ImGui::Dummy({bw, bh + 6.0F});
  }

  // --- Recommended move card.
  ImGui::Dummy({0, 4});
  {
    const ImVec2 p{ImGui::GetCursorScreenPos()};
    const float cw{ImGui::GetContentRegionAvail().x};
    const float ch{96.0F};
    ImDrawList* dl{ImGui::GetWindowDrawList()};
    const ImVec4 accent{r.best_for_user ? k_amber : k_muted};
    dl->AddRectFilled(p, {p.x + cw, p.y + ch},
                      col({accent.x, accent.y, accent.z, 0.08F + best_flash_ * 0.18F}), 12.0F);
    dl->AddRect(p, {p.x + cw, p.y + ch}, col(accent, 0.45F + best_flash_ * 0.5F), 12.0F, 0, 1.2F);
    ImGui::SetCursorScreenPos({p.x + 16.0F, p.y + 12.0F});
    ImGui::BeginGroup();
    caps(r.best_for_user ? "BEST MOVE FOR YOU" : "OPPONENT'S BEST REPLY", accent);
    ImGui::PushFont(ui_font_, 38.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, r.best_for_user ? k_amber : k_text);
    if (!options.show_hints && r.best_for_user) {
      ImGui::TextUnformatted("hidden");
    } else {
      const char* best{r.best_move_san.empty() ? (board.is_game_over() ? "-" : "...")
                                               : r.best_move_san.c_str()};
      ImGui::TextUnformatted(best);
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    if (!r.best_move_lan.empty()) {
      const std::string sq{square_name(r.best_from) + "  ->  " + square_name(r.best_to)};
      const ImVec2 ts{ImGui::CalcTextSize(sq.c_str())};
      dl->AddText({p.x + cw - ts.x - 16.0F, p.y + ch - ts.y - 14.0F}, col(k_muted),
                  sq.c_str());
    }
    ImGui::SetCursorScreenPos({p.x, p.y + ch + 8.0F});
  }

  // --- Principal variation.
  if (r.pv_san.size() > 1) {
    label("Expected line");
    ImGui::PushFont(mono_font_, 14.0F);
    ImGui::PushTextWrapPos(0.0F);
    std::string line;
    int ply{static_cast<int>(board.history().size())};
    for (size_t i = 0; i < r.pv_san.size(); ++i, ++ply) {
      if (ply % 2 == 0) {
        line += std::to_string(ply / 2 + 1) + ". ";
      } else if (i == 0) {
        line += std::to_string(ply / 2 + 1) + "... ";
      }
      line += r.pv_san[i] + " ";
    }
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
  }

  // --- Engine status line.
  ImGui::Dummy({0, 2});
  ImGui::Separator();
  {
    ImDrawList* dl{ImGui::GetWindowDrawList()};
    const ImVec2 p{ImGui::GetCursorScreenPos()};
    const float t{static_cast<float>(ImGui::GetTime())};
    const ImVec4 dot{!r.engine_ok ? k_coral : r.searching ? k_amber : k_mint};
    const float pulse{r.searching ? 0.55F + 0.45F * std::sin(t * 6.0F) : 1.0F};
    dl->AddCircleFilled({p.x + 5.0F, p.y + 9.0F}, 4.0F, col(dot, pulse));
    ImGui::SetCursorScreenPos({p.x + 16.0F, p.y});
    ImGui::PushStyleColor(ImGuiCol_Text, r.engine_ok ? k_muted : k_coral);
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(r.status.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }

  // --- Options.
  ImGui::Dummy({0, 2});
  bool autoplay{options.autoplay_opponent};
  if (ImGui::Checkbox("Stockfish plays opponent", &autoplay)) {
    actions.toggle_autoplay = true;
  }
  ImGui::Checkbox("Show hints", &options.show_hints);
  {
    int scene{options.scene == SceneStyle::Gallery ? 0 : 1};
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::Combo("##scene", &scene, "Environment: Gallery hall\0Environment: Dark studio\0")) {
      options.scene = scene == 0 ? SceneStyle::Gallery : SceneStyle::Studio;
    }
  }
  ImGui::Checkbox("Depth of field", &options.dof);
  ImGui::SameLine();
  ImGui::Checkbox("SSAO", &options.ssao);
  ImGui::PushStyleColor(ImGuiCol_Text, k_muted);
  float gpu{};
  for (const float v : options.gpu_ms) {
    gpu += v;
  }
  if (!demo_) {
    ImGui::Text("%.0f fps  |  GPU %.1f ms  |  CPU %.1f ms", static_cast<double>(options.fps),
                static_cast<double>(gpu), static_cast<double>(options.cpu_ms));
  }
  ImGui::TextUnformatted("Right-drag orbit  |  C reset  |  F3 profiler");
  ImGui::PopStyleColor();
  ImGui::End();
}

void WinningAssistantHUD::draw_perf(const HudOptions& options) {
  static constexpr std::array<const char*, 6> k_labels{
      "Picking ids", "Shadow map", "Reflection", "Scene (MSAA HDR)", "Post (SSAO/DOF)", "HUD"};
  const ImGuiIO& io{ImGui::GetIO()};
  ImGui::SetNextWindowPos({io.DisplaySize.x - 22.0F, io.DisplaySize.y - 18.0F},
                          ImGuiCond_Always, {1.0F, 1.0F});
  ImGui::SetNextWindowSize({300.0F, 0.0F});
  ImGui::Begin("##perf", nullptr, k_panel_flags | ImGuiWindowFlags_AlwaysAutoResize);
  caps("GPU PROFILER", k_amber);
  float total{};
  for (const float v : options.gpu_ms) {
    total += v;
  }
  const float budget{16.67F};
  ImDrawList* dl{ImGui::GetWindowDrawList()};
  ImGui::PushFont(mono_font_, 14.0F);
  for (size_t i = 0; i < k_labels.size(); ++i) {
    const float v{options.gpu_ms[i]};
    ImGui::TextUnformatted(k_labels[i]);
    ImGui::SameLine(170.0F);
    ImGui::Text("%5.2f ms", static_cast<double>(v));
    const ImVec2 p{ImGui::GetCursorScreenPos()};
    const float bw{ImGui::GetContentRegionAvail().x};
    dl->AddRectFilled(p, {p.x + bw, p.y + 3.0F}, col({1, 1, 1, 0.06F}), 2.0F);
    dl->AddRectFilled(p, {p.x + bw * std::clamp(v / budget, 0.0F, 1.0F), p.y + 3.0F},
                      col(v > budget * 0.5F ? k_coral : k_mint), 2.0F);
    ImGui::Dummy({bw, 5.0F});
  }
  ImGui::Separator();
  ImGui::Text("GPU total %6.2f ms", static_cast<double>(total));
  ImGui::Text("CPU frame %6.2f ms", static_cast<double>(options.cpu_ms));
  ImGui::Text("%.0f fps (vsync)", static_cast<double>(options.fps));
  ImGui::PopFont();
  ImGui::End();
}

namespace {
int terminal_callback(ImGuiInputTextCallbackData* data) {
  auto* self{static_cast<std::pair<std::vector<std::string>*, int*>*>(data->UserData)};
  auto& history{*self->first};
  int& pos{*self->second};
  if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || history.empty()) {
    return 0;
  }
  const int prev{pos};
  if (data->EventKey == ImGuiKey_UpArrow) {
    pos = pos < 0 ? static_cast<int>(history.size()) - 1 : std::max(0, pos - 1);
  } else if (data->EventKey == ImGuiKey_DownArrow && pos >= 0) {
    pos = pos + 1 >= static_cast<int>(history.size()) ? -1 : pos + 1;
  }
  if (prev != pos) {
    data->DeleteChars(0, data->BufTextLen);
    if (pos >= 0) {
      data->InsertChars(0, history[static_cast<size_t>(pos)].c_str());
    }
  }
  return 0;
}
}  // namespace

void WinningAssistantHUD::draw_terminal(HudActions& actions, float w, float h) {
  const float th{170.0F};
  ImGui::SetNextWindowPos({22.0F, h - th - 18.0F});
  ImGui::SetNextWindowSize({std::max(w, 360.0F), th});
  ImGui::Begin("##terminal", nullptr, k_panel_flags | ImGuiWindowFlags_NoScrollbar);
  caps("MOVE TERMINAL");
  ImGui::BeginChild("##log", {0, th - 96.0F});
  ImGui::PushFont(mono_font_, 14.0F);
  for (const auto& line : log_) {
    ImGui::PushStyleColor(ImGuiCol_Text, line.error ? k_coral : k_muted);
    ImGui::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
    ImGui::SetScrollHereY(1.0F);
  }
  ImGui::PopFont();
  ImGui::EndChild();

  ImGui::PushFont(mono_font_, 17.0F);
  ImGui::PushStyleColor(ImGuiCol_Text, k_mint);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(">");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0F);
  if (demo_) {
    // Scripted typing: show the text (with a blinking caret) in the box.
    const bool caret{std::fmod(ImGui::GetTime(), 1.0) < 0.55 && side_chosen_};
    std::snprintf(input_, sizeof(input_), "%s%s", demo_input_.c_str(), caret ? "|" : "");
    focus_terminal_ = false;
  }
  if (focus_terminal_ && side_chosen_) {
    ImGui::SetKeyboardFocusHere();
    focus_terminal_ = false;
  }
  std::pair<std::vector<std::string>*, int*> cb_data{&input_history_, &input_history_pos_};
  const bool submitted{ImGui::InputTextWithHint(
      "##cmd", "Enter a move (d4, Nf3, exd5, O-O) or a command",
      input_, sizeof(input_),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
      terminal_callback, &cb_data)};
  ImGui::PopFont();
  if (submitted) {
    std::string text{input_};
    input_[0] = '\0';
    input_history_pos_ = -1;
    if (!text.empty()) {
      input_history_.push_back(text);
      actions.command = text;
    }
    focus_terminal_ = true;  // keep typing moves without clicking
  }
  ImGui::End();
}
