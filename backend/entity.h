/**
 * entity.h - 實體（敵人/掉落/子彈）記憶體管理介面
 */
#ifndef ENTITY_H
#define ENTITY_H

#include "game.h"

/* ----- 敵人鏈結串列 ----- */
Enemy *enemy_list_push(Game *game, float x, float y, float hp, int is_boss);
void   enemy_list_remove_node(Game *game, Enemy *target);
void   enemy_list_clear(Game *game);
int    enemy_list_count(const Game *game);
Enemy *enemy_find_nearest(const Game *game, float px, float py);

/* ----- 掉落物鏈結串列 ----- */
Loot *loot_list_push(Game *game, float x, float y, LootType type);
void  loot_list_remove_node(Game *game, Loot *target);
void  loot_list_clear(Game *game);

/* ----- 子彈固定陣列池 ----- */
void bullet_pool_clear(Game *game);
int  bullet_pool_spawn(Game *game, float x, float y, float vx, float vy,
                       float damage, BulletOwner owner);

/* ----- 重置時一次釋放所有動態記憶體 ----- */
void entity_id_reset(void);
void entity_reset_all(Game *game);

#endif /* ENTITY_H */
