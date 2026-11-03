#include "minigolf.h"
#include <math.h>

const golf_course_t golf_courses[GOLF_HOLES] = {
    {200,480,200,90,2,0,0,{{0}},{{0}}},
    {90,480,310,90,3,1,0,{{0,270,265,18}},{{0}}},
    {200,490,200,70,3,2,0,{{0,350,255,18},{145,190,255,18}},{{0}}},
    {80,480,320,80,3,1,1,{{175,160,20,265}},{{35,230,100,100}}},
    {200,490,200,80,3,2,1,{{100,210,18,210},{282,210,18,210}},{{130,270,140,90}}},
    {70,490,330,70,4,3,0,{{0,370,270,18},{130,240,270,18},{0,110,265,18}},{{0}}},
    {200,490,200,70,3,3,1,{{100,310,70,70},{230,310,70,70},{165,180,70,70}},{{25,170,75,120}}},
    {80,480,320,480,4,2,1,{{180,200,40,360},{80,150,240,20}},{{260,270,110,100}}},
    {200,490,200,70,4,4,2,{{0,385,250,16},{150,280,250,16},{0,175,250,16},{305,80,18,100}},{{30,290,85,70},{280,180,85,70}}},
};

static const golf_course_t extra_courses[GOLF_HOLES] = {
    {200,485,200,80,3,2,0,{{0,300,145,18},{255,300,145,18}},{{0}}},
    {75,480,325,85,3,2,0,{{110,270,18,200},{275,110,18,200}},{{0}}},
    {200,485,200,80,3,2,1,{{90,350,125,18},{185,180,125,18}},{{265,295,95,80}}},
    {70,480,330,80,4,3,0,{{0,325,265,18},{135,225,265,18},{0,125,250,18}},{{0}}},
    {320,480,80,80,3,2,1,{{75,210,18,190},{260,245,18,190}},{{125,290,105,80}}},
    {200,480,200,75,4,3,1,{{60,355,100,18},{240,265,100,18},{60,175,100,18}},{{180,300,65,95}}},
    {80,480,320,80,4,3,1,{{120,340,18,180},{210,170,18,180},{300,340,18,180}},{{245,410,80,75}}},
    {310,480,90,75,4,3,1,{{80,370,240,18},{80,245,240,18},{80,120,240,18}},{{180,275,80,75}}},
    {200,485,200,70,4,4,2,{{0,390,250,16},{150,295,250,16},{0,200,250,16},{150,105,250,16}},{{40,305,85,70},{275,210,85,70}}},
};

static unsigned next_random(unsigned *seed) {
    *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
    return *seed;
}

void golf_course(int mode, int hole, int day, golf_course_t *out) {
    if (!out) return;
    if (hole < 0 || hole >= GOLF_HOLES) hole = 0;
    if (mode == 0) { *out = golf_courses[hole]; return; }
    if (mode == 1) { *out = extra_courses[hole]; return; }
    /* Every daily round cycles through eight shapes. The date changes the
       starting side, cup, mirror and obstacle positions; no Wi-Fi required. */
    unsigned seed = (unsigned)day * 2654435761u ^ (unsigned)(hole + 1) * 2246822519u;
    if (!seed) seed = 1;
    int kind=((unsigned)day + (unsigned)hole*5u)%8u;
    *out = (golf_course_t){.sx=60+(next_random(&seed)%3)*140,.sy=490,
                           .hx=60+(next_random(&seed)%3)*140,.hy=70,
                           .par=kind==2||kind==5?4:3};
    int a=(int)(next_random(&seed)%35),b=(int)(next_random(&seed)%35);
    switch(kind) {
    case 0: /* staggered gates */
        out->walls=3;
        out->wall[0]=(golf_rect_t){0,375+a,235,18};
        out->wall[1]=(golf_rect_t){165,260+b,235,18};
        out->wall[2]=(golf_rect_t){0,150+a,220,18};
        break;
    case 1: /* four bumpers, generous lanes between them */
        out->walls=4;
        out->wall[0]=(golf_rect_t){75+a,315,65,70};
        out->wall[1]=(golf_rect_t){255-b,315,65,70};
        out->wall[2]=(golf_rect_t){75+a,170,65,70};
        out->wall[3]=(golf_rect_t){255-b,170,65,70};
        break;
    case 2: /* tall slalom */
        out->walls=3;
        out->wall[0]=(golf_rect_t){95+a,250,18,185};
        out->wall[1]=(golf_rect_t){190+b,125,18,235};
        out->wall[2]=(golf_rect_t){285-a,250,18,185};
        break;
    case 3: /* two offset U-shaped bends */
        out->walls=4;
        out->wall[0]=(golf_rect_t){65,340,230,16};
        out->wall[1]=(golf_rect_t){65,340,16,90};
        out->wall[2]=(golf_rect_t){105,190,230,16};
        out->wall[3]=(golf_rect_t){319,190,16,90};
        break;
    case 4: /* isolated islands, not a wall across the fairway */
        out->walls=4;
        out->wall[0]=(golf_rect_t){90+a,365,65,65};
        out->wall[1]=(golf_rect_t){245-b,305,65,65};
        out->wall[2]=(golf_rect_t){90+a,205,65,65};
        out->wall[3]=(golf_rect_t){245-b,145,65,65};
        break;
    case 5: /* narrow middle runway, open around both ends */
        out->walls=3;
        out->wall[0]=(golf_rect_t){105+a/2,170,18,245};
        out->wall[1]=(golf_rect_t){275-b/2,170,18,245};
        out->wall[2]=(golf_rect_t){170,270,60,18};
        break;
    case 6: /* diagonal stepping stones */
        out->walls=4;
        out->wall[0]=(golf_rect_t){60+a,380,90,28};
        out->wall[1]=(golf_rect_t){245-b,295,90,28};
        out->wall[2]=(golf_rect_t){65+a,210,90,28};
        out->wall[3]=(golf_rect_t){240-b,125,90,28};
        break;
    default: /* thin rails with gaps on opposite sides */
        out->walls=3;
        out->wall[0]=(golf_rect_t){0,380+a,165,18};
        out->wall[1]=(golf_rect_t){235,265+b,165,18};
        out->wall[2]=(golf_rect_t){0,150+a,165,18};
    }
    if(next_random(&seed)&1u) {
        for(int i=0;i<out->walls;i++)out->wall[i].x=GOLF_W-out->wall[i].x-out->wall[i].w;
    }
    /* Sand sits clear of spawn and cup and changes the shot without blocking it. */
    out->sands=kind==1||kind==4?2:1;
    out->sand[0]=(golf_rect_t){55+next_random(&seed)%240,285,75,65};
    if(out->sands==2)out->sand[1]=(golf_rect_t){55+next_random(&seed)%240,140,75,65};
}

