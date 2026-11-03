#include "fabpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "src/system/core/json.h"

void fab_pack_free(fab_pack_t *p) {free(p->entries);memset(p,0,sizeof(*p));}
bool fab_pack_load(fab_pack_t *out,const char *path) {
    fab_pack_t p={0}; FILE*f=fopen(path,"rb");if(!f)return false;
    struct stat st; char header[48]; unsigned long n=0;char extra;
    if(fstat(fileno(f),&st) || st.st_size>64*1024*1024 || !fgets(header,sizeof(header),f) ||
       sscanf(header,"FAB1 %lu %c",&n,&extra)!=1 || !n || n>1024*1024) {fclose(f);return false;}
    p.base=ftell(f)+(long)n;
    if(p.base>st.st_size){fclose(f);return false;}
    char *text=malloc(n+1);if(!text){fclose(f);return false;}
    bool ok=fread(text,1,n,f)==n;fclose(f);text[n]=0;
    json_doc_t d={0};ok=ok && !memchr(text,0,n) && json_parse(text,&d);
    if(!ok){free(text);return false;}
    int root=json_root(&d);p.count=json_len(&d,root);
    ok=root>=0 && d.toks[root].type==JSON_ARRAY && p.count<=5000;
    p.entries=calloc(p.count?p.count:1,sizeof(*p.entries));ok=ok && p.entries;
    for(int i=0;ok && i<p.count;i++) {
        fab_entry_t *e=&p.entries[i];int item=json_at(&d,root,i);
        ok=json_obj_str(&d,item,"id",e->id,sizeof(e->id)) &&
           json_obj_str(&d,item,"cat",e->cat,sizeof(e->cat)) &&
           json_obj_str(&d,item,"title",e->title,sizeof(e->title));
        e->offset=json_obj_long(&d,item,"offset",-1);
        e->length=json_obj_long(&d,item,"length",-1);
        e->image=json_obj_long(&d,item,"image",-1);
        ok=ok && e->id[0] && e->length>0 && e->length<=512*1024 && e->image>=0 && e->image<=512*1024 &&
           e->offset>=0 && e->offset<=st.st_size-p.base &&
           e->length+e->image<=st.st_size-p.base-e->offset;
        if(i>0)ok=ok && e->offset>=p.entries[i-1].offset+p.entries[i-1].length+p.entries[i-1].image;
    }
    json_free(&d);free(text);
    if(!ok){fab_pack_free(&p);return false;}
    snprintf(p.path,sizeof(p.path),"%s",path);*out=p;return true;
}
