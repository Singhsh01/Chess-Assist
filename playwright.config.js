// @ts-check
// Playwright configuration for the Chess Assist end-to-end suite.
//
// By default Playwright launches the app itself in headless mode (no window,
// real Stockfish) and talks to its automation console. Environment overrides:
//
//   CHESS_APP_URL   attach to an already running app instead of launching one
//   CHESS_APP_CMD   custom launch command (e.g. the windowed app under xvfb-run)
//   CHESS_APP_PORT  port for the launched app (default 8765)

const { defineConfig, devices } = require('@playwright/test');

const PORT = Number(process.env.CHESS_APP_PORT || 8765);
const BIN = process.platform === 'win32' ? 'bin\\chess-assist.exe' : './bin/chess-assist';
const external = Boolean(process.env.CHESS_APP_URL);

module.exports = defineConfig({
  testDir: './tests',
  testMatch: /.*\.spec\.js$/,
  timeout: 90_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1, // one app instance, shared game state
  retries: process.env.CI ? 1 : 0,
  reporter: [['list'], ['html', { open: 'never' }]],
  use: {
    ...devices['Desktop Chrome'],
    baseURL: process.env.CHESS_APP_URL || `http://127.0.0.1:${PORT}`,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  webServer: external
    ? undefined
    : {
        command: process.env.CHESS_APP_CMD || `${BIN} --headless --port ${PORT} --movetime 600`,
        url: `http://127.0.0.1:${PORT}/api/health`,
        reuseExistingServer: !process.env.CI,
        timeout: 60_000,
        stdout: 'pipe',
        stderr: 'pipe',
      },
});
