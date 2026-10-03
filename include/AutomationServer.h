#pragma once
// AutomationServer.h — tiny loopback HTTP server for automation & remote view.
//
// Playwright can't click inside a native OpenGL window, so the app exposes a
// small control surface on http://127.0.0.1:<port>:
//
//   GET  /                     web console mirroring the HUD (Playwright target)
//   GET  /api/state            full JSON snapshot (board, history, assistant)
//   POST /api/side             {"side":"white"|"black"}
//   POST /api/move             {"san":"Nf3"}
//   POST /api/command          {"command":"undo"}   (same as the HUD terminal)
//   POST /api/reset            {"clearSide":true}
//   GET  /api/screenshot.png   PNG of the live 3D frame (windowed mode only)
//
// It only binds to 127.0.0.1. Requests are served one at a time on a
// background thread; the handler is responsible for thread safety.

#include <atomic>
#include <cstdint>
#include <string_view>
#include <functional>
#include <string>
#include <thread>

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
};

struct HttpResponse {
  int status{200};
  std::string content_type{"application/json"};
  std::string body;
};

class AutomationServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  AutomationServer() = default;
  ~AutomationServer() { stop(); }
  AutomationServer(const AutomationServer&) = delete;
  AutomationServer& operator=(const AutomationServer&) = delete;
  AutomationServer(AutomationServer&&) = delete;
  AutomationServer& operator=(AutomationServer&&) = delete;

  bool start(int port, Handler handler);
  void stop();
  [[nodiscard]] int port() const { return port_; }
  [[nodiscard]] bool running() const { return running_; }

 private:
  void serve();
  void handle_client(std::intptr_t client);

  Handler handler_;
  std::intptr_t listen_socket_{-1};
  int port_{};
  std::atomic<bool> running_{};
  std::thread thread_;
};

// --- small JSON helpers shared with the game -------------------------------
std::string json_escape(std::string_view s);
// Extracts a top-level string or boolean value from a flat JSON object.
// Returns "" when missing. Good enough for the tiny request bodies above.
std::string json_get(std::string_view body, std::string_view key);
