#include "golfpage.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/meloetta/minigolf.h"

lv_obj_t *golfpage_screen;
static lv_obj_t *board,*ball_obj,*aim,*status,*next_btn,*next_label,*mode_label;
static lv_timer_t *timer;
static lv_point_precise_t aim_points[2];
static golf_ball_t ball;
static int hole,total,best,mode,day;
static golf_course_t course;
static float scale,ax,ay,dx,dy;
static bool dragging,active;
static uint32_t last_tick;
static int today(void) {
    time_t now=time(NULL);struct tm date;
    if(now<=0 || !localtime_r(&now,&date))return 0;
    return (date.tm_year+1900)*10000+(date.tm_mon+1)*100+date.tm_mday;
}
static void select_course(void){golf_course(mode,hole,day,&course);}
/* Par of holes [0, upto) in the current mode (and date, for Daily). */
static int par_through(int upto) {
    int p=0; golf_course_t c;
    for(int i=0;i<upto && i<GOLF_HOLES;i++){golf_course(mode,i,day,&c);p+=c.par;}
    return p;
}
static void to_par(char *out,size_t n,int strokes,int par) {
    int d=strokes-par;
    if(d==0)snprintf(out,n,"E"); else snprintf(out,n,"%+d",d);
}

static void save(void) {
    config_set_int("golf","mode",mode);config_set_int("golf","day",day);
    config_set_int("golf","hole",hole); config_set_int("golf","total",total);
    config_set_int("golf","strokes",ball.strokes);
    config_set_int("golf","x",(int)(ball.x*100)); config_set_int("golf","y",(int)(ball.y*100));
    config_set_int("golf","vx",(int)(ball.vx*100)); config_set_int("golf","vy",(int)(ball.vy*100));
    config_set_bool("golf","sunk",ball.sunk); config_save();
}
static lv_obj_t *shape(float x,float y,float w,float h,lv_color_t color,int radius) {
    lv_obj_t *o=lv_obj_create(board); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,lroundf(x*scale),lroundf(y*scale));
    lv_obj_set_size(o,lroundf(w*scale),lroundf(h*scale));
    lv_obj_set_style_bg_color(o,color,0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0); lv_obj_set_clickable(o,false); return o;
}
static void render(void) {
    lv_obj_set_pos(ball_obj,lroundf((ball.x-GOLF_RADIUS)*scale),lroundf((ball.y-GOLF_RADIUS)*scale));
    lv_obj_set_hidden(aim,!dragging);
    if(dragging) {
        float d=hypotf(dx,dy), k=d>140?140/d:1;
        aim_points[0]=(lv_point_precise_t){ball.x*scale,ball.y*scale};
        aim_points[1]=(lv_point_precise_t){(ball.x+dx*k)*scale,(ball.y+dy*k)*scale};
        lv_line_set_points(aim,aim_points,2);
    }
    char s[220],line2[120],tp[12],bp[12],bs[24];
    int course_par=par_through(GOLF_HOLES);
    /* The running +/- counts finished holes, plus this one once it drops. */
    int played_par=par_through(hole)+(ball.sunk?course.par:0);
    int played=total+(ball.sunk?ball.strokes:0);
    to_par(tp,sizeof(tp),played,played_par);
    if(best){to_par(bp,sizeof(bp),best,course_par);snprintf(bs,sizeof(bs),"%d (%s)",best,bp);}
    else snprintf(bs,sizeof(bs),"-");
    if(ball.sunk && hole==GOLF_HOLES-1) {
        snprintf(s,sizeof(s),tr("golf_finished"),played,tp,bs);
    } else {
        snprintf(line2,sizeof(line2),tr("golf_total"),total+ball.strokes,tp,bs);
        snprintf(s,sizeof(s),tr("golf_score"),hole+1,course.par,ball.strokes);
        strncat(s,"\n",sizeof(s)-strlen(s)-1); strncat(s,line2,sizeof(s)-strlen(s)-1);
    }
    lv_label_set_text(status,s);
    lv_obj_set_hidden(next_btn,!ball.sunk);
    lv_label_set_text(next_label,tr(hole==GOLF_HOLES-1?"golf_again":"golf_next"));
}
static void draw_course(void) {
    lv_obj_clean(board);
    lv_obj_set_style_bg_color(board,lv_color_hex(0x17352e),0);
    const golf_course_t *c=&course;
    for(int i=0;i<c->sands;i++) {const golf_rect_t*r=&c->sand[i]; shape(r->x,r->y,r->w,r->h,lv_color_hex(0x75664a),8);}
    for(int i=0;i<c->walls;i++) {const golf_rect_t*r=&c->wall[i]; shape(r->x,r->y,r->w,r->h,theme()->accent,3);}
    shape(c->hx-12,c->hy-12,24,24,lv_color_hex(0x08130f),LV_RADIUS_CIRCLE);
    shape(c->hx+12,c->hy-32,2,31,lv_color_hex(0xd6d9bc),0);
    shape(c->hx+14,c->hy-32,17,11,theme()->accent,2);
    aim=lv_line_create(board); lv_obj_set_clickable(aim,false);
    lv_obj_set_style_line_color(aim,theme()->accent,0); lv_obj_set_style_line_width(aim,3,0);
    lv_obj_set_style_line_rounded(aim,true,0);
    ball_obj=shape(0,0,GOLF_RADIUS*2,GOLF_RADIUS*2,lv_color_hex(0xf6f2dc),LV_RADIUS_CIRCLE);
    render();
}
static void touch(lv_event_t *e) {
    lv_event_code_t code=lv_event_get_code(e);
    lv_indev_t *in=lv_event_get_indev(e); if(!in)return;
    lv_point_t p; lv_indev_get_point(in,&p);
    if(code==LV_EVENT_PRESSED && !golf_moving(&ball) && !ball.sunk) {
        dragging=true; ax=p.x; ay=p.y; dx=dy=0;
    } else if(code==LV_EVENT_PRESSING && dragging) {dx=(ax-p.x)/scale; dy=(ay-p.y)/scale;}
    else if(code==LV_EVENT_RELEASED && dragging) {dragging=false; golf_shoot(&ball,dx,dy);}
    else if(code==LV_EVENT_PRESS_LOST) dragging=false;
    render();
}
static void tick(lv_timer_t *t) {
    if(!active){lv_timer_pause(t);return;}
    uint32_t now=lv_tick_get(),elapsed=now-last_tick; last_tick=now;
    /* Standby does not advance the ball through a whole course. */
    if(elapsed>100)elapsed=16;
    if(golf_moving(&ball)) {
        golf_step_course(&ball,&course,elapsed/1000.0f);
        if(!golf_moving(&ball)) {
            if(ball.sunk && hole==GOLF_HOLES-1 && (!best || total+ball.strokes<best)) {
                best=total+ball.strokes;
                config_set_int("golf",mode==0?"best_classic":mode==1?"best_extra":"best_daily",best);
            }
            save();
        }
        render();
    }
}
static void restart(void *user) {
    (void)user; hole=total=0; dragging=false;select_course();golf_reset_course(&ball,&course);save();draw_course();
}
static void mode_cb(lv_event_t *e) {
    (void)e;mode=(mode+1)%GOLF_MODES;
    if(mode==2)config_set_int("golf","daily_layout_version",3);
    if(mode==2){day=today();if(day!=config_get_int("golf","best_day",0)){
        config_set_int("golf","best_day",day);config_set_int("golf","best_daily",0);
    }}
    best=(int)config_get_int("golf",mode==0?"best_classic":mode==1?"best_extra":"best_daily",
                             mode==0?config_get_int("golf","best",0):0);
    lv_label_set_text(mode_label,tr(mode==0?"golf_classic":mode==1?"golf_extra":"golf_daily"));
    restart(NULL);
}
static void restart_cb(lv_event_t *e) {
    (void)e; confirm_show(tr("golf_restart"),NULL,tr("golf_again"),restart,NULL);
}
static void next_cb(lv_event_t *e) {
    (void)e;if(!ball.sunk)return;
    if(hole==GOLF_HOLES-1){restart(NULL);return;}
    total+=ball.strokes;hole++;select_course();golf_reset_course(&ball,&course);save();draw_course();
}
static void loaded(lv_event_t *e) {
    (void)e;active=true;last_tick=lv_tick_get();
    if(mode==2 && (day!=today() || config_get_int("golf","daily_layout_version",0)!=3)){
        config_set_int("golf","daily_layout_version",3);
        day=today();best=0;config_set_int("golf","best_day",day);config_set_int("golf","best_daily",0);restart(NULL);
    }else draw_course();
    lv_timer_resume(timer);
}
static void unloaded(lv_event_t *e) {
    (void)e;active=dragging=false;lv_timer_pause(timer);save();
}
void golfpage_open(void) {switch_screen(golfpage_screen);}
static lv_obj_t *button(lv_obj_t *parent,const char *tag,lv_event_cb_t cb) {
    lv_obj_t*b=lv_button_create(parent);lv_obj_add_style(b,&theme_style_card,0);
    lv_obj_set_size(b,LV_PCT(31),52);lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*l=lv_label_create(b);lv_label_set_text(l,tr(tag));
    lv_obj_set_style_text_font(l,&font_ui_16,0);lv_obj_set_style_text_color(l,theme()->accent,0);lv_obj_center(l);return b;
}
void golfpage_init(gui_config_t *cfg) {
    lv_obj_add_style(golfpage_screen,&theme_style_screen,0);lv_obj_set_scrollable(golfpage_screen,false);
    settingsrow_title(golfpage_screen,cfg,"mini_golf");
    int top=settingsrow_content_top(cfg);
    status=lv_label_create(golfpage_screen);lv_obj_add_style(status,&theme_style_text,0);
    lv_obj_set_style_text_font(status,&font_ui_16,0);lv_obj_set_width(status,cfg->screen_width-16);
    lv_obj_set_style_text_align(status,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(status,8,top);
    scale=fminf((cfg->screen_width-20)/(float)GOLF_W,(cfg->screen_height-top-118)/(float)GOLF_H);
    board=lv_obj_create(golfpage_screen);lv_obj_remove_style_all(board);
    lv_obj_set_size(board,lroundf(GOLF_W*scale),lroundf(GOLF_H*scale));
    lv_obj_set_pos(board,(cfg->screen_width-lroundf(GOLF_W*scale))/2,top+52);
    lv_obj_set_style_bg_opa(board,LV_OPA_COVER,0);lv_obj_set_style_radius(board,8,0);
    lv_obj_set_scrollable(board,false);lv_obj_set_gesture_bubble(board,false);
    lv_obj_add_event_cb(board,touch,LV_EVENT_PRESSED,NULL);lv_obj_add_event_cb(board,touch,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(board,touch,LV_EVENT_RELEASED,NULL);lv_obj_add_event_cb(board,touch,LV_EVENT_PRESS_LOST,NULL);
    lv_obj_t*bar=lv_obj_create(golfpage_screen);lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar,10,cfg->screen_height-56);lv_obj_set_size(bar,cfg->screen_width-20,52);
    lv_obj_set_flex_flow(bar,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(bar,5,0);
    lv_obj_t *mode_btn=button(bar,"golf_classic",mode_cb);mode_label=lv_obj_get_child(mode_btn,0);
    button(bar,"golf_again",restart_cb);next_btn=button(bar,"golf_next",next_cb);next_label=lv_obj_get_child(next_btn,0);
    mode=(int)config_get_int("golf","mode",0);if(mode<0||mode>=GOLF_MODES)mode=0;
    day=(int)config_get_int("golf","day",0);
    lv_label_set_text(mode_label,tr(mode==0?"golf_classic":mode==1?"golf_extra":"golf_daily"));
    hole=(int)config_get_int("golf","hole",0);if(hole<0||hole>=GOLF_HOLES)hole=0;
    total=(int)config_get_int("golf","total",0);if(total<0)total=0;
    best=(int)config_get_int("golf",mode==0?"best_classic":mode==1?"best_extra":"best_daily",
                             mode==0?config_get_int("golf","best",0):0);
    select_course();golf_reset_course(&ball,&course);ball.strokes=(int)config_get_int("golf","strokes",0);
    ball.x=config_get_int("golf","x",(int)(ball.x*100))/100.0f;
    ball.y=config_get_int("golf","y",(int)(ball.y*100))/100.0f;
    ball.vx=config_get_int("golf","vx",0)/100.0f;
    ball.vy=config_get_int("golf","vy",0)/100.0f;
    if(ball.x<GOLF_RADIUS||ball.x>GOLF_W-GOLF_RADIUS||ball.y<GOLF_RADIUS||ball.y>GOLF_H-GOLF_RADIUS)golf_reset(&ball,hole);
    ball.sunk=config_get_bool("golf","sunk",false);
    timer=lv_timer_create(tick,16,NULL);lv_timer_pause(timer);power_pause_in_standby(timer);
    lv_obj_add_event_cb(golfpage_screen,loaded,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(golfpage_screen,unloaded,LV_EVENT_SCREEN_UNLOADED,NULL);
}
