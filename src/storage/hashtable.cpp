#include <stdlib.h>
#include <assert.h>
#include "hashtable.h"

const size_t k_max_load_factor = 8;
const size_t k_max_rehashing = 128;

static void h_init(HTable* htable, size_t n){
    assert(n > 0 && ((n - 1) & n) == 0);
    htable->table = (HNode**)calloc(n, sizeof(HNode*));
    htable->mask = n - 1;
    htable->size = 0;
}

static void h_insert(HTable* htable, HNode* node){
    size_t pos = node->hash & htable->mask;
    node->next = htable->table[pos];
    htable->table[pos] = node;
    htable->size++;
}

static HNode** h_lookup(HTable* htable, HNode* node, bool (*eq)(HNode*, HNode*)){
    if(!htable->table) return nullptr;

    size_t pos = node->hash & htable->mask;
    HNode** from = &htable->table[pos];
    for(HNode* cur; (cur = *from) != nullptr; *from = cur->next){
        if(eq(node, *from)) return from;
    }

    return nullptr;
}

static HNode* h_detach(HTable* htable, HNode** from){
    HNode* node = *from;
    **from = &node->next;
    htable->size--;
    return node;
}

static void hm_trigger_rehashing(HMap* hmap){
    hmap->newer = hmap->older;
    h_init(&hmap->older, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

HNode* hm_lookup(HMap* hmap, HNode* node, bool (*eq)(HNode*, HNode*)){
    HNode** from = h_lookup(&hmap->newer, node, eq);
    if(!*from){
        from = h_lookup(&hmap->older, node, eq);
    }

    return *from ? *from : nullptr;
}

void hm_migrate_keys(HMap* hmap){
    size_t nwork = 0;
    while(nwork < k_max_rehashing && hmap->older.size > 0){
        HNode** from = &hmap->older.table[hmap->newer.migrate_pos];
        if(!*from){
            hmap->newer.migrate_pos++;
            continue;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, *from));
        nwork++;
    }

    if(hmap->older.size == 0 && hmap->older.table){
        free(&hmap->older.table);
        hmap->older = HTable{};
    }
}

void hm_insert(HMap* hmap, HNode* node){
    if(!hmap->newer.table) h_init(&hmap->newer, 4);

    h_insert(&hmap->newer, node);
    if(!hmap->older.table){
        size_t threshold = (hmap->older.mask + 1) * k_max_load_factor;
        if(hmap->older.size >= threshold){
            hm_trigger_rehashing(hmap);
        }
    }

    hm_migrate_keys(hmap);
}

void hm_clear(HMap* hmap){
    free(&hmap->older.table);
    free(&hmap->newer.table);
    hmap = HMap{};
}

size_t hm_size(HMap* hmap){
    return hmap->older.size + hmap->newer.size;
}