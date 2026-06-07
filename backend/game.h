/**
 * game.h - 彈幕肉鴿遊戲核心資料結構與常數定義
 * 期末報告重點：指標、鏈結串列、固定大小陣列的記憶體配置策略
 */
#ifndef BULLETHELL_GAME_H
#define BULLETHELL_GAME_H

#include <stddef.h>

/* ========== 邏輯世界尺寸（與前端 Canvas 對齊，單位：像素） ========== */
#define GAME_W           800.0f
#define GAME_H           600.0f

/* ========== 實體數量上限 ========== */
#define MAX_BULLETS      512   /* 子彈池：Boss 環彈 + 大量彈幕需要較大池 */
#define PLAYER_MAX_HP    100
#define BOSS_HP          5000
#define TICK_DT          (1.0f / 60.0f)  /* 固定 tick：每秒 60 次邏輯更新 */
#define TICK_RATE_HZ     60

/* ========== 碰撞半徑（圓形近似，簡化 AABB 運算） ========== */
#define PLAYER_RADIUS    18.0f
#define ENEMY_RADIUS     20.0f
#define BOSS_RADIUS      36.0f
#define BULLET_RADIUS    5.0f
#define LOOT_RADIUS      14.0f

/* ========== 波次與生成參數 ========== */
#define MAX_WAVE         5
#define BOSS_WAVE        5
/* 小怪 HP：Base + Wave * Growth（Wave1 約 2~3 發擊殺） */
#define NORMAL_ENEMY_BASE_HP     9.0f
#define NORMAL_ENEMY_HP_GROWTH   3.5f
#define ENEMY_SPEED_BASE         75.0f
#define BOSS_SPEED            55.0f
#define BOSS_RING_CD          2.4f   /* 模式 A：環狀彈幕冷卻 */
#define BOSS_FAN_CD           1.6f   /* 模式 B：追蹤散彈冷卻 */
#define BOSS_BULLET_SPEED     240.0f
#define BOSS_FAN_BULLET_SPEED 340.0f
#define BOSS_RING_BULLETS     18
#define BOSS_FAN_BULLETS      15
#define BOSS_BULLET_DAMAGE    14.0f

#define PLAYER_SPEED          260.0f
#define PLAYER_BULLET_SPEED   400.0f
#define BASE_DAMAGE           3.5f   /* 玩家基礎傷害（削弱） */
#define BASE_FIRE_RATE        3.5f
#define MAX_PLAYER_DAMAGE     22.0f  /* 傷害上限（遞減 Buff） */
#define MAX_PLAYER_FIRE_RATE  8.0f

/* 脫戰回血：3 秒未受傷後，每秒恢復 REGEN_HP_PER_SEC 點生命 */
#define REGEN_COOLDOWN_TICKS  (3 * TICK_RATE_HZ)
#define REGEN_HP_PER_SEC      8.0f
#define REGEN_INTERVAL_SEC    1.0f

/* ========== 遊戲狀態機（與前端 overlay 對應） ========== */
typedef enum {
    GAME_STATE_MENU = 0,       /* 主選單，等待開始 */
    GAME_STATE_PLAYING,        /* 進行中 */
    GAME_STATE_GAME_OVER,      /* 玩家 HP 歸零 */
    GAME_STATE_WIN             /* 第 5 波 Boss 擊敗 */
} GameState;

/* ========== 子彈歸屬（碰撞時區分傷害對象） ========== */
typedef enum {
    BULLET_OWNER_PLAYER = 0,
    BULLET_OWNER_BOSS
} BulletOwner;

/* ========== 掉落物種類（撿取後套用 Buff） ========== */
typedef enum {
    LOOT_DAMAGE_UP = 0,        /* 提升玩家傷害 */
    LOOT_FIRE_RATE_UP          /* 提升射速 */
} LootType;

/**
 * Player - 玩家本體（單一 struct，不用鏈結串列）
 * 由 main.c 每 tick 依 stdin 輸入更新位置，並驅動自動射擊計時。
 */
