/**
 * game.js
 * - 玩家：前端預測移動（僅玩家平滑位移）
 * - 世界：固定座標繪製，不隨玩家移動做視覺補償
 * - 敵人：Map<id> + Lerp 追 target
 * - 子彈：Map<id> + 100% Snap + Dead Reckoning (vx, vy)
 * - 掉落：Map<id> + 直接對齊
 */

(function () {
  "use strict";

  const POLL_DELAY_MS = 25;
  const FETCH_TIMEOUT_MS = 2500;
  const CANVAS_W = 800;
  const CANVAS_H = 600;
  const PLAYER_R = 18;
  const PLAYER_SPEED = 260;
  const ENEMY_LERP = 0.3;
  const ENEMY_SNAP_DIST = 100;

  const canvas = document.getElementById("gameCanvas");
  const ctx = canvas.getContext("2d");
  const waveDisplay = document.getElementById("waveDisplay");
  const scoreDisplay = document.getElementById("scoreDisplay");
  const bossBar = document.getElementById("bossBar");
  const bossBarFill = document.getElementById("bossBarFill");
  const overlay = document.getElementById("overlay");
  const overlayTitle = document.getElementById("overlayTitle");
  const overlayMessage = document.getElementById("overlayMessage");
  const overlayBtn = document.getElementById("overlayBtn");

  let latestState = null;
  let prevGameState = null;
  let pollScheduled = false;
  let lastRafTime = performance.now();

  let pendingMove = null;
  let moveSendInFlight = false;

  const localPlayer = {
    x: CANVAS_W / 2,
    y: CANVAS_H / 2,
    hp: 100,
    maxHp: 100,
    damage: 3.5,
    fireRate: 3.5,
  };

  /** id -> 視覺實體（Map 追蹤同一物件） */
  const enemyVis = new Map();
  const bulletVis = new Map();
  const lootVis = new Map();

  const keys = {
    ArrowUp: false,
    ArrowDown: false,
    ArrowLeft: false,
    ArrowRight: false,
    KeyW: false,
    KeyA: false,
    KeyS: false,
    KeyD: false,
  };

  function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
  }

  function dist(x1, y1, x2, y2) {
    return Math.hypot(x2 - x1, y2 - y1);
  }

  function clearVisualEntities() {
    enemyVis.clear();
    bulletVis.clear();
    lootVis.clear();
  }

  function computeMoveVector() {
    let dx = 0;
    let dy = 0;
    if (keys.ArrowLeft || keys.KeyA) dx -= 1;
    if (keys.ArrowRight || keys.KeyD) dx += 1;
    if (keys.ArrowUp || keys.KeyW) dy -= 1;
    if (keys.ArrowDown || keys.KeyS) dy += 1;
    return { dx, dy };
  }

  function updateLocalPlayer(frameDt) {
    if (latestState?.state !== "PLAYING") return;
    const { dx, dy } = computeMoveVector();
    const len = Math.hypot(dx, dy);
    if (len < 1e-6) return;
    localPlayer.x += (dx / len) * PLAYER_SPEED * frameDt;
    localPlayer.y += (dy / len) * PLAYER_SPEED * frameDt;
    localPlayer.x = clamp(localPlayer.x, PLAYER_R, CANVAS_W - PLAYER_R);
    localPlayer.y = clamp(localPlayer.y, PLAYER_R, CANVAS_H - PLAYER_R);
  }

  function syncPlayerStatsFromServer(p) {
    if (!p) return;
    localPlayer.hp = p.hp;
    localPlayer.maxHp = p.maxHp;
    localPlayer.damage = p.damage;
    localPlayer.fireRate = p.fireRate;
  }

  function syncPlayerFullFromServer(p) {
    if (!p) return;
    localPlayer.x = p.x;
    localPlayer.y = p.y;
    syncPlayerStatsFromServer(p);
  }

  /**
   * 通用：依伺服器 ID 列表同步 Map（更新 target / 新增 / 刪除孤兒）
   */
  function syncIdMap(map, serverList, onCreate, onUpdate) {
    const seen = new Set();
    if (!Array.isArray(serverList)) {
      map.clear();
      return;
    }

    for (const s of serverList) {
      const id = s.id;
      if (id == null) continue;
      seen.add(id);

      if (map.has(id)) {
        onUpdate(map.get(id), s);
      } else {
        map.set(id, onCreate(s));
      }
    }

    for (const id of map.keys()) {
      if (!seen.has(id)) {
        map.delete(id);
      }
    }
  }

  function syncEnemiesFromServer(list) {
    syncIdMap(
      enemyVis,
      list,
      (s) => ({
        current_x: s.x,
        current_y: s.y,
        target_x: s.x,
        target_y: s.y,
        hp: s.hp,
        maxHp: s.maxHp,
        boss: s.boss,
        speed: s.speed,
      }),
      (vis, s) => {
        if (dist(vis.current_x, vis.current_y, s.x, s.y) > ENEMY_SNAP_DIST) {
          vis.current_x = s.x;
          vis.current_y = s.y;
        }
        vis.target_x = s.x;
        vis.target_y = s.y;
        vis.hp = s.hp;
        vis.maxHp = s.maxHp;
        vis.boss = s.boss;
        vis.speed = s.speed;
      }
    );
  }

  /** 子彈：收到時 100% Snap，不做 Lerp */
  function syncBulletsFromServer(list) {
    syncIdMap(
      bulletVis,
      list,
      (s) => ({
        x: s.x,
        y: s.y,
        vx: s.vx || 0,
        vy: s.vy || 0,
        owner: s.owner,
      }),
      (vis, s) => {
        vis.x = s.x;
        vis.y = s.y;
        vis.vx = s.vx || 0;
        vis.vy = s.vy || 0;
        vis.owner = s.owner;
      }
    );
  }

  function syncLootFromServer(list) {
    syncIdMap(
      lootVis,
      list,
      (s) => ({
        x: s.x,
        y: s.y,
        type: s.type,
      }),
      (vis, s) => {
        vis.x = s.x;
        vis.y = s.y;
        vis.type = s.type;
      }
    );
  }

  function stepEnemyLerp() {
    for (const vis of enemyVis.values()) {
      const d = dist(vis.current_x, vis.current_y, vis.target_x, vis.target_y);
      if (d > ENEMY_SNAP_DIST) {
        vis.current_x = vis.target_x;
        vis.current_y = vis.target_y;
      } else {
        vis.current_x += (vis.target_x - vis.current_x) * ENEMY_LERP;
        vis.current_y += (vis.target_y - vis.current_y) * ENEMY_LERP;
      }
    }
  }

  /** 子彈 Dead Reckoning：Snap 後依 vx,vy 每帧前進 */
  function stepBulletDeadReckoning(frameDt) {
    for (const b of bulletVis.values()) {
      b.x += b.vx * frameDt;
      b.y += b.vy * frameDt;
    }
  }

  function onStateReceived(data) {
    latestState = data;

    if (data.state === "PLAYING") {
      if (prevGameState !== "PLAYING") {
        syncPlayerFullFromServer(data.player);
      } else {
        syncPlayerStatsFromServer(data.player);
      }
      syncEnemiesFromServer(data.enemies);
      syncBulletsFromServer(data.bullets);
      syncLootFromServer(data.loot);
    } else {
      syncPlayerFullFromServer(data.player);
      clearVisualEntities();
    }
    prevGameState = data.state;

    updateHudDom(data);
    updateOverlay(data);
  }

  async function pollState() {
    pollScheduled = false;
    try {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS);
      const res = await fetch("/api/state", {
        signal: controller.signal,
        cache: "no-store",
      });
      clearTimeout(timer);
      if (res.ok) {
        onStateReceived(JSON.parse(await res.text()));
      }
    } catch (err) {
      if (err.name !== "AbortError") console.warn("pollState:", err);
    }
    pollScheduled = true;
    setTimeout(pollState, POLL_DELAY_MS);
  }

  function startPolling() {
    if (!pollScheduled) {
      pollScheduled = true;
      pollState();
    }
  }

  async function sendAction(payload) {
    try {
      await fetch("/api/action", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
    } catch (err) {
      console.warn("sendAction failed", err);
    }
  }

  function buildMovePayload() {
    const { dx, dy } = computeMoveVector();
    return {
      action: "move",
      dx,
      dy,
      x: Math.round(localPlayer.x * 10) / 10,
      y: Math.round(localPlayer.y * 10) / 10,
    };
  }

  /** 合併高頻移動：飛行中只更新 pending，完成後送出最新座標，避免亂序與堆積 */
  async function flushMoveQueue() {
    if (!pendingMove || moveSendInFlight) return;
    moveSendInFlight = true;
    try {
      while (pendingMove) {
        const payload = pendingMove;
        pendingMove = null;
        await sendAction(payload);
      }
    } finally {
      moveSendInFlight = false;
      if (pendingMove) flushMoveQueue();
    }
  }

  function queuePlayerPosition() {
    if (latestState?.state !== "PLAYING") return;
    pendingMove = buildMovePayload();
    flushMoveQueue();
  }

  function sendPlayerPosition() {
    queuePlayerPosition();
  }

  function flushPlayerPosition() {
    if (latestState?.state !== "PLAYING") return;
    pendingMove = buildMovePayload();
    flushMoveQueue();
  }

  function clearAllKeys() {
    for (const k of Object.keys(keys)) keys[k] = false;
    flushPlayerPosition();
  }

  function updateHudDom(state) {
    if (!state) return;
    waveDisplay.textContent = `Wave ${state.wave ?? 0}`;
    scoreDisplay.textContent = `Score ${state.score ?? 0}`;
  }

  function findBossFromServer() {
    const list = latestState?.enemies;
    if (!Array.isArray(list)) return null;
    return list.find((e) => e.boss === 1) || null;
  }

  function updateBossBar() {
    if (!latestState || latestState.state !== "PLAYING") {
      bossBar.classList.add("hidden");
      return;
    }
    const boss = findBossFromServer();
    if (!boss) {
      bossBar.classList.add("hidden");
      return;
    }
    bossBar.classList.remove("hidden");
    const maxHp = boss.maxHp > 0 ? boss.maxHp : 5000;
    bossBarFill.style.width = `${clamp((boss.hp / maxHp) * 100, 0, 100)}%`;
  }

  function updateOverlay(state) {
    if (!state) {
      showOverlay("載入中", "正在連線 C 引擎…", "請稍候", false);
      return;
    }
    if (state.engine_ok === false) {
      showOverlay(
        "引擎離線",
        state.engine_error || "請編譯 backend/game_engine",
        "重試",
        true,
        () => sendAction({ action: "start" })
      );
      return;
    }
    switch (state.state) {
      case "MENU":
        showOverlay("彈幕肉鴿", "生存 4 波後迎戰 Boss", "開始遊戲", true, () =>
          sendAction({ action: "start" })
        );
        break;
      case "PLAYING":
        overlay.classList.add("hidden");
        canvas.focus();
        break;
      case "GAME_OVER":
        showOverlay("Game Over", `分數：${state.score ?? 0}`, "再玩一次", true, () =>
          sendAction({ action: "restart" })
        );
        break;
      case "WIN":
        showOverlay("Victory!", `Boss 擊敗！分數：${state.score ?? 0}`, "再玩一次", true, () =>
          sendAction({ action: "restart" })
        );
        break;
      default:
        showOverlay("連線中", `狀態：${state.state}`, "開始", true, () =>
          sendAction({ action: "start" })
        );
    }
  }

  function showOverlay(title, message, btnText, showBtn, onClick) {
    overlay.classList.remove("hidden");
    overlayTitle.textContent = title;
    overlayMessage.textContent = message;
    overlayBtn.textContent = btnText;
    overlayBtn.style.display = showBtn ? "inline-block" : "none";
    overlayBtn.onclick = onClick || null;
  }

  function clearCanvas() {
    ctx.fillStyle = "#12151f";
    ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);
  }

  function drawPlayer() {
    const p = localPlayer;
    ctx.beginPath();
    ctx.arc(p.x, p.y, PLAYER_R, 0, Math.PI * 2);
    ctx.fillStyle = "#4dabf7";
    ctx.fill();
    ctx.strokeStyle = "#74c0fc";
    ctx.lineWidth = 2;
    ctx.stroke();
    const maxHp = p.maxHp || 100;
    const w = 36;
    const h = 4;
    ctx.fillStyle = "#333";
    ctx.fillRect(p.x - w / 2, p.y - 28, w, h);
    ctx.fillStyle = "#51cf66";
    ctx.fillRect(p.x - w / 2, p.y - 28, w * (p.hp / maxHp), h);
  }

  function drawEnemies() {
    for (const e of enemyVis.values()) {
      const x = e.current_x;
      const y = e.current_y;
      if (e.boss) {
        ctx.fillStyle = "#9c36b5";
        ctx.fillRect(x - 36, y - 36, 72, 72);
        ctx.strokeStyle = "#e599f7";
        ctx.lineWidth = 2;
        ctx.strokeRect(x - 36, y - 36, 72, 72);
      } else {
        ctx.fillStyle = "#fa5252";
        ctx.fillRect(x - 20, y - 20, 40, 40);
      }
    }
  }

  function drawBullets() {
    for (const b of bulletVis.values()) {
      ctx.beginPath();
      ctx.arc(b.x, b.y, b.owner === 1 ? 6 : 5, 0, Math.PI * 2);
      ctx.fillStyle = b.owner === 1 ? "#ff922b" : "#ffe066";
      ctx.fill();
    }
  }

  function drawLoot() {
    for (const l of lootVis.values()) {
      ctx.save();
      ctx.translate(l.x, l.y);
      ctx.rotate(Math.PI / 4);
      ctx.fillStyle = l.type === 0 ? "#ff6b6b" : "#69db7c";
      ctx.fillRect(-10, -10, 20, 20);
      ctx.restore();
    }
  }

  function renderFrame(now) {
    const frameDt = clamp((now - lastRafTime) / 1000, 0, 0.05);
    lastRafTime = now;

    updateLocalPlayer(frameDt);
    sendPlayerPosition();
    stepEnemyLerp();
    stepBulletDeadReckoning(frameDt);

    clearCanvas();

    if (!latestState) {
      requestAnimationFrame(renderFrame);
      return;
    }

    if (latestState.state === "PLAYING") {
      drawLoot();
      drawEnemies();
      drawBullets();
      drawPlayer();
      updateBossBar();
    } else {
      drawPlayer();
    }

    requestAnimationFrame(renderFrame);
  }

  const MOVE_CODES = new Set([
    "ArrowUp",
    "ArrowDown",
    "ArrowLeft",
    "ArrowRight",
    "KeyW",
    "KeyA",
    "KeyS",
    "KeyD",
  ]);

  function setKeyByCode(code, pressed) {
    if (!Object.prototype.hasOwnProperty.call(keys, code)) return;
    keys[code] = pressed;
    flushPlayerPosition();
  }

  window.addEventListener(
    "keydown",
    (e) => {
      if (!MOVE_CODES.has(e.code)) return;
      e.preventDefault();
      if (e.repeat) return;
      setKeyByCode(e.code, true);
    },
    { passive: false }
  );

  window.addEventListener(
    "keyup",
    (e) => {
      if (!MOVE_CODES.has(e.code)) return;
      e.preventDefault();
      setKeyByCode(e.code, false);
    },
    { passive: false }
  );

  window.addEventListener("blur", clearAllKeys);
  overlayBtn.addEventListener("click", () => {
    if (overlayBtn.onclick) overlayBtn.onclick();
  });

  startPolling();
  requestAnimationFrame(renderFrame);
  canvas.focus();
})();
