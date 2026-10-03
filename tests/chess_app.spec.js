// @ts-check
// End-to-end tests for Chess Assist, driven through the app's automation
// console (http://127.0.0.1:8765/ by default, see AutomationServer.h).
//
// Covers: side-selection prompt, camera flip (yaw 0° White / 180° Black),
// SAN move entry (1. d4 e6) with LAN mapping and history, Winning Assistant
// recommendation + win-probability updates, input validation, and (in
// windowed mode) a PNG capture of the live 3D frame.
//
// Run:  npx playwright test            (starts ./bin/chess-assist --headless)
//       CHESS_APP_URL=http://127.0.0.1:8765 npx playwright test   (attach to a running app)

const { test, expect } = require('@playwright/test');

/** Smallest angle between two headings, in degrees (handles 359.9 vs 0). */
function yawDistance(a, b) {
  return Math.abs(((((a - b) % 360) + 540) % 360) - 180);
}

/** @param {import('@playwright/test').APIRequestContext} request */
async function getState(request) {
  const res = await request.get('/api/state');
  expect(res.ok()).toBeTruthy();
  return res.json();
}

/**
 * Waits until the engine has finished analysing the *current* position and
 * returns that state.
 * @param {import('@playwright/test').APIRequestContext} request
 */
async function waitForAnalysis(request, timeout = 20_000) {
  let latest;
  await expect
    .poll(
      async () => {
        latest = await getState(request);
        const a = latest.assistant;
        return a.complete && a.upToDate && a.analysedPositionId === latest.positionId && a.bestMoveSan !== '';
      },
      { timeout, message: 'Stockfish analysis for the current position' },
    )
    .toBe(true);
  return latest;
}

/**
 * Types a line into the move terminal and waits until the app has answered
 * (the feedback line mentions the input, or the input box is cleared).
 * @param {import('@playwright/test').Page} page
 */
async function enterMove(page, san) {
  const input = page.getByTestId('move-input');
  const feedback = page.getByTestId('terminal-feedback');
  const before = await feedback.textContent();
  await input.fill(san);
  await input.press('Enter');
  await expect(feedback).not.toHaveText(before ?? '', { timeout: 10_000 });
}

/** Plays a move that must be legal and waits for it to land. */
async function playMove(page, san) {
  await enterMove(page, san);
  await expect(page.getByTestId('terminal-feedback')).toHaveAttribute('data-ok', 'true');
  await expect(page.getByTestId('move-input')).toHaveValue('');
}

test.describe.configure({ mode: 'serial' });

test.beforeEach(async ({ page, request }) => {
  // Fresh game with no side chosen, then open the console.
  const res = await request.post('/api/reset', { data: { clearSide: true } });
  expect(res.ok()).toBeTruthy();
  await page.goto('/');
  await expect(page.getByTestId('connection')).toContainText('online');
});

test('engine is connected', async ({ request }) => {
  const s = await waitForAnalysis(request);
  expect(s.assistant.engineOk, s.assistant.engineError).toBe(true);
  expect(s.assistant.engineName).toMatch(/stockfish/i);
});

test('launch prompts the user to choose White or Black', async ({ page, request }) => {
  await expect(page.getByTestId('side-selection')).toBeVisible();
  await expect(page.getByTestId('side-white')).toBeVisible();
  await expect(page.getByTestId('side-black')).toBeVisible();
  const s = await getState(request);
  expect(s.sideSelected).toBe(false);
  expect(s.side).toBeNull();
  expect(s.history).toHaveLength(0);

  // Moves are refused until a side is chosen.
  await enterMove(page, 'd4');
  await expect(page.getByTestId('terminal-feedback')).toHaveAttribute('data-ok', 'false');
  await expect(page.getByTestId('terminal-feedback')).toContainText(/choose a side/i);
});

test('side selection flips the 3D camera: Black = 180°, White = 0°', async ({ page, request }) => {
  await page.getByTestId('side-black').click();
  await expect(page.getByTestId('side-selection')).toBeHidden();
  await expect(page.getByTestId('side-label')).toHaveText('Black');

  // Camera target snaps to 180° and the animated yaw converges on it.
  await expect.poll(async () => (await getState(request)).camera.targetYaw).toBeCloseTo(180, 0);
  await expect
    .poll(async () => yawDistance((await getState(request)).camera.yaw, 180), { timeout: 10_000 })
    .toBeLessThan(1.0);
  await expect(page.getByTestId('board-2d')).toHaveAttribute('data-orientation', 'black');
  // From Black's seat the top-left square is h1 and the bottom-right is a8.
  const squares = page.getByTestId('board-2d').locator('[data-square]');
  await expect(squares.first()).toHaveAttribute('data-square', 'h1');
  await expect(squares.last()).toHaveAttribute('data-square', 'a8');

  // Switching back to White returns the camera to 0°.
  await page.getByTestId('switch-white').click();
  await expect(page.getByTestId('side-label')).toHaveText('White');
  await expect
    .poll(async () => yawDistance((await getState(request)).camera.yaw, 0), { timeout: 10_000 })
    .toBeLessThan(1.0);
  await expect(page.getByTestId('board-2d')).toHaveAttribute('data-orientation', 'white');
  await expect(squares.first()).toHaveAttribute('data-square', 'a8');
});

test('SAN terminal plays 1. d4 e6, maps to LAN and records history', async ({ page, request }) => {
  await page.getByTestId('side-white').click();
  await expect(page.getByTestId('side-label')).toHaveText('White');

  await playMove(page, 'd4');
  await expect(page.getByTestId('terminal-feedback')).toContainText('d2d4');
  await expect(page.getByTestId('history-row')).toHaveCount(1);
  await expect(page.getByTestId('history-row').first()).toContainText('d4');

  await playMove(page, 'e6');
  await expect(page.getByTestId('terminal-feedback')).toContainText('e7e6');
  await expect(page.getByTestId('move-history')).toHaveAttribute('data-text', '1. d4 e6');

  const s = await getState(request);
  expect(s.historyText).toBe('1. d4 e6');
  expect(s.history.map((h) => h.lan)).toEqual(['d2d4', 'e7e6']);
  expect(s.turn).toBe('white');
  // Pieces moved on the board (a1 = index 0).
  const board2d = page.getByTestId('board-2d');
  await expect(board2d.locator('[data-square="d4"]')).toHaveAttribute('data-piece', 'P');
  await expect(board2d.locator('[data-square="e6"]')).toHaveAttribute('data-piece', 'p');
  await expect(board2d.locator('[data-square="d2"]')).toHaveAttribute('data-piece', '');

  // Turn numbering continues: 2. c4
  await playMove(page, 'c4');
  await expect(page.getByTestId('move-history')).toHaveAttribute('data-text', '1. d4 e6 2. c4');
});

