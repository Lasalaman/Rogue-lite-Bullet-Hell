"""
app.py - Flask Web API（subprocess + C 引擎）

效能：
  - stdout 執行緒只儲存 C 引擎的 JSON 原始字串，不做 json.loads
  - /api/state 直接回傳 raw string（mimetype=application/json），零序列化開銷
"""

from __future__ import annotations

import atexit
import json
import logging
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

from flask import Flask, Response, jsonify, render_template, request

BASE_DIR = Path(__file__).resolve().parent
BACKEND_DIR = BASE_DIR / "backend"
ENGINE_EXE = "game_engine.exe" if sys.platform == "win32" else "game_engine"
ENGINE_PATH = BACKEND_DIR / ENGINE_EXE

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("flask-game")

CLIENT_POLL_MS = 15

# 啟動時 C 格式 JSON（不含 Flask 中繼欄位；中繼欄位由 _wrap_c_json 在 GET 時拼接）
_C_BOOT_PAYLOAD = {
    "state": "BOOT",
    "wave": 0,
    "tick": 0,
    "score": 0,
    "tickRate": 60,
    "player": {
        "x": 400.0,
        "y": 300.0,
        "hp": 100.0,
        "maxHp": 100.0,
        "damage": 3.5,
        "fireRate": 3.5,
        "speed": 260.0,
        "dx": 0,
        "dy": 0,
    },
    "enemies": [],
    "bullets": [],
    "loot": [],
}
DEFAULT_C_JSON_RAW = json.dumps(_C_BOOT_PAYLOAD, separators=(",", ":"))

STALE_ENGINE_SECONDS = 8.0
HEALTH_CHECK_INTERVAL = 2.0
STDOUT_DRAIN_MAX = 40


def _wrap_c_json(c_json: str, *, engine_ok: bool, engine_error: Optional[str]) -> str:
    """
    將 C 引擎 JSON 加上 Flask 中繼欄位（字串拼接，不 parse C 內容）。
    c_json 必須以 '{' 開頭。
    """
    ts = time.time()
    meta = (
        f'"engine_ok":{str(engine_ok).lower()},'
        f'"engine_error":{json.dumps(engine_error)},'
        f'"serverTime":{ts},'
        f'"clientPollMs":{CLIENT_POLL_MS},'
    )
    if c_json.startswith("{"):
        return "{" + meta + c_json[1:]
    return c_json


