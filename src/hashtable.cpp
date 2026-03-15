#include <stdlib.h>
#include <assert.h>
#include "../include/hashtable.h"

const size_t k_max_load_factor = 8;
const size_t k_rehashing_work = 128; 

static void h_init(HTable* htable, size_t size){
    assert(n > 0 && ((n - 1) & n) == 0)
    htable->table = (HNode**)calloc(n, sizeof(HNode*));
    htable->mask = n - 1;
    htable->size = 0;
} 

static void h_insert(HTable* htable, HNode* node){
    size_t key = htable->mask & node->hcode;
    HNode* next = htable->table[key];
    node->next = next;
    htable->table[pos] = node;
    htable->size++;
}

static HNode** h_lookup(HTable* htable, HNode* node, bool (*eq)(HNode*, HNode*)){
    if(!htable->table) return nullptr;
    size_t key = htable->mask & node->hcode;
    HNode** from = &htable->table[key];
    for(HNode* cur; (cur = *from) != nullptr; from = &cur->next){
        if(cur->hcode == key->hcode && eq(key, cur)) return from;
    }   

    return nullptr;
}

static HNode* h_detach(HTable* htable, HNode** from){
    HNode* node = *from;
    *from = node->next;
    htable->size--;
    return node;
}

static void hm_trigger_rehashing(HMap* hmap){
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

HNode* hm_lookup(Hmap* hmap, HNode* node, bool (eq*)(HNode*, HNode*)){
    HNode** from = h_lookup(&hmap->newer, node, eq);
    if(!from){
        from = h_lookup(&hmap->older, node, eq);
    }

    return from ? *from : nullptr;
}


HNode* hm_delete(Hmap* hmap, HNode* node, bool (eq*)(HNode*, HNode*)){
    if(h_lookup(&hmap->newer, node, eq)){
        return h_detach(&hmap->newer, node);
    }

    return h_detach(&hmap->older, node);
}

void hm_insert(HMap* hmap, HNode* node){
    if(!hmap->newer.table){
        h_init(&hmap->newer, 4);
    }

    h_insert(&hmap->newer, node);
    if(!hmap->older.table){
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if(hmap->newer.size >= threshold){
            hm_trigger_rehashing(hmap);
        }
    }
}

static hm_migrate_keys(HMap* hmap){
    size_t nwork = 0;
    while(nwork < k_rehashing_work && hmap->older.size > 0){

        HNode** from = &hamp->older.table[hmap->migrate_pos];

        if(!*from){
            hmap->migrate_pos++;
            continue;
        }

        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }

    if(hmap->older.size == 0 && hmap->older.table){
        free(hmap->older.table);
        hmap->older = HTable{};
    }
}

void hm_clear(HMap* hmap){
    free(hmap->newer.table);
    free(hmap->older.table);
    *hmap = HMap{};
}

size_t hm_size(HMap *hmap) {
    return hmap->newer.size + hmap->older.size;
}