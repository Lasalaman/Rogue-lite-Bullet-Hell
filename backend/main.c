/**
 * main.c - 遊戲主迴圈、Wave 系統、AI、碰撞、stdin/stdout JSON 協定
 *
 * 通訊約定：
 *   stdin  一行 JSON：移動 / 開始 / 重開
 *   stdout 每 tick 一行 STATE JSON（後接 fflush）
 *   stderr 除錯訊息（絕不寫入 stdout）
 */
#include "game.h"
#include "entity.h"

#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#endif

/* ---------- 輸入緩衝（累積 stdin 直到換行） ---------- */
#define INPUT_BUF_SIZE 512
#define JSON_BUF_SIZE  32768

static char g_input_line[INPUT_BUF_SIZE];
static int  g_input_len = 0;

static void game_init(Game *g);
static void game_reset_playing(Game *g);
static int  poll_stdin_line(char *out, size_t out_cap);
static void parse_input_line(Game *g, const char *line);
static void game_update(Game *g, float dt);
static void emit_state_json(const Game *g);

/* Wave / 戰鬥 */
static void wave_update(Game *g, float dt);
static void spawn_normal_enemy(Game *g);
static void spawn_boss(Game *g);
static void player_auto_shoot(Game *g);
static void update_enemy_ai(Game *g, float dt);
static void boss_attack_ring(Game *g, Enemy *boss);
static void boss_attack_fan(Game *g, Enemy *boss);
static void boss_attack_cycle(Game *g, Enemy *boss);
static void update_bullets(Game *g, float dt);
static void resolve_collisions(Game *g);
static void apply_loot_buff(Player *p, LootType t);
static void player_register_hit(Game *g);
static void player_apply_hp_regen(Game *g, float dt);

static float randf(float a, float b) {
    return a + (float)rand() / (float)RAND_MAX * (b - a);
}

static float vec_len(float x, float y) {
    return sqrtf(x * x + y * y);
}

static void clamp_player(Player *p) {
    if (p->x < PLAYER_RADIUS) p->x = PLAYER_RADIUS;
    if (p->y < PLAYER_RADIUS) p->y = PLAYER_RADIUS;
    if (p->x > GAME_W - PLAYER_RADIUS) p->x = GAME_W - PLAYER_RADIUS;
    if (p->y > GAME_H - PLAYER_RADIUS) p->y = GAME_H - PLAYER_RADIUS;
}

