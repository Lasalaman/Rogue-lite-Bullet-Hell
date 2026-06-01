/**
 * entity.c - Enemy / Loot 鏈結串列記憶體管理，以及 Bullet 固定陣列池
 *
 * 期末報告重點：
 *   1. malloc 建立節點後，必須接到鏈結串列上，否則成為孤兒造成 leak
 *   2. free 前必須先從鏈結串列上摘下節點，否則 head 仍指向已釋放記憶體（dangling pointer）
 *   3. 清空時遍歷整條鏈，逐節點 free，最後 head = NULL
 */
#include "entity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 實體 ID 計數器（遞增；遊戲重置時歸 1） */
static int s_next_enemy_id = 1;
static int s_next_loot_id = 1;
static int s_next_bullet_id = 1;

void entity_id_reset(void) {
    s_next_enemy_id = 1;
    s_next_loot_id = 1;
    s_next_bullet_id = 1;
}

/* ========== 敵人鏈結串列 ========== */

/**
 * enemy_list_push - 在鏈結串列「頭端」插入新敵人（頭插法）
 *
 * 指標串接示意：
 *   插入前:  game->enemy_head --> [A] --> [B] --> NULL
 *   插入後:  game->enemy_head --> [NEW] --> [A] --> [B] --> NULL
 *              new_node->next 指向原本的 head
 */
Enemy *enemy_list_push(Game *game, float x, float y, float hp, int is_boss) {
    Enemy *node = (Enemy *)malloc(sizeof(Enemy));
    if (node == NULL) {
        return NULL;
    }

    node->id = s_next_enemy_id++;
    node->x = x;
    node->y = y;
    node->hp = hp;
    node->max_hp = hp;
    node->speed = is_boss ? BOSS_SPEED : ENEMY_SPEED_BASE;
    node->is_boss = is_boss ? 1 : 0;
    node->boss_attack_cd = 1.0f; /* Boss 生成後短暫延遲再攻擊 */
    node->boss_pattern = 0;

    /* 頭插：新節點的 next 指向舊 head，再讓 head 指向新節點 */
    node->next = game->enemy_head;
    game->enemy_head = node;

    return node;
}

/**
 * enemy_list_remove_node - 從鏈結串列中移除指定節點並 free
 *
 * 需處理兩種情況：
 *   (1) 刪除頭節點：head 改為 head->next
 *   (2) 刪除中間/尾端：prev->next = target->next
 */
void enemy_list_remove_node(Game *game, Enemy *target) {
    if (game == NULL || target == NULL) {
        return;
    }

    Enemy *cur = game->enemy_head;
    Enemy *prev = NULL;

    while (cur != NULL) {
        if (cur == target) {
            if (prev == NULL) {
                /* 刪除的是頭節點 */
                game->enemy_head = cur->next;
            } else {
                /* 跳過 cur，把 prev 接到 cur 的下一個 */
                prev->next = cur->next;
            }
            cur->next = NULL; /* 斷開連結，避免懸空引用 */
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/**
 * enemy_list_clear - 釋放整條敵人鏈結串列（遊戲重置 / 結束時呼叫）
 */
void enemy_list_clear(Game *game) {
    if (game == NULL) {
        return;
    }

    Enemy *cur = game->enemy_head;
    while (cur != NULL) {
        Enemy *next = cur->next; /* 先存 next，free 後就無法讀取 */
        cur->next = NULL;
        free(cur);
        cur = next;
    }
    game->enemy_head = NULL;
}

int enemy_list_count(const Game *game) {
    int n = 0;
    const Enemy *cur = game->enemy_head;
    while (cur != NULL) {
        n++;
        cur = cur->next;
    }
    return n;
}

/**
 * enemy_find_nearest - 遍歷鏈結串列，找距離 (px,py) 最近的敵人（自動瞄準用）
 */
Enemy *enemy_find_nearest(const Game *game, float px, float py) {
    Enemy *best = NULL;
    float best_d2 = 1e30f;
    Enemy *cur = game->enemy_head;

    while (cur != NULL) {
        float dx = cur->x - px;
        float dy = cur->y - py;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = cur;
        }
        cur = cur->next;
    }
    return best;
}

/* ========== 掉落物鏈結串列 ========== */

Loot *loot_list_push(Game *game, float x, float y, LootType type) {
    Loot *node = (Loot *)malloc(sizeof(Loot));
    if (node == NULL) {
        return NULL;
    }

    node->id = s_next_loot_id++;
    node->x = x;
    node->y = y;
    node->type = type;
    node->next = game->loot_head;
    game->loot_head = node;

    return node;
}

void loot_list_remove_node(Game *game, Loot *target) {
    if (game == NULL || target == NULL) {
        return;
    }

    Loot *cur = game->loot_head;
    Loot *prev = NULL;

    while (cur != NULL) {
        if (cur == target) {
            if (prev == NULL) {
                game->loot_head = cur->next;
            } else {
                prev->next = cur->next;
            }
            cur->next = NULL;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

void loot_list_clear(Game *game) {
    if (game == NULL) {
        return;
    }

    Loot *cur = game->loot_head;
    while (cur != NULL) {
        Loot *next = cur->next;
        cur->next = NULL;
        free(cur);
        cur = next;
    }
    game->loot_head = NULL;
}

/* ========== 子彈固定陣列池（無 malloc，僅標記 active） ========== */

void bullet_pool_clear(Game *game) {
    if (game == NULL) {
        return;
    }
    memset(game->bullets, 0, sizeof(game->bullets));
}

int bullet_pool_spawn(Game *game, float x, float y, float vx, float vy,
                      float damage, BulletOwner owner) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!game->bullets[i].active) {
            game->bullets[i].active = 1;
            game->bullets[i].id = s_next_bullet_id++;
            game->bullets[i].x = x;
            game->bullets[i].y = y;
            game->bullets[i].vx = vx;
            game->bullets[i].vy = vy;
            game->bullets[i].damage = damage;
            game->bullets[i].owner = owner;
            return 1;
        }
    }
    return 0; /* 池滿，略過本次發射 */
}

void entity_reset_all(Game *game) {
    entity_id_reset();
    enemy_list_clear(game);
    loot_list_clear(game);
    bullet_pool_clear(game);
}
