#ifndef FABPACK_H
#define FABPACK_H
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char id[128],cat[16],title[512];
    long offset,length,image;
} fab_entry_t;
typedef struct {
    fab_entry_t *entries;
    int count;
    long base;
    char path[640];
} fab_pack_t;
bool fab_pack_load(fab_pack_t *out,const char *path);
void fab_pack_free(fab_pack_t *p);
#endif
