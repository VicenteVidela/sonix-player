#include "fabularium.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/meloetta/fabpack.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

lv_obj_t *fabularium_screen;
static gui_config_t *cfg;
static char dir[512],server[180];
static fab_pack_t packs[2];
static const char *pack_names[]={"library","readings"};
static const char *cats[]={"readings","cards","stones","herbs"};
static const char *tags[]={"fab_readings","fab_cards","fab_stones","fab_herbs"};
static lv_obj_t *col,*home_grid,*heading,*status,*search_layer,*search_field,*search_btn;
static keyboard_t *keyboard;
static cover_image_t picture;
static int category=-1,page,depth,deck=-1,suit=-1;
static const char *suit_ids[]={"major-","wands-","cups-","swords-","pentacles-"};
static const char *suit_tags[]={"fab_major","fab_wands","fab_cups","fab_swords","fab_pentacles"};
static char current[128],history[16][128],query[128];
static bool detail,searching,busy,failed;
static unsigned generation;
static time_t last_attempt;
static char export_date[64];
static void rebuild(void);
static void open_entry(const char *id);
static bool tapping(void){return !player_sheet_drag_active()&&!switcher_back_drag_active();}

static bool ensure_dir(void) {
    if(!cfg->sd_root_path || !gui_card_available())return false;
    snprintf(dir,sizeof(dir),"%s/Fabularium",cfg->sd_root_path);
    return mkdir(dir,0755)==0 || errno==EEXIST;
}
static void clear_view(void) {
    generation++;lv_obj_clean(col);cover_free(&picture);
}
static void load_packs(void) {
    for(int i=0;i<2;i++) {
        fab_pack_free(&packs[i]);char path[640];
        snprintf(path,sizeof(path),"%s/%s.fab",dir,pack_names[i]);
        fab_pack_load(&packs[i],path);
    }
}
static void refresh_status(void) {
    char text[180];
    if(busy)snprintf(text,sizeof(text),"%s",tr("fab_syncing"));
    else if(failed)snprintf(text,sizeof(text),"%s",tr(packs[0].count?"fab_offline":"fab_empty"));
    else if(export_date[0])snprintf(text,sizeof(text),tr("fab_updated"),export_date);
    else snprintf(text,sizeof(text),"%s",tr(packs[0].count?"fab_cached":"fab_empty"));
    lv_label_set_text(status,text);
}
typedef struct {
    char dir[512],server[180],date[64],hash[2][17];
    bool changed[2],ok,manual;
} sync_job_t;
static bool valid_hash(const char *s) {
    if(strlen(s)!=16)return false;
    for(int i=0;i<16;i++)if(!strchr("0123456789abcdef",s[i]))return false;
    return true;
}
static bool fetch_pack(sync_job_t *j,int i,long size) {
    char path[640],url[256],hashpath[640],old[32]="";
    snprintf(path,sizeof(path),"%s/%s.fab",j->dir,pack_names[i]);
    snprintf(hashpath,sizeof(hashpath),"%s/%s.hash",j->dir,pack_names[i]);
    FILE *f=fopen(hashpath,"r");if(f){fgets(old,sizeof(old),f);fclose(f);}
    struct stat st;
    if(!strcmp(old,j->hash[i]) && stat(path,&st)==0 && st.st_size==size) {
        fab_pack_t check={0};bool good=fab_pack_load(&check,path);fab_pack_free(&check);
        if(good)return true;
    }
    snprintf(path,sizeof(path),"%s/%s.new",j->dir,pack_names[i]);
    snprintf(url,sizeof(url),"%s/%s.fab",j->server,j->hash[i]);
    http_stream_t stream;
    if(!http_stream_open(&stream,url,15))return false;
    bool ok=stream.content_length==size;
    f=ok?fopen(path,"wb"):NULL;
    if(!f){http_stream_close(&stream);return false;}
    uint64_t hash=14695981039346656037ULL;
    char buf[8192];long got=0;
    while(ok && got<size) {
        int want=(int)((size-got)<(long)sizeof(buf)?size-got:(long)sizeof(buf));
        int n=http_stream_read(&stream,buf,want);
        if(n<=0){ok=false;break;}
        for(int k=0;k<n;k++){hash^=(unsigned char)buf[k];hash*=1099511628211ULL;}
        ok=fwrite(buf,1,n,f)==(size_t)n;got+=n;
    }
    http_stream_close(&stream);
    ok=fflush(f)==0 && ok;ok=fsync(fileno(f))==0 && ok;ok=fclose(f)==0 && ok;
    char actual[17];snprintf(actual,sizeof(actual),"%016" PRIx64,hash);
    ok=ok && got==size && !strcmp(actual,j->hash[i]);
    fab_pack_t check={0};if(ok){ok=fab_pack_load(&check,path);fab_pack_free(&check);}
    if(!ok)unlink(path);else j->changed[i]=true;
    return ok;
}
static void sync_done(void *arg) {
    sync_job_t *j=arg;busy=false;
    bool ok=j->ok && gui_card_available();
    for(int i=0;i<2;i++) {
        char from[640],to[640];snprintf(from,sizeof(from),"%s/%s.new",j->dir,pack_names[i]);
        snprintf(to,sizeof(to),"%s/%s.fab",j->dir,pack_names[i]);
        if(ok && j->changed[i]) {
            if(rename(from,to)!=0){ok=false;continue;}
            snprintf(to,sizeof(to),"%s/%s.hash",j->dir,pack_names[i]);
            FILE*f=fopen(to,"w");if(f){fputs(j->hash[i],f);fclose(f);}
        } else unlink(from);
    }
    failed=!ok;
    if(ok) {
        snprintf(export_date,sizeof(export_date),"%s",j->date);
        config_set("fabularium","export_date",export_date);config_save();
    }
    load_packs();
    if(lv_screen_active()==fabularium_screen && !detail && !searching)rebuild();
    refresh_status();
    if(j->manual){if(ok)toast_success(tr("fab_synced"));else toast_error(tr("fab_offline"));}
    free(j);
}
static void *sync_worker(void *arg) {
    sync_job_t *j=arg;thread_be_background("fab-sync");
    char url[256],*body=NULL;size_t len=0;
    snprintf(url,sizeof(url),"%s/manifest.json",j->server);
    json_doc_t d={0};
    j->ok=http_get(url,&body,&len,8192,10) && body && json_parse(body,&d);
    if(j->ok) {
        int root=json_root(&d);j->ok=json_obj_long(&d,root,"version",0)==1;
        json_obj_str(&d,root,"exportedAt",j->date,sizeof(j->date));
        for(int i=0;j->ok && i<2;i++) {
            int item=json_get(&d,root,pack_names[i]);long size=json_obj_long(&d,item,"size",0);
            j->ok=json_obj_str(&d,item,"hash",j->hash[i],sizeof(j->hash[i])) && valid_hash(j->hash[i]) &&
                  size>0 && size<=64*1024*1024 && fetch_pack(j,i,size);
        }
    }
    json_free(&d);free(body);
    if(!gui_post(sync_done,j)) {
        for(int i=0;i<2;i++){char path[640];snprintf(path,sizeof(path),"%s/%s.new",j->dir,pack_names[i]);unlink(path);}
        free(j);
    }
    return NULL;
}
static void sync_start(bool manual) {
    if(busy || !ensure_dir())return;
    sync_job_t*j=calloc(1,sizeof(*j));if(!j)return;
    snprintf(j->dir,sizeof(j->dir),"%s",dir);snprintf(j->server,sizeof(j->server),"%s",server);j->manual=manual;
    pthread_t th;
    if(pthread_create(&th,NULL,sync_worker,j)!=0){free(j);return;}
    pthread_detach(th);busy=true;last_attempt=time(NULL);refresh_status();
}
static void auto_sync(lv_timer_t *timer) {
    (void)timer;wifi_status_t w;wifi_get_status(&w);
    if(w.state==WIFI_STATE_CONNECTED && time(NULL)-last_attempt>=300)sync_start(false);
}
static void sync_cb(lv_event_t *e){(void)e;sync_start(true);}

