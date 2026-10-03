#pragma once
// ProcessPipe.h — spawn a child process and talk to it over STDIN/STDOUT pipes.
//
// POSIX (macOS / Linux): pipe() + posix_spawnp(), non-blocking line reads via
// poll(). Windows: CreatePipe() + CreateProcessA(), reads via PeekNamedPipe().
//
// The class is deliberately small and synchronous: it is owned by exactly one
// worker thread (see StockfishEngine), so it has no internal locking.

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
extern char** environ;
#endif

class ProcessPipe {
 public:
  ProcessPipe() = default;
  ~ProcessPipe() { terminate(); }

  ProcessPipe(const ProcessPipe&) = delete;
  ProcessPipe& operator=(const ProcessPipe&) = delete;
  ProcessPipe(ProcessPipe&&) = delete;
  ProcessPipe& operator=(ProcessPipe&&) = delete;

  // Launches `executable` (searched on PATH when it has no slash) with `args`.
  // Returns false and fills `error()` when the process cannot be started.
  bool spawn(const std::string& executable,
             const std::vector<std::string>& args = {}) {
    terminate();
    buffer_.clear();
    eof_ = false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_stdout_w{};
    HANDLE child_stdin_r{};
    if (!CreatePipe(&stdout_r_, &child_stdout_w, &sa, 0) ||
        !SetHandleInformation(stdout_r_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&child_stdin_r, &stdin_w_, &sa, 0) ||
        !SetHandleInformation(stdin_w_, HANDLE_FLAG_INHERIT, 0)) {
      error_ = "CreatePipe failed";
      return false;
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_r;
    si.hStdOutput = child_stdout_w;
    si.hStdError = child_stdout_w;
    std::string cmd{"\"" + executable + "\""};
    for (const auto& a : args) {
      cmd += " " + a;
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok{CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)};
    CloseHandle(child_stdout_w);
    CloseHandle(child_stdin_r);
    if (!ok) {
      error_ = "CreateProcess failed for " + executable;
      close_handles();
      return false;
    }
    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    running_ = true;
    return true;
#else
    int in_pipe[2]{-1, -1};   // parent writes -> child stdin
    int out_pipe[2]{-1, -1};  // child stdout -> parent reads
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
      error_ = "pipe() failed";
      return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);

    std::vector<std::string> argv_store{executable};
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : argv_store) {
      argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid{};
    const int rc{posix_spawnp(&pid, executable.c_str(), &actions, nullptr,
                              argv.data(), environ)};
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (rc != 0) {
      close(in_pipe[1]);
      close(out_pipe[0]);
      error_ = "posix_spawnp failed for '" + executable + "' (errno " +
               std::to_string(rc) + ")";
      return false;
    }

    // Writing to a dead child must not kill the whole app with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);

    pid_ = pid;
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    fcntl(stdout_fd_, F_SETFL, fcntl(stdout_fd_, F_GETFL) | O_NONBLOCK);
    running_ = true;
    return true;
#endif
  }

  // Writes one line (a '\n' is appended). Returns false if the pipe is broken.
  bool write_line(const std::string& line) {
    if (!running_) {
      return false;
    }
    const std::string data{line + "\n"};
#ifdef _WIN32
    DWORD written{};
    return WriteFile(stdin_w_, data.data(), static_cast<DWORD>(data.size()),
                     &written, nullptr) != 0;
#else
    size_t off{};
    while (off < data.size()) {
      const ssize_t n{::write(stdin_fd_, data.data() + off, data.size() - off)};
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        running_ = false;
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
#endif
  }

  // Returns the next complete line, waiting at most `timeout`. std::nullopt on
  // timeout or EOF (check `alive()` to distinguish).
  std::optional<std::string> read_line(std::chrono::milliseconds timeout) {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (true) {
      if (auto line = pop_line()) {
        return line;
      }
      if (eof_) {
        return std::nullopt;
      }
      const auto now{std::chrono::steady_clock::now()};
      if (now >= deadline) {
        return std::nullopt;
      }
      const auto left{std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - now)};
      fill_buffer(left);
    }
  }

  [[nodiscard]] bool alive() const { return running_ && !eof_; }
  [[nodiscard]] const std::string& error() const { return error_; }

  void terminate() {
    if (!running_) {
      close_handles();
      return;
    }
    running_ = false;
#ifdef _WIN32
    TerminateProcess(process_, 0);
    WaitForSingleObject(process_, 1000);
    close_handles();
#else
    close_handles();  // closing stdin makes well-behaved children exit
    int status{};
    for (int i = 0; i < 50; ++i) {  // up to ~500 ms grace period
      if (waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      usleep(10000);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, &status, 0);
    pid_ = -1;
#endif
  }

 private:
  std::optional<std::string> pop_line() {
    const auto pos{buffer_.find('\n')};
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    std::string line{buffer_.substr(0, pos)};
    buffer_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  void fill_buffer(std::chrono::milliseconds wait) {
    char chunk[4096];
#ifdef _WIN32
    const auto deadline{std::chrono::steady_clock::now() + wait};
    do {
      DWORD avail{};
      if (!PeekNamedPipe(stdout_r_, nullptr, 0, nullptr, &avail, nullptr)) {
        eof_ = true;
        return;
      }
      if (avail > 0) {
        DWORD n{};
        if (ReadFile(stdout_r_, chunk,
                     std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) &&
            n > 0) {
          buffer_.append(chunk, n);
          return;
        }
        eof_ = true;
        return;
      }
      Sleep(2);
    } while (std::chrono::steady_clock::now() < deadline);
#else
    pollfd pfd{stdout_fd_, POLLIN, 0};
    const int rc{poll(&pfd, 1, static_cast<int>(wait.count()))};
    if (rc <= 0) {
      return;  // timeout or EINTR
    }
    while (true) {
      const ssize_t n{::read(stdout_fd_, chunk, sizeof(chunk))};
      if (n > 0) {
        buffer_.append(chunk, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        eof_ = true;
      }
      return;  // EAGAIN: drained
    }
#endif
  }

  void close_handles() {
#ifdef _WIN32
    for (HANDLE* h : {&stdin_w_, &stdout_r_, &process_}) {
      if (*h != nullptr) {
        CloseHandle(*h);
        *h = nullptr;
      }
    }
#else
    if (stdin_fd_ >= 0) {
      close(stdin_fd_);
      stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0) {
      close(stdout_fd_);
      stdout_fd_ = -1;
    }
#endif
  }

  std::string buffer_;
  std::string error_;
  bool running_{};
  bool eof_{};
#ifdef _WIN32
  HANDLE stdin_w_{};
  HANDLE stdout_r_{};
  HANDLE process_{};
#else
  pid_t pid_{-1};
  int stdin_fd_{-1};
  int stdout_fd_{-1};
#endif
};