test('Winning Assistant overlay updates after every move', async ({ page, request }) => {
  await page.getByTestId('side-white').click();
  await expect(page.getByTestId('side-label')).toHaveText('White');
  const overlay = page.getByTestId('assistant-overlay');

  // Starting position: a recommendation for the user (White).
  const s0 = await waitForAnalysis(request);
  expect(s0.assistant.bestForUser).toBe(true);
  await expect(overlay).toHaveAttribute('data-complete', 'true');
  await expect(page.getByTestId('best-move')).not.toHaveText('...');

  // 1. d4 -> the overlay switches to Black's (opponent's) best reply.
  await playMove(page, 'd4');
  const s1 = await waitForAnalysis(request);
  expect(s1.positionId).toBeGreaterThan(s0.positionId);
  expect(s1.assistant.bestForUser).toBe(false);
  await expect(overlay).toHaveAttribute('data-analysed-position-id', String(s1.positionId));
  await expect(overlay).toHaveAttribute('data-best-for-user', 'false');

  // 1... e6 -> back to the user's turn with a fresh recommendation.
  await playMove(page, 'e6');
  const s2 = await waitForAnalysis(request);
  expect(s2.positionId).toBeGreaterThan(s1.positionId);
  const a = s2.assistant;
  expect(a.bestForUser).toBe(true);
  expect(a.bestMoveSan).toMatch(/^([KQRBN][a-h]?[1-8]?x?[a-h][1-8]|[a-h](x[a-h])?[1-8](=[QRBN])?|O-O(-O)?)[+#]?$/);
  expect(a.bestMoveLan).toMatch(/^[a-h][1-8][a-h][1-8][qrbn]?$/);
  expect(a.depth).toBeGreaterThan(0);

  await expect(overlay).toHaveAttribute('data-analysed-position-id', String(s2.positionId));
  await expect(overlay).toHaveAttribute('data-best-for-user', 'true');
  await expect(page.getByTestId('best-move')).toHaveText(a.bestMoveSan);
  await expect(page.getByTestId('assistant-headline')).toContainText(`play ${a.bestMoveSan}`);
  await expect(page.getByTestId('win-probability')).toHaveText(/^\d{1,3}\.\d%$/);
  await expect(page.getByTestId('eval-text')).toHaveText(/^([+-]\d+\.\d\d|#-?\d+)$/);

  // WinProb = 1 / (1 + 10^(-eval/4)), eval in pawns, from the user's side.
  if (!a.isMate) {
    const expected = 1 / (1 + Math.pow(10, -(a.scoreCp / 100) / 4));
    expect(a.whiteWinProbability).toBeCloseTo(expected, 3);
    expect(a.userWinProbability).toBeCloseTo(expected, 3); // user is White
  }

  // The recommendation is a legal move: play it, then take it back.
  const play = await request.post('/api/move', { data: { san: a.bestMoveSan } });
  expect(play.ok()).toBeTruthy();
  const undo = await request.post('/api/command', { data: { command: 'undo' } });
  expect(undo.ok()).toBeTruthy();
  expect((await getState(request)).historyText).toBe('1. d4 e6');
});

test('invalid input is rejected with a helpful message', async ({ page, request }) => {
  await page.getByTestId('side-white').click();
  await expect(page.getByTestId('side-label')).toHaveText('White');
  for (const m of ['Nf3', 'a6', 'd4', 'a5']) {
    await playMove(page, m);
  }
  const before = (await getState(request)).positionId;

  await enterMove(page, 'Ke2x');
  await expect(page.getByTestId('terminal-feedback')).toHaveAttribute('data-ok', 'false');
  await expect(page.getByTestId('terminal-feedback')).toContainText(/illegal/i);

  // Two knights can reach d2 -> ambiguity is explained.
  await enterMove(page, 'Nd2');
  await expect(page.getByTestId('terminal-feedback')).toContainText(/Ambiguous.*Nbd2.*Nfd2/);
  expect((await getState(request)).positionId).toBe(before);

  // The disambiguated move is accepted.
  await playMove(page, 'Nbd2');
  await expect(page.getByTestId('terminal-feedback')).toContainText('b1d2');
});

test('new game and change side commands', async ({ request }) => {
  await request.post('/api/side', { data: { side: 'white' } });
  await request.post('/api/move', { data: { san: 'e4' } });
  await request.post('/api/move', { data: { san: 'e5' } });
  // "side" reopens the side prompt but keeps the game.
  let res = await request.post('/api/command', { data: { command: 'side' } });
  expect(res.ok()).toBeTruthy();
  let s = await getState(request);
  expect(s.sideSelected).toBe(false);
  expect(s.historyText).toBe('1. e4 e5');
  await request.post('/api/side', { data: { side: 'black' } });
  // "new" clears the moves and keeps the chosen side.
  res = await request.post('/api/command', { data: { command: 'new' } });
  expect(res.ok()).toBeTruthy();
  s = await getState(request);
  expect(s.history).toHaveLength(0);
  expect(s.side).toBe('black');
  expect(s.turn).toBe('white');
});

test('environment can switch between gallery hall and studio', async ({ request }) => {
  const s0 = await getState(request);
  expect(s0.scene).toBe('gallery');
  expect(s0.app.version).toMatch(/^\d+\.\d+\.\d+$/);
  expect(s0.perf).toHaveProperty('gpuMs.scene');
  let res = await request.post('/api/command', { data: { command: 'studio' } });
  expect(res.ok()).toBeTruthy();
  expect((await getState(request)).scene).toBe('studio');
  res = await request.post('/api/command', { data: { command: 'gallery' } });
  expect(res.ok()).toBeTruthy();
  expect((await getState(request)).scene).toBe('gallery');
});

test('live 3D frame can be captured (windowed mode only)', async ({ request }, testInfo) => {
  const s = await getState(request);
  test.skip(s.app.headless, 'App is running headless; start it windowed to capture frames');
  await request.post('/api/side', { data: { side: 'black' } });
  await request.post('/api/move', { data: { san: 'd4' } });
  await request.post('/api/move', { data: { san: 'e6' } });
  await waitForAnalysis(request);
  const res = await request.get('/api/screenshot.png', { timeout: 30_000 });
  expect(res.ok()).toBeTruthy();
  expect(res.headers()['content-type']).toBe('image/png');
  const png = await res.body();
  expect(png.subarray(1, 4).toString()).toBe('PNG');
  await testInfo.attach('3d-view-black-after-d4-e6.png', { body: png, contentType: 'image/png' });
});
