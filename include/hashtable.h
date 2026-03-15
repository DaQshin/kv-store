#pragma once

#include <stddef.h>
#include <stdint.h>

struct HNode{
    HNode* next = nullptr;
    uint64_t hash = 0;
};

struct HTable {
    HNode** table = nullptr;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTable older;
    HTable newer;
    size_t migrate_pos = 0;
};

HNode* hm_lookup(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*));
void hm_insert(HMap* hmap, HNode* node);
HNode* hm_delete(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*));
void hm_clear(HMap* hmap);
void hm_size(HMap* hmap);


