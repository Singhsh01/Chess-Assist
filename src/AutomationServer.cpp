#include "AutomationServer.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "log.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
namespace {
void close_socket(std::intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
int poll_socket(std::intptr_t s, int timeout_ms) {
  WSAPOLLFD pfd{static_cast<SOCKET>(s), POLLRDNORM, 0};
  return WSAPoll(&pfd, 1, timeout_ms);
}
}  // namespace
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
void close_socket(std::intptr_t s) { ::close(static_cast<int>(s)); }
int poll_socket(std::intptr_t s, int timeout_ms) {
  pollfd pfd{static_cast<int>(s), POLLIN, 0};
  return ::poll(&pfd, 1, timeout_ms);
}
}  // namespace
#endif

bool AutomationServer::start(int port, Handler handler) {
  stop();
  handler_ = std::move(handler);
#ifdef _WIN32
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const auto s{static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, 0))};
  if (s < 0) {
    LOG("HTTP", "socket() failed");
    return false;
  }
  int yes{1};
  setsockopt(static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifndef _WIN32
  // Don't leak the listening socket into the Stockfish child process.
  fcntl(static_cast<int>(s), F_SETFD, FD_CLOEXEC);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(static_cast<int>(s), 16) != 0) {
    LOGF("HTTP", "Cannot listen on 127.0.0.1:{} (port in use?)", port);
    close_socket(s);
    return false;
  }
  listen_socket_ = s;
  port_ = port;
  running_ = true;
  thread_ = std::thread{&AutomationServer::serve, this};
  LOGF("HTTP", "Automation console on http://127.0.0.1:{}/", port);
  return true;
}

void AutomationServer::stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
  if (listen_socket_ >= 0) {
    close_socket(listen_socket_);
    listen_socket_ = -1;
  }
}

void AutomationServer::serve() {
  while (running_) {
    if (poll_socket(listen_socket_, 100) <= 0) {
      continue;
    }
    const auto client{static_cast<std::intptr_t>(
        ::accept(static_cast<int>(listen_socket_), nullptr, nullptr))};
    if (client < 0) {
      continue;
    }
    handle_client(client);
    close_socket(client);
  }
}

namespace {
#if defined(MSG_NOSIGNAL)
constexpr int k_send_flags{MSG_NOSIGNAL};
#else
constexpr int k_send_flags{0};  // macOS: SIGPIPE is ignored process-wide in main()
#endif

const char* reason(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 503:
      return "Service Unavailable";
    default:
      return "Error";
  }
}

bool send_all(std::intptr_t s, const std::string& data) {
  size_t off{};
  while (off < data.size()) {
#ifdef _WIN32
    const int n{::send(static_cast<SOCKET>(s), data.data() + off,
                       static_cast<int>(data.size() - off), 0)};
#else
    const ssize_t n{::send(static_cast<int>(s), data.data() + off,
                           data.size() - off, k_send_flags)};
#endif
    if (n <= 0) {
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}
}  // namespace

void AutomationServer::handle_client(std::intptr_t client) {
  std::string raw;
  char buf[4096];
  size_t header_end{std::string::npos};
  size_t content_length{0};
  // Read headers, then the body (bounded, with a poll timeout per chunk).
  while (raw.size() < 1 << 20) {
    if (poll_socket(client, 2000) <= 0) {
      return;
    }
#ifdef _WIN32
    const int n{::recv(static_cast<SOCKET>(client), buf, sizeof(buf), 0)};
#else
    const ssize_t n{::recv(static_cast<int>(client), buf, sizeof(buf), 0)};
#endif
    if (n <= 0) {
      return;
    }
    raw.append(buf, static_cast<size_t>(n));
    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string lower{raw.substr(0, header_end)};
        for (auto& c : lower) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (const auto p = lower.find("content-length:"); p != std::string::npos) {
          content_length = std::strtoul(lower.c_str() + p + 15, nullptr, 10);
        }
      }
    }
    if (header_end != std::string::npos &&
        raw.size() >= header_end + 4 + content_length) {
      break;
    }
  }
  if (header_end == std::string::npos) {
    return;
  }

  HttpRequest req;
  {
    const auto line_end{raw.find("\r\n")};
    const std::string line{raw.substr(0, line_end)};
    const auto sp1{line.find(' ')};
    const auto sp2{line.find(' ', sp1 + 1)};
    req.method = line.substr(0, sp1);
    std::string target{line.substr(sp1 + 1, sp2 - sp1 - 1)};
    if (const auto q = target.find('?'); q != std::string::npos) {
      req.query = target.substr(q + 1);
      target.resize(q);
    }
    req.path = target;
    req.body = raw.substr(header_end + 4, content_length);
  }

  HttpResponse res;
  if (req.method == "OPTIONS") {
    res.status = 204;
    res.content_type = "text/plain";
  } else {
    try {
      res = handler_(req);
    } catch (const std::exception& e) {
      res = {500, "application/json", std::string{"{\"error\":\""} + json_escape(e.what()) + "\"}"};
    }
  }

  std::string head{"HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n"};
  head += "Content-Type: " + res.content_type + "\r\n";
  head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
  head += "Cache-Control: no-store\r\n";
  head += "Access-Control-Allow-Origin: *\r\n";
  head += "Access-Control-Allow-Headers: Content-Type\r\n";
  head += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  head += "Connection: close\r\n\r\n";
  send_all(client, head + res.body);
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", c);
          out += hex;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string json_get(std::string_view body, std::string_view key) {
  const std::string needle{"\"" + std::string{key} + "\""};
  auto p{body.find(needle)};
  if (p == std::string_view::npos) {
    return {};
  }
  p = body.find(':', p + needle.size());
  if (p == std::string_view::npos) {
    return {};
  }
  ++p;
  while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p])) != 0) {
    ++p;
  }
  if (p >= body.size()) {
    return {};
  }
  if (body[p] == '"') {
    std::string out;
    for (++p; p < body.size() && body[p] != '"'; ++p) {
      if (body[p] == '\\' && p + 1 < body.size()) {
        ++p;
      }
      out += body[p];
    }
    return out;
  }
  const auto end{body.find_first_of(",}", p)};
  std::string out{body.substr(p, end - p)};
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) {
    out.pop_back();
  }
  return out;
}