void golf_reset_course(golf_ball_t *b, const golf_course_t *c) {
    *b = (golf_ball_t){.x=c->sx,.y=c->sy};
}

void golf_reset(golf_ball_t *b, int hole) {
    golf_reset_course(b,&golf_courses[hole]);
}
bool golf_moving(const golf_ball_t *b) { return b->vx != 0 || b->vy != 0; }
bool golf_shoot(golf_ball_t *b, float dx, float dy) {
    float d = hypotf(dx,dy);
    if (b->sunk || golf_moving(b) || !isfinite(d) || d < 5) return false;
    float scale = 3.4f * (d > 140 ? 140/d : 1);
    b->vx = dx*scale; b->vy = dy*scale; b->strokes++;
    return true;
}
static float clamp(float n, float lo, float hi) { return fmaxf(lo,fminf(hi,n)); }
static void collide(golf_ball_t *b, const golf_rect_t *r) {
    float nx=b->x-clamp(b->x,r->x,r->x+r->w), ny=b->y-clamp(b->y,r->y,r->y+r->h);
    float d=hypotf(nx,ny);
    if (d >= GOLF_RADIUS) return;
    if (d < 0.0001f) {
        float left=fabsf(b->x-r->x), right=fabsf(b->x-r->x-r->w);
        float top=fabsf(b->y-r->y), bottom=fabsf(b->y-r->y-r->h);
        float m=fminf(fminf(left,right),fminf(top,bottom));
        if (m==left) { b->x=r->x-GOLF_RADIUS; nx=-1; ny=0; }
        else if (m==right) { b->x=r->x+r->w+GOLF_RADIUS; nx=1; ny=0; }
        else if (m==top) { b->y=r->y-GOLF_RADIUS; nx=0; ny=-1; }
        else { b->y=r->y+r->h+GOLF_RADIUS; nx=0; ny=1; }
    } else {
        nx/=d; ny/=d;
        b->x+=nx*(GOLF_RADIUS-d); b->y+=ny*(GOLF_RADIUS-d);
    }
    float dot=b->vx*nx+b->vy*ny;
    if (dot < 0) { b->vx-=1.82f*dot*nx; b->vy-=1.82f*dot*ny; }
}
void golf_step_course(golf_ball_t *b, const golf_course_t *c, float dt) {
    if (b->sunk || !golf_moving(b) || dt<=0) return;
    /* Substeps keep the fastest ball from tunnelling through a wall. */
    int n=(int)ceilf(dt*240); if(n>48) n=48;
    dt=fminf(dt,0.2f)/n;
    for(int k=0;k<n;k++) {
        b->x+=b->vx*dt; b->y+=b->vy*dt;
        if(b->x<GOLF_RADIUS) {b->x=GOLF_RADIUS; b->vx=fabsf(b->vx)*0.82f;}
        if(b->x>GOLF_W-GOLF_RADIUS) {b->x=GOLF_W-GOLF_RADIUS; b->vx=-fabsf(b->vx)*0.82f;}
        if(b->y<GOLF_RADIUS) {b->y=GOLF_RADIUS; b->vy=fabsf(b->vy)*0.82f;}
        if(b->y>GOLF_H-GOLF_RADIUS) {b->y=GOLF_H-GOLF_RADIUS; b->vy=-fabsf(b->vy)*0.82f;}
        for(int i=0;i<c->walls;i++) collide(b,&c->wall[i]);
        float speed=hypotf(b->vx,b->vy);
        if(hypotf(b->x-c->hx,b->y-c->hy)<10 && speed<155) {
            b->x=c->hx; b->y=c->hy; b->vx=b->vy=0; b->sunk=true; return;
        }
        float friction=68;
        for(int i=0;i<c->sands;i++) {
            const golf_rect_t *r=&c->sand[i];
            if(b->x>=r->x && b->x<=r->x+r->w && b->y>=r->y && b->y<=r->y+r->h) friction=230;
        }
        float next=fmaxf(0,speed-friction*dt);
        if(next<2) b->vx=b->vy=0;
        else {b->vx*=next/speed; b->vy*=next/speed;}
    }
}
void golf_step(golf_ball_t *b, int hole, float dt) {
    golf_step_course(b,&golf_courses[hole],dt);
}