class GameEngineBridge:
    def __init__(self) -> None:
        self._state_lock = threading.Lock()
        self._proc_lock = threading.Lock()
        # C 引擎 STATE 本體（不含 STATE 前綴）的原始 JSON 字串
        self._latest_c_json: str = DEFAULT_C_JSON_RAW
        self._engine_ok: bool = False
        self._engine_error: Optional[str] = "等待 C 引擎"
        self._last_update_mono: float = 0.0
        self._proc: Optional[subprocess.Popen[str]] = None
        self._stop = threading.Event()
        self._threads_started = False

    def get_state_raw(self) -> str:
        """回傳可直接作為 HTTP body 的 JSON 字串（已含中繼欄位）。"""
        with self._state_lock:
            body = _wrap_c_json(
                self._latest_c_json,
                engine_ok=self._engine_ok,
                engine_error=self._engine_error,
            )
        return body

    def send_action(self, payload: dict) -> None:
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        with self._proc_lock:
            proc = self._proc
            if proc is None or proc.stdin is None:
                raise RuntimeError("C 引擎未啟動")
            if proc.poll() is not None:
                raise RuntimeError("C 引擎已結束，請稍後重試")
            try:
                proc.stdin.write(line)
                proc.stdin.flush()
            except (BrokenPipeError, OSError) as exc:
                log.warning("stdin 寫入失敗: %s", exc)
                self._restart_engine_locked()
                raise RuntimeError("C 引擎通訊中斷，已嘗試重啟") from exc

    def start_background_workers(self) -> None:
        if self._threads_started:
            return
        self._threads_started = True
        self._stop.clear()
        self._spawn_engine_locked()

        threading.Thread(target=self._stdout_reader_loop, name="engine-stdout", daemon=True).start()
        threading.Thread(target=self._stderr_reader_loop, name="engine-stderr", daemon=True).start()
        threading.Thread(target=self._health_monitor_loop, name="engine-health", daemon=True).start()
        log.info("背景執行緒已啟動，引擎路徑: %s", ENGINE_PATH)

    def shutdown(self) -> None:
        self._stop.set()
        with self._proc_lock:
            self._terminate_proc_locked()

    def _set_error_state(self, message: str) -> None:
        with self._state_lock:
            self._engine_ok = False
            self._engine_error = message

    def _spawn_engine_locked(self) -> None:
        if not ENGINE_PATH.is_file():
            msg = f"找不到 C 引擎: {ENGINE_PATH}（請在 backend/ 編譯 game_engine）"
            log.error(msg)
            self._set_error_state(msg)
            return

        self._terminate_proc_locked()
        try:
            self._proc = subprocess.Popen(
                [str(ENGINE_PATH)],
                cwd=str(BACKEND_DIR),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
            log.info("C 引擎已啟動 pid=%s", self._proc.pid)
            with self._state_lock:
                self._engine_ok = True
                self._engine_error = None
            self._last_update_mono = time.monotonic()
        except OSError as exc:
            log.exception("無法啟動 C 引擎")
            self._set_error_state(str(exc))

    def _terminate_proc_locked(self) -> None:
        proc = self._proc
        self._proc = None
        if proc is None:
            return
        try:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2.0)
        except OSError as exc:
            log.warning("結束子行程時發生錯誤: %s", exc)

    def _restart_engine_locked(self) -> None:
        log.warning("正在重啟 C 引擎…")
        self._spawn_engine_locked()

    def _apply_state_line(self, line: str) -> None:
        """只截取 JSON 字串存入記憶體，不做 json.loads。"""
        if not line.startswith("STATE "):
            return
        c_json = line[6:].strip()
        if not c_json.startswith("{"):
            return
        with self._state_lock:
            self._latest_c_json = c_json
            self._engine_ok = True
            self._engine_error = None
        self._last_update_mono = time.monotonic()

    def _stdout_reader_loop(self) -> None:
        while not self._stop.is_set():
            with self._proc_lock:
                proc = self._proc

            if proc is None or proc.stdout is None:
                time.sleep(0.05)
                continue

            drained = 0
            try:
                while drained < STDOUT_DRAIN_MAX:
                    line = proc.stdout.readline()
                    if line == "":
                        if proc.poll() is not None:
                            log.warning("C 引擎已結束 (code=%s)", proc.returncode)
                            self._handle_engine_crash()
                        break
                    stripped = line.strip()
                    if stripped:
                        self._apply_state_line(stripped)
                    drained += 1
            except OSError as exc:
                log.warning("讀取 stdout 失敗: %s", exc)
                self._handle_engine_crash()
                continue

            if drained == 0:
                time.sleep(0.002)

    def _stderr_reader_loop(self) -> None:
        while not self._stop.is_set():
            with self._proc_lock:
                proc = self._proc
            if proc is None or proc.stderr is None:
                time.sleep(0.1)
                continue
            try:
                err_line = proc.stderr.readline()
            except OSError:
                time.sleep(0.1)
                continue
            if err_line == "":
                time.sleep(0.05)
                continue
            log.debug("[C stderr] %s", err_line.rstrip())

    def _health_monitor_loop(self) -> None:
        while not self._stop.is_set():
            time.sleep(HEALTH_CHECK_INTERVAL)
            if self._stop.is_set():
                break

            with self._proc_lock:
                proc = self._proc

            if proc is None:
                with self._proc_lock:
                    if self._proc is None and ENGINE_PATH.is_file():
                        self._spawn_engine_locked()
                continue

            if proc.poll() is not None:
                log.warning("健康檢查：引擎已退出，重啟")
                self._handle_engine_crash()
                continue

            stale_for = time.monotonic() - self._last_update_mono
            if self._last_update_mono > 0 and stale_for > STALE_ENGINE_SECONDS:
                log.warning("健康檢查：%.1fs 無 STATE，重啟", stale_for)
                self._handle_engine_crash()

    def _handle_engine_crash(self) -> None:
        if self._stop.is_set():
            return
        with self._proc_lock:
            self._restart_engine_locked()
        self._set_error_state("引擎已重啟，請重新開始")


app = Flask(
    __name__,
    template_folder=str(BASE_DIR / "frontend" / "templates"),
    static_folder=str(BASE_DIR / "frontend" / "static"),
    static_url_path="/static",
)

engine = GameEngineBridge()


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/state", methods=["GET"])
def api_state():
    """零拷貝：直接回傳預組好的 JSON 字串。"""
    return Response(engine.get_state_raw(), mimetype="application/json")


@app.route("/api/action", methods=["POST"])
def api_action():
    payload = request.get_json(silent=True)
    if not payload or "action" not in payload:
        return jsonify({"ok": False, "error": "需要 JSON 欄位 action"}), 400

    action = payload.get("action")
    out: dict = {"action": action}

    if action == "move":
        out["dx"] = max(-1, min(1, int(payload.get("dx", 0))))
        out["dy"] = max(-1, min(1, int(payload.get("dy", 0))))
        if "x" in payload and "y" in payload:
            out["x"] = round(float(payload["x"]), 2)
            out["y"] = round(float(payload["y"]), 2)
    elif action in ("start", "restart"):
        pass
    else:
        return jsonify({"ok": False, "error": f"不支援的 action: {action}"}), 400

    try:
        engine.send_action(out)
    except RuntimeError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 503

    return jsonify({"ok": True, "sent": out})


def _startup() -> None:
    engine.start_background_workers()


def _shutdown() -> None:
    engine.shutdown()


atexit.register(_shutdown)
_startup()


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000, debug=False, threaded=True)