static lv_obj_t *text_label(const char *text,bool heading_text) {
    lv_obj_t*l=lv_label_create(col);lv_label_set_text(l,text);lv_obj_set_width(l,LV_PCT(100));
    lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l,heading_text?&font_ui_24_bold:&font_ui_22,0);
    lv_obj_set_style_text_color(l,heading_text?theme()->accent:theme()->text_primary,0);
    lv_obj_set_style_text_line_space(l,5,0);return l;
}
static lv_obj_t *row(const char *text,lv_event_cb_t cb,void *user) {
    lv_obj_t*b=lv_button_create(col);lv_obj_set_size(b,LV_PCT(100),LV_SIZE_CONTENT);
    lv_obj_add_style(b,&theme_style_card,0);lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_pad_all(b,16,0);lv_obj_set_style_min_height(b,58,0);
    lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,user);lv_obj_set_event_bubble(b,true);
    lv_obj_t*l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_width(l,LV_PCT(100));
    lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_font(l,&font_ui_22,0);
    lv_obj_set_style_text_color(l,theme()->text_primary,0);return b;
}
static void choose_category(int c){category=c;deck=suit=-1;page=0;query[0]=0;rebuild();}
static void open_readings(void){choose_category(0);}
static void open_cards(void){choose_category(1);}
static void open_stones(void){choose_category(2);}
static void open_herbs(void){choose_category(3);}
static void deck_cb(lv_event_t *e){if(!tapping())return;deck=(int)(intptr_t)lv_event_get_user_data(e);suit=-1;page=0;rebuild();}
static void suit_cb(lv_event_t *e){if(!tapping())return;suit=(int)(intptr_t)lv_event_get_user_data(e);page=0;rebuild();}
static bool card_matches(const char *id){
    if(category!=1)return true;
    if(deck<0)return false;
    const char *name=id;
    bool meows=!strncmp(name,"card:mm-",8);
    if(deck!=meows)return false;
    name+=meows?8:5;
    return suit<0 || !strncmp(name,suit_ids[suit],strlen(suit_ids[suit]));
}
static void page_cb(lv_event_t *e){if(!tapping())return;page+=(int)(intptr_t)lv_event_get_user_data(e);rebuild();}
static void entry_cb(lv_event_t *e){if(tapping())open_entry(((fab_entry_t*)lv_event_get_user_data(e))->id);}
static void link_free(lv_event_t *e){free(lv_event_get_user_data(e));}
static void link_cb(lv_event_t *e) {
    if(!tapping())return;
    char id[128];snprintf(id,sizeof(id),"%s",(char*)lv_event_get_user_data(e));
    if(depth<16){snprintf(history[depth++],128,"%s",current);open_entry(id);}
}
static void rebuild(void) {
    clear_view();detail=false;current[0]=0;depth=0;
    lv_obj_set_hidden(home_grid,category>=0);
    lv_obj_set_hidden(col,category<0);
    lv_obj_set_hidden(search_btn,category<0 || (category==1 && suit<0));
    lv_label_set_text(heading,tr(category<0?"fabularium":category==1&&deck<0?"fab_cards":category==1&&suit<0?deck?"fab_meows":"fab_fablemakers":category==1?suit_tags[suit]:tags[category]));
    if(category<0) {
        /* Home is the shared four-tile grid, not a list. */
    } else if(category==1 && deck<0) {
        row(tr("fab_fablemakers"),deck_cb,(void*)(intptr_t)0);
        row(tr("fab_meows"),deck_cb,(void*)(intptr_t)1);
    } else if(category==1 && suit<0) {
        for(int s=0;s<5;s++)row(tr(suit_tags[s]),suit_cb,(void*)(intptr_t)s);
    } else {
        if(query[0])text_label(query,true);
        int total=0,shown=0;
        for(int p=0;p<2;p++)for(int i=0;i<packs[p].count;i++) {
            fab_entry_t*e=&packs[p].entries[i];
            if(strcmp(e->cat,cats[category]) || !card_matches(e->id) || (query[0]&&!strcasestr(e->title,query)))continue;
            if(total>=page*32 && shown<32){row(e->title,entry_cb,e);shown++;}
            total++;
        }
        if(!total)text_label(tr("fab_no_results"),false);
        if(page>0)row(tr("fab_previous"),page_cb,(void*)(intptr_t)-1);
        if(total>(page+1)*32)row(tr("fab_next"),page_cb,(void*)(intptr_t)1);
    }
    lv_obj_scroll_to_y(col,0,LV_ANIM_OFF);refresh_status();
}
typedef struct {
    FILE*f;fab_entry_t entry;long base;unsigned generation;
    char *body;cover_image_t picture;bool ok;
} view_job_t;
static void render_link(const json_doc_t *d,int link) {
    char id[128],title[512];
    if(!json_obj_str(d,link,"id",id,sizeof(id)) || !json_obj_str(d,link,"title",title,sizeof(title)))return;
    char*copy=strdup(id);if(!copy)return;
    lv_obj_t*b=row(title,link_cb,copy);lv_obj_add_event_cb(b,link_free,LV_EVENT_DELETE,copy);
}
static void render_text(char *s,const json_doc_t *d,int links) {
    int paragraph=0;
    while(*s) {
        while(*s=='\n')s++;
        if(!*s)break;
        bool h=*s=='#';if(h){while(*s=='#')s++;if(*s==' ')s++;}
        char *end=strstr(s,"\n\n");if(!end)end=s+strlen(s);
        if(end-s>1600) {end=s+1600;while(end>s && ((unsigned char)*end&0xc0)==0x80)end--;}
        char saved=*end;*end=0;text_label(s,h);*end=saved;s=end;
        for(int i=0;i<json_len(d,links);i++){
            int link=json_at(d,links,i);
            if(json_obj_long(d,link,"after",-1)==paragraph)render_link(d,link);
        }
        paragraph++;
    }
}
static void view_done(void *arg) {
    view_job_t*j=arg;
    if(j->generation!=generation || !detail || lv_screen_active()!=fabularium_screen)goto cleanup;
    lv_obj_clean(col);
    if(!j->ok){text_label(tr("fab_open_failed"),false);goto cleanup;}
    json_doc_t d={0};if(!json_parse(j->body,&d)){text_label(tr("fab_open_failed"),false);goto cleanup;}
    int root=json_root(&d);
    if(j->picture.pixels) {
        picture=j->picture;memset(&j->picture,0,sizeof(j->picture));
        lv_obj_t*im=lv_image_create(col);lv_image_set_src(im,&picture.dsc);lv_obj_set_style_align(im,LV_ALIGN_CENTER,0);
    }
    int links=json_get(&d,root,"links");
    char*text=malloc((size_t)j->entry.length+1);
    if(text){json_obj_str(&d,root,"text",text,(size_t)j->entry.length+1);render_text(text,&d,links);free(text);}
    for(int i=0;i<json_len(&d,links);i++) {
        int l=json_at(&d,links,i);
        if(json_obj_long(&d,l,"after",-1)<0)render_link(&d,l);
    }
    json_free(&d);lv_obj_scroll_to_y(col,0,LV_ANIM_OFF);
cleanup:
    cover_free(&j->picture);free(j->body);free(j);
}
static void *view_worker(void *arg) {
    view_job_t*j=arg;thread_be_background("fab-read");
    j->body=malloc((size_t)j->entry.length+1);
    j->ok=j->body && fseek(j->f,j->base+j->entry.offset,SEEK_SET)==0 &&
          fread(j->body,1,j->entry.length,j->f)==(size_t)j->entry.length;
    if(j->body)j->body[j->entry.length]=0;
    if(j->ok && j->entry.image) {
        void*img=malloc(j->entry.image);
        if(img && fread(img,1,j->entry.image,j->f)==(size_t)j->entry.image)
            cover_load_image_memory(img,j->entry.image,320,400,COVER_FIT_CONTAIN,&j->picture);
        free(img);
    }
    fclose(j->f);
    if(!gui_post(view_done,j)){cover_free(&j->picture);free(j->body);free(j);}
    return NULL;
}
static void open_entry(const char *id) {
    fab_entry_t*found=NULL;fab_pack_t*pack=NULL;
    for(int p=0;p<2 && !found;p++)for(int i=0;i<packs[p].count;i++)if(!strcmp(packs[p].entries[i].id,id)){found=&packs[p].entries[i];pack=&packs[p];break;}
    if(!found){toast_error(tr("fab_open_failed"));return;}
    view_job_t*j=calloc(1,sizeof(*j));if(!j)return;
    j->f=fopen(pack->path,"rb");if(!j->f){free(j);toast_error(tr("fab_open_failed"));return;}
    j->entry=*found;j->base=pack->base;
    /* Open before a sync can rename the pack: this fd retains its exact version. */
    snprintf(current,sizeof(current),"%s",id);clear_view();detail=true;j->generation=generation;
    lv_label_set_text(heading,found->title);lv_obj_set_hidden(search_btn,true);
    text_label(tr("fab_loading"),false);
    pthread_t th;
    if(pthread_create(&th,NULL,view_worker,j)!=0){fclose(j->f);free(j);text_label(tr("fab_open_failed"),false);return;}
    pthread_detach(th);
}
static void search_hide(void){searching=false;lv_obj_set_hidden(search_layer,true);back_btn_hide_on_screen(fabularium_screen,false);}
static void search_cancel(lv_event_t *e){(void)e;search_hide();}
static void search_accept(lv_event_t *e){(void)e;snprintf(query,sizeof(query),"%s",lv_textarea_get_text(search_field));page=0;search_hide();rebuild();}
static void search_cb(lv_event_t *e) {
    (void)e;searching=true;lv_textarea_set_text(search_field,query);keyboard_reset(keyboard);
    lv_obj_set_hidden(search_layer,false);lv_obj_move_foreground(search_layer);back_btn_hide_on_screen(fabularium_screen,true);
}
static bool back_guard(void) {
    if(searching){search_hide();return true;}
    if(detail) {
        if(depth){char id[128];snprintf(id,sizeof(id),"%s",history[--depth]);open_entry(id);}
        else rebuild();
        return true;
    }
    if(category==1 && suit>=0){suit=-1;page=0;query[0]=0;rebuild();return true;}
    if(category==1 && deck>=0){deck=-1;page=0;rebuild();return true;}
    if(category>=0){category=-1;page=0;query[0]=0;rebuild();return true;}
    return false;
}
static bool back_peek(void){return searching||detail||category>=0;}
static void loaded(lv_event_t *e){(void)e;if(ensure_dir()){load_packs();rebuild();if(time(NULL)-last_attempt>=300)sync_start(false);}}
static void unloaded(lv_event_t *e){(void)e;search_hide();clear_view();detail=false;}
void fabularium_open(void) {
    if(!ensure_dir()){gui_notify_no_card();return;}
    category=deck=suit=-1;query[0]=0;page=depth=0;switch_screen(fabularium_screen);
}
static lv_obj_t *corner(const lv_image_dsc_t *icon,int slot,lv_event_cb_t cb) {
    lv_obj_t*b=lv_button_create(fabularium_screen);lv_obj_set_size(b,56,56);
    lv_obj_align(b,LV_ALIGN_TOP_RIGHT,-cfg->padding-slot*60,cfg->padding+cfg->top_bar_height);
    lv_obj_set_style_bg_opa(b,0,0);lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*im=lv_image_create(b);lv_image_set_src(im,icon);lv_obj_add_style(im,&theme_style_icon,0);lv_obj_center(im);return b;
}
void fabularium_init(gui_config_t *config) {
    cfg=config;snprintf(server,sizeof(server),"%s",config_get("fabularium","server","http://192.168.100.11:8093"));
    snprintf(export_date,sizeof(export_date),"%s",config_get("fabularium","export_date",""));
    lv_obj_add_style(fabularium_screen,&theme_style_screen,0);lv_obj_set_scrollable(fabularium_screen,false);
    heading=settingsrow_title(fabularium_screen,cfg,"fabularium");settingsrow_title_corner_slots(heading,cfg,2);
    corner(&icon_file_down,0,sync_cb);search_btn=corner(&icon_search,1,search_cb);
    int top=settingsrow_content_top(cfg);
    status=lv_label_create(fabularium_screen);lv_obj_add_style(status,&theme_style_text_dim,0);
    lv_obj_set_style_text_font(status,&font_ui_16,0);lv_obj_set_pos(status,cfg->padding,top);
    lv_obj_set_width(status,cfg->screen_width-2*cfg->padding);lv_label_set_long_mode(status,LV_LABEL_LONG_DOT);
    col=lv_obj_create(fabularium_screen);lv_obj_remove_style_all(col);
    lv_obj_set_pos(col,cfg->padding,top+30);lv_obj_set_size(col,cfg->screen_width-2*cfg->padding,cfg->screen_height-top-30);
    lv_obj_set_flex_flow(col,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(col,12,0);lv_obj_set_style_pad_bottom(col,20,0);
    lv_obj_set_scrollable(col,true);lv_obj_set_scroll_dir(col,LV_DIR_VER);lv_obj_set_event_bubble(col,true);
    const grid_entry_t entries[]={
        {"fab_readings",&icon_menu_books,NULL,open_readings},
        {"fab_cards",&icon_menu_favourites,NULL,open_cards},
        {"fab_stones",&icon_menu_all,NULL,open_stones},
        {"fab_herbs",&icon_menu_explorer,NULL,open_herbs},
    };
    gui_config_t grid_cfg={cfg->screen_width,cfg->screen_height-30,
                           cfg->padding,cfg->top_bar_height,cfg->sd_root_path};
    home_grid=gridpage_build(fabularium_screen,&grid_cfg,entries,4,2,2,true);
    lv_obj_set_y(home_grid,top+30);
    search_layer=lv_obj_create(fabularium_screen);lv_obj_add_style(search_layer,&theme_style_screen,0);
    lv_obj_set_size(search_layer,LV_PCT(100),LV_PCT(100));lv_obj_set_style_pad_all(search_layer,0,0);
    lv_obj_set_style_border_width(search_layer,0,0);lv_obj_set_style_radius(search_layer,0,0);lv_obj_set_scrollable(search_layer,false);
    search_field=lv_textarea_create(search_layer);lv_textarea_set_one_line(search_field,true);lv_textarea_set_max_length(search_field,120);
    lv_obj_add_style(search_field,&theme_style_card,0);lv_obj_set_style_text_font(search_field,&font_ui_24,0);
    lv_obj_set_pos(search_field,cfg->padding,top);lv_obj_set_size(search_field,cfg->screen_width-2*cfg->padding,64);
    lv_textarea_set_placeholder_text(search_field,tr("fab_search"));keyboard_style_caret(search_field);
    lv_obj_t*cancel=lv_button_create(search_layer);lv_obj_set_pos(cancel,cfg->padding,top+84);lv_obj_set_size(cancel,120,50);
    lv_obj_add_event_cb(cancel,search_cancel,LV_EVENT_CLICKED,NULL);
    lv_obj_t*l=lv_label_create(cancel);lv_label_set_text(l,tr("cancel"));lv_obj_center(l);
    keyboard=keyboard_create(search_layer,cfg->screen_width,316,search_field,NULL,"ok",search_accept,NULL);
    lv_obj_set_hidden(search_layer,true);
    switcher_set_back_guard(fabularium_screen,back_guard);switcher_set_back_guard_peek(fabularium_screen,back_peek);
    lv_obj_add_event_cb(fabularium_screen,loaded,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(fabularium_screen,unloaded,LV_EVENT_SCREEN_UNLOADED,NULL);
    lv_timer_create(auto_sync,30000,NULL);refresh_status();
}