/* ---------- stdin 非阻塞讀取 ---------- */
static int stdin_data_available(void) {
#ifdef _WIN32
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (hin == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!PeekNamedPipe(hin, NULL, 0, NULL, &avail, NULL)) {
        return 0;
    }
    return (int)avail > 0;
#else
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

/**
 * poll_stdin_line - 非阻塞讀取 stdin，湊滿一行後回傳 1
 * 讀取到的字串寫入 out（不含換行），並清空內部緩衝。
 */
static int poll_stdin_line(char *out, size_t out_cap) {
    while (stdin_data_available()) {
        int c = fgetc(stdin);
        if (c == EOF) {
            break;
        }
        if (c == '\n' || c == '\r') {
            if (g_input_len > 0) {
                g_input_line[g_input_len] = '\0';
                strncpy(out, g_input_line, out_cap - 1);
                out[out_cap - 1] = '\0';
                g_input_len = 0;
                return 1;
            }
            continue;
        }
        if (g_input_len < INPUT_BUF_SIZE - 1) {
            g_input_line[g_input_len++] = (char)c;
        }
    }
    return 0;
}

/* 簡易 JSON 欄位解析（避免依賴第三方函式庫） */
static void parse_input_line(Game *g, const char *line) {
    if (strstr(line, "\"action\"") && strstr(line, "start")) {
        if (g->state == GAME_STATE_MENU || g->state == GAME_STATE_GAME_OVER ||
            g->state == GAME_STATE_WIN) {
            game_reset_playing(g);
            fprintf(stderr, "[engine] 遊戲開始\n");
        }
        return;
    }
    if (strstr(line, "\"action\"") && strstr(line, "restart")) {
        game_reset_playing(g);
        fprintf(stderr, "[engine] 重新開始\n");
        return;
    }

    if (g->state != GAME_STATE_PLAYING) {
        return;
    }

    /* 移動：前端權威座標 (x,y) 直接寫入 Player；dx/dy 僅供除錯或相容 */
    if (strstr(line, "\"x\"") || strstr(line, "\"y\"") ||
        strstr(line, "\"dx\"") || strstr(line, "\"dy\"") ||
        (strstr(line, "\"action\"") && strstr(line, "move"))) {
        Player *p = &g->player;
        float x = p->x;
        float y = p->y;
        int dx = p->move_dx;
        int dy = p->move_dy;
        const char *px = strstr(line, "\"x\"");
        const char *py = strstr(line, "\"y\"");
        const char *pdx = strstr(line, "\"dx\"");
        const char *pdy = strstr(line, "\"dy\"");

        if (px) {
            sscanf(px, "%*[^:]:%f", &x);
        }
        if (py) {
            sscanf(py, "%*[^:]:%f", &y);
        }
        p->x = x;
        p->y = y;
        clamp_player(p);

        if (pdx) {
            sscanf(pdx, "%*[^:]:%d", &dx);
        }
        if (pdy) {
            sscanf(pdy, "%*[^:]:%d", &dy);
        }
        if (dx < -1) {
            dx = -1;
        }
        if (dx > 1) {
            dx = 1;
        }
        if (dy < -1) {
            dy = -1;
        }
        if (dy > 1) {
            dy = 1;
        }
        p->move_dx = dx;
        p->move_dy = dy;
    }
}

static void game_init(Game *g) {
    memset(g, 0, sizeof(*g));
    g->state = GAME_STATE_MENU;
    g->player.max_hp = (float)PLAYER_MAX_HP;
    g->player.hp = (float)PLAYER_MAX_HP;
    g->player.damage = BASE_DAMAGE;
    g->player.fire_rate = BASE_FIRE_RATE;
    g->player.fire_cooldown = 0.0f;
    g->player.speed = PLAYER_SPEED;
    g->player.x = GAME_W * 0.5f;
    g->player.y = GAME_H * 0.5f;
    g->player.last_hit_tick = 0;
    g->player.hp_regen_timer = 0.0f;
    g->enemy_head = NULL;
    g->loot_head = NULL;
    bullet_pool_clear(g);
}

static void game_reset_playing(Game *g) {
    entity_reset_all(g);
    g->state = GAME_STATE_PLAYING;
    g->tick = 0;
    g->wave = 1;
    g->wave_timer = 0.0f;
    g->spawned_this_wave = 0;
    g->to_spawn_this_wave = 10;
  g->boss_spawned = 0;
  g->spawn_cooldown = 0.0f;
  g->score = 0;

    g->player.hp = g->player.max_hp;
    g->player.damage = BASE_DAMAGE;
    g->player.fire_rate = BASE_FIRE_RATE;
    g->player.fire_cooldown = 0.0f;
    g->player.x = GAME_W * 0.5f;
    g->player.y = GAME_H * 0.5f;
    g->player.move_dx = 0;
    g->player.move_dy = 0;
    g->player.last_hit_tick = g->tick;
    g->player.hp_regen_timer = 0.0f;
}

/* ---------- Wave 系統：1~4 波大量敵人，第 5 波 Boss ---------- */

/** 本波總生成配額（會持續補怪直到達上限） */
static int wave_spawn_quota(int wave) {
  switch (wave) {
    case 1: return 10;
    case 2: return 24;
    case 3: return 48;
    case 4: return 80;
    default: return 0;
  }
}

/** 同場最大敵人數（Wave 4 營造包圍感） */
static int wave_max_on_field(int wave) {
  switch (wave) {
    case 1: return 10;
    case 2: return 20;
    case 3: return 38;
    case 4: return 55;
    default: return 0;
  }
}

/** 生成間隔（秒），波次越高越快 */
static float wave_spawn_interval(int wave) {
  switch (wave) {
    case 1: return 0.55f;
    case 2: return 0.38f;
    case 3: return 0.22f;
    case 4: return 0.14f;
    default: return 0.5f;
  }
}

/** HP = Base_HP + Wave * Growth_Factor */
static float wave_enemy_hp(int wave) {
  return NORMAL_ENEMY_BASE_HP + (float)wave * NORMAL_ENEMY_HP_GROWTH;
}

static float wave_enemy_speed(int wave) {
  return ENEMY_SPEED_BASE + (float)wave * 14.0f;
}

static void spawn_normal_enemy(Game *g) {
  Enemy *node;
  float x = randf(30.0f, GAME_W - 30.0f);
  float y = randf(30.0f, 140.0f);
  float hp = wave_enemy_hp(g->wave);
  node = enemy_list_push(g, x, y, hp, 0);
  if (node != NULL) {
    node->speed = wave_enemy_speed(g->wave);
  }
  g->spawned_this_wave++;
}

static void spawn_boss(Game *g) {
  Enemy *boss;
  if (g->boss_spawned) {
    return;
  }
  boss = enemy_list_push(g, GAME_W * 0.5f, 80.0f, (float)BOSS_HP, 1);
  if (boss != NULL) {
    boss->boss_pattern = 0;
    boss->boss_attack_cd = 1.2f;
  }
  g->boss_spawned = 1;
  g->spawned_this_wave = 1;
  g->to_spawn_this_wave = 1;
  fprintf(stderr, "[engine] Boss 生成 HP=%d\n", BOSS_HP);
}

static void wave_update(Game *g, float dt) {
  int on_field;
  int quota;
  int max_field;
  float interval;

  if (g->state != GAME_STATE_PLAYING) {
    return;
  }

  g->wave_timer += dt;

  if (g->wave < BOSS_WAVE) {
    quota = wave_spawn_quota(g->wave);
    max_field = wave_max_on_field(g->wave);
    interval = wave_spawn_interval(g->wave);
    g->to_spawn_this_wave = quota;
    on_field = enemy_list_count(g);

    g->spawn_cooldown -= dt;
    if (g->spawned_this_wave < quota && on_field < max_field && g->spawn_cooldown <= 0.0f) {
      spawn_normal_enemy(g);
      g->spawn_cooldown = interval;
    }

    if (g->spawned_this_wave >= quota && on_field == 0 && g->wave_timer > 0.5f) {
      g->wave++;
      g->wave_timer = 0.0f;
      g->spawned_this_wave = 0;
      g->spawn_cooldown = 0.0f;
      fprintf(stderr, "[engine] 進入第 %d 波\n", g->wave);
    }
  } else {
    if (!g->boss_spawned) {
      spawn_boss(g);
    }
    if (g->boss_spawned && enemy_list_count(g) == 0) {
      g->state = GAME_STATE_WIN;
      fprintf(stderr, "[engine] 勝利！\n");
    }
  }
}

/* ---------- 玩家自動射擊：瞄準最近敵人 ---------- */
static void player_auto_shoot(Game *g) {
  Player *p = &g->player;
  if (p->fire_cooldown > 0.0f) {
    return;
  }

  Enemy *target = enemy_find_nearest(g, p->x, p->y);
  if (target == NULL) {
    return;
  }

  float dx = target->x - p->x;
  float dy = target->y - p->y;
  float len = vec_len(dx, dy);
  if (len < 1e-3f) {
    return;
  }

  dx /= len;
  dy /= len;
  bullet_pool_spawn(g, p->x, p->y, dx * PLAYER_BULLET_SPEED, dy * PLAYER_BULLET_SPEED,
                    p->damage, BULLET_OWNER_PLAYER);
  p->fire_cooldown = 1.0f / p->fire_rate;
}

/* ---------- 敵人 AI：朝玩家移動；Boss 額外散彈 ---------- */
static void update_enemy_ai(Game *g, float dt) {
  Player *p = &g->player;
  Enemy *e = g->enemy_head;

  while (e != NULL) {
    float dx = p->x - e->x;
    float dy = p->y - e->y;
    float len = vec_len(dx, dy);
    if (len > 1e-3f) {
      e->x += (dx / len) * e->speed * dt;
      e->y += (dy / len) * e->speed * dt;
    }

    if (e->is_boss) {
      e->boss_attack_cd -= dt;
      if (e->boss_attack_cd <= 0.0f) {
        boss_attack_cycle(g, e);
      }
    }
    e = e->next;
  }
}

/** 模式 A：360° 環狀彈幕（18 發，每 20°） */
static void boss_attack_ring(Game *g, Enemy *boss) {
  int i;
  const float step = (2.0f * (float)M_PI) / (float)BOSS_RING_BULLETS;
  for (i = 0; i < BOSS_RING_BULLETS; i++) {
    float ang = step * (float)i;
    float vx = cosf(ang) * BOSS_BULLET_SPEED;
    float vy = sinf(ang) * BOSS_BULLET_SPEED;
    bullet_pool_spawn(g, boss->x, boss->y, vx, vy, BOSS_BULLET_DAMAGE, BULLET_OWNER_BOSS);
  }
  fprintf(stderr, "[engine] Boss 環狀彈幕 %d 發\n", BOSS_RING_BULLETS);
}

/** 模式 B：鎖定玩家的 15 發扇形追蹤散彈（速度較快） */
static void boss_attack_fan(Game *g, Enemy *boss) {
  Player *p = &g->player;
  float dx = p->x - boss->x;
  float dy = p->y - boss->y;
  float base = atan2f(dy, dx);
  const float spread = 1.1f; /* 總扇形約 126° */
  int i;

  for (i = 0; i < BOSS_FAN_BULLETS; i++) {
    float t = (float)i / (float)(BOSS_FAN_BULLETS - 1);
    float ang = base - spread * 0.5f + spread * t;
    float vx = cosf(ang) * BOSS_FAN_BULLET_SPEED;
    float vy = sinf(ang) * BOSS_FAN_BULLET_SPEED;
    bullet_pool_spawn(g, boss->x, boss->y, vx, vy, BOSS_BULLET_DAMAGE, BULLET_OWNER_BOSS);
  }
  fprintf(stderr, "[engine] Boss 追蹤散彈 -> (%.0f, %.0f)\n", p->x, p->y);
}

/** 在環狀 / 扇形兩種模式間輪換 */
static void boss_attack_cycle(Game *g, Enemy *boss) {
  if (boss->boss_pattern == 0) {
    boss_attack_ring(g, boss);
    boss->boss_attack_cd = BOSS_RING_CD;
    boss->boss_pattern = 1;
  } else {
    boss_attack_fan(g, boss);
    boss->boss_attack_cd = BOSS_FAN_CD;
    boss->boss_pattern = 0;
  }
}

static void update_bullets(Game *g, float dt) {
  int i;
  for (i = 0; i < MAX_BULLETS; i++) {
    if (!g->bullets[i].active) {
      continue;
    }
    g->bullets[i].x += g->bullets[i].vx * dt;
    g->bullets[i].y += g->bullets[i].vy * dt;
    if (g->bullets[i].x < -50.0f || g->bullets[i].x > GAME_W + 50.0f ||
        g->bullets[i].y < -50.0f || g->bullets[i].y > GAME_H + 50.0f) {
      g->bullets[i].active = 0;
    }
  }
}

/**
 * 邊際效益遞減：越接近上限，每次 Buff 增加越少
 */
static void player_register_hit(Game *g) {
  g->player.last_hit_tick = g->tick;
  g->player.hp_regen_timer = REGEN_INTERVAL_SEC;
}

static void player_apply_hp_regen(Game *g, float dt) {
  Player *p = &g->player;

  if (p->hp >= p->max_hp) {
    return;
  }
  if (g->tick - p->last_hit_tick <= (unsigned long)REGEN_COOLDOWN_TICKS) {
    return;
  }

  p->hp_regen_timer -= dt;
  if (p->hp_regen_timer <= 0.0f) {
    p->hp += REGEN_HP_PER_SEC;
    if (p->hp > p->max_hp) {
      p->hp = p->max_hp;
    }
    p->hp_regen_timer = REGEN_INTERVAL_SEC;
  }
}

static void apply_loot_buff(Player *p, LootType t) {
  if (t == LOOT_DAMAGE_UP) {
    float room = MAX_PLAYER_DAMAGE - p->damage;
    if (room > 0.0f) {
      p->damage += room * 0.14f;
      if (p->damage > MAX_PLAYER_DAMAGE) {
        p->damage = MAX_PLAYER_DAMAGE;
      }
    }
    fprintf(stderr, "[engine] Buff 傷害(遞減) -> %.1f\n", p->damage);
  } else if (t == LOOT_FIRE_RATE_UP) {
    float room = MAX_PLAYER_FIRE_RATE - p->fire_rate;
    if (room > 0.0f) {
      p->fire_rate += room * 0.16f;
      if (p->fire_rate > MAX_PLAYER_FIRE_RATE) {
        p->fire_rate = MAX_PLAYER_FIRE_RATE;
      }
    }
    fprintf(stderr, "[engine] Buff 射速(遞減) -> %.1f\n", p->fire_rate);
  }
}

/* ---------- 碰撞：子彈↔敵人、Boss彈↔玩家、玩家↔Loot ---------- */
static void resolve_collisions(Game *g) {
  int i;
  Player *p = &g->player;

  for (i = 0; i < MAX_BULLETS; i++) {
    Bullet *b = &g->bullets[i];
    Enemy *e;
    float er, dx, dy, dist2;

    if (!b->active) {
      continue;
    }

    if (b->owner == BULLET_OWNER_PLAYER) {
      e = g->enemy_head;
      while (e != NULL) {
        er = e->is_boss ? BOSS_RADIUS : ENEMY_RADIUS;
        dx = b->x - e->x;
        dy = b->y - e->y;
        dist2 = dx * dx + dy * dy;
        if (dist2 <= (er + BULLET_RADIUS) * (er + BULLET_RADIUS)) {
          b->active = 0;
          e->hp -= b->damage;
          if (e->hp <= 0.0f) {
            LootType lt =
                (rand() % 2 == 0) ? LOOT_DAMAGE_UP : LOOT_FIRE_RATE_UP;
            loot_list_push(g, e->x, e->y, lt);
            g->score += e->is_boss ? 2000 : 10;
            {
              Enemy *dead = e;
              e = e->next; /* 先取 next，remove 後 e 會失效 */
              enemy_list_remove_node(g, dead);
              continue;
            }
          }
          break;
        }
        e = e->next;
      }
    } else if (b->owner == BULLET_OWNER_BOSS) {
      dx = b->x - p->x;
      dy = b->y - p->y;
      dist2 = dx * dx + dy * dy;
      if (dist2 <= (PLAYER_RADIUS + BULLET_RADIUS) * (PLAYER_RADIUS + BULLET_RADIUS)) {
        b->active = 0;
        p->hp -= b->damage;
        player_register_hit(g);
        if (p->hp <= 0.0f) {
          p->hp = 0.0f;
          g->state = GAME_STATE_GAME_OVER;
          fprintf(stderr, "[engine] Game Over\n");
        }
      }
    }
  }

  /* 玩家與敵人接觸傷害 */
  {
    Enemy *e = g->enemy_head;
    while (e != NULL) {
      float er = e->is_boss ? BOSS_RADIUS : ENEMY_RADIUS;
      float dx = p->x - e->x;
      float dy = p->y - e->y;
      if (dx * dx + dy * dy <= (er + PLAYER_RADIUS) * (er + PLAYER_RADIUS)) {
        p->hp -= 30.0f * TICK_DT;
        player_register_hit(g);
        if (p->hp <= 0.0f) {
          p->hp = 0.0f;
          g->state = GAME_STATE_GAME_OVER;
        }
      }
      e = e->next;
    }
  }

  /* 玩家撿取 Loot */
  {
    Loot *l = g->loot_head;
    Loot *lnext;
    while (l != NULL) {
      float dx = p->x - l->x;
      float dy = p->y - l->y;
      lnext = l->next;
      if (dx * dx + dy * dy <= (LOOT_RADIUS + PLAYER_RADIUS) * (LOOT_RADIUS + PLAYER_RADIUS)) {
        apply_loot_buff(p, l->type);
        loot_list_remove_node(g, l);
      }
      l = lnext;
    }
  }
}

static void game_update(Game *g, float dt) {
  if (g->state == GAME_STATE_PLAYING) {
    Player *p = &g->player;

    /* 玩家位置由前端 POST (x,y) 權威更新，此處不再用 dx/dy 積分移動 */

    if (p->fire_cooldown > 0.0f) {
      p->fire_cooldown -= dt;
    }

    player_apply_hp_regen(g, dt);

    wave_update(g, dt);
    player_auto_shoot(g);
    update_enemy_ai(g, dt);
    update_bullets(g, dt);
    resolve_collisions(g);
  }
  g->tick++;
}

/* ---------- 輸出單行 JSON（僅 stdout） ---------- */
static const char *state_name(GameState s) {
  switch (s) {
    case GAME_STATE_MENU: return "MENU";
    case GAME_STATE_PLAYING: return "PLAYING";
    case GAME_STATE_GAME_OVER: return "GAME_OVER";
    case GAME_STATE_WIN: return "WIN";
    default: return "UNKNOWN";
  }
}

static void emit_state_json(const Game *g) {
  static char buf[JSON_BUF_SIZE];
  int pos = 0;
  const Player *p = &g->player;
  const Enemy *e;
  const Loot *l;
  int i;
  int first;

  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                  "{\"state\":\"%s\",\"wave\":%d,\"tick\":%lu,\"score\":%d,"
                  "\"tickRate\":%d,"
                  "\"player\":{\"x\":%.1f,\"y\":%.1f,\"hp\":%.1f,\"maxHp\":%.1f,"
                  "\"damage\":%.1f,\"fireRate\":%.1f,\"speed\":%.1f,"
                  "\"dx\":%d,\"dy\":%d},"
                  "\"enemies\":[",
                  state_name(g->state), g->wave, g->tick, g->score,
                  TICK_RATE_HZ,
                  p->x, p->y, p->hp, p->max_hp, p->damage, p->fire_rate,
                  p->speed, p->move_dx, p->move_dy);

  first = 1;
  e = g->enemy_head;
  while (e != NULL) {
    if (!first) {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
    }
    first = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"id\":%d,\"x\":%.1f,\"y\":%.1f,\"hp\":%.1f,\"maxHp\":%.1f,"
                    "\"boss\":%d,\"speed\":%.1f}",
                    e->id, e->x, e->y, e->hp, e->max_hp, e->is_boss, e->speed);
    e = e->next;
    if (pos >= JSON_BUF_SIZE - 256) {
      break;
    }
  }

  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "],\"bullets\":[");

  first = 1;
  for (i = 0; i < MAX_BULLETS; i++) {
    if (!g->bullets[i].active) {
      continue;
    }
    if (!first) {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
    }
    first = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"id\":%d,\"x\":%.1f,\"y\":%.1f,\"vx\":%.1f,\"vy\":%.1f,\"owner\":%d}",
                    g->bullets[i].id, g->bullets[i].x, g->bullets[i].y,
                    g->bullets[i].vx, g->bullets[i].vy,
                    (int)g->bullets[i].owner);
    if (pos >= JSON_BUF_SIZE - 256) {
      break;
    }
  }

  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "],\"loot\":[");

  first = 1;
  l = g->loot_head;
  while (l != NULL) {
    if (!first) {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
    }
    first = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"id\":%d,\"x\":%.1f,\"y\":%.1f,\"type\":%d}",
                    l->id, l->x, l->y, (int)l->type);
    l = l->next;
    if (pos >= JSON_BUF_SIZE - 256) {
      break;
    }
  }

  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");

  /* 協定前綴方便 Python 解析 */
  printf("STATE %s\n", buf);
  fflush(stdout);
}

static void sleep_tick(void) {
#ifdef _WIN32
  Sleep((DWORD)(TICK_DT * 1000.0f));
#else
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = (long)(TICK_DT * 1e9f);
  nanosleep(&ts, NULL);
#endif
}

int main(void) {
  Game game;
  char line[INPUT_BUF_SIZE];

  srand((unsigned)time(NULL));
  game_init(&game);

  fprintf(stderr, "[engine] C 彈幕引擎啟動 (subprocess JSON 協定)\n");
  fprintf(stderr, "[engine] 等待 stdin: {\"action\":\"start\"}\n");

  while (1) {
    if (poll_stdin_line(line, sizeof(line))) {
      parse_input_line(&game, line);
    }

    game_update(&game, TICK_DT);
    emit_state_json(&game);
    sleep_tick();
  }

  return 0;
}