typedef struct {
    float x;                   /* 世界座標 X */
    float y;                   /* 世界座標 Y */
    float hp;                  /* 當前生命值 */
    float max_hp;              /* 最大生命值（用於 UI 顯示比例） */
    float damage;              /* 子彈傷害（可被 Loot Buff 疊加） */
    float fire_rate;           /* 每秒射擊次數（可被 Loot Buff 疊加） */
    float fire_cooldown;       /* 距離下次射擊剩餘秒數，<=0 時可發射 */
    float speed;               /* 移動速度（像素/秒） */
    int   move_dx;             /* 輸入：水平 -1, 0, 1（八方向，C 端會正規化） */
    int   move_dy;             /* 輸入：垂直 -1, 0, 1 */
    unsigned long last_hit_tick; /* 最後受傷的邏輯 tick（脫戰回血用） */
    float hp_regen_timer;      /* 回血計時器（秒） */
} Player;

/**
 * Enemy - 敵人節點（鏈結串列，動態 malloc / free）
 *
 * next 指標將多個敵人串成單向鏈結串列，head 保存在 Game.enemy_head。
 * 生成時 enemy_list_push() 以頭插法 malloc 新節點；擊殺或重置時
 * enemy_list_remove_node() / enemy_list_clear() 逐節點 free，避免 memory leak。
 * （與 Flappy Bird 水管 Queue 相同核心：鏈結串列 + 指標串接管理動態實體生命週期）
 *
 * is_boss 為 1 時表示第 5 波 Boss（體型大、HP 高、會發射散彈）。
 */
typedef struct Enemy {
    int   id;                  /* 全域唯一 ID，供前端 Map 追蹤 */
    float x, y;
    float hp;
    float max_hp;
    float speed;
    int   is_boss;             /* 1 = Boss，0 = 一般敵人 */
    float boss_attack_cd;      /* 僅 Boss：距離下次攻擊的冷卻 */
    int   boss_pattern;        /* 0=環狀彈幕, 1=追蹤散彈（交替切換） */
    struct Enemy *next;        /* 指向下一個敵人節點，尾端為 NULL */
} Enemy;

/**
 * Bullet - 子彈槽位（固定大小陣列中的一格）
 * active 為 0 表示此槽位閒置，可重複使用，不需 free（避免碎片 malloc）。
 */
typedef struct {
    int   id;                    /* 全域唯一 ID（生成時分配，槽位重用時不變直到 deactivate） */
    int   active;                /* 1 = 使用中，0 = 空槽 */
    float x, y;
    float vx, vy;                /* 速度向量（像素/秒） */
    float damage;
    BulletOwner owner;
} Bullet;

/**
 * Loot - 掉落物節點（鏈結串列）
 * 敵人死亡時 malloc 新節點插入鏈結串列；玩家撿取後 free。
 */
typedef struct Loot {
    int   id;                    /* 全域唯一 ID */
    float x, y;
    LootType type;
    struct Loot *next;
} Loot;

/**
 * Game - 全域遊戲上下文（main.c 持有單一實例）
 * enemy_head / loot_head 為鏈結串列頭指標； bullets 為固定陣列。
 */
typedef struct {
    GameState state;
    unsigned long tick;          /* 邏輯帧計數，供除錯與 JSON 輸出 */

    int   wave;                  /* 當前波次 1~5 */
    float wave_timer;            /* 波次內計時 */
    int   spawned_this_wave;     /* 本波已生成敵人數 */
    int   to_spawn_this_wave;    /* 本波預定生成總數 */
    int   boss_spawned;          /* 第 5 波 Boss 是否已生成 */
    float spawn_cooldown;        /* 距離下次生成敵人的剩餘秒數 */

    Player player;
    Enemy *enemy_head;           /* 敵人鏈結串列頭指標 */
    Loot  *loot_head;            /* 掉落物鏈結串列頭指標 */
    Bullet bullets[MAX_BULLETS];

    int   score;
} Game;

/* 鏈結串列與子彈池操作宣告見 entity.h（由 main.c / entity.c 引入） */

#endif /* BULLETHELL_GAME_H */
