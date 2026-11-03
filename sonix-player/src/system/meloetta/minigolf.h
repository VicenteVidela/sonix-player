#ifndef MELOETTA_MINIGOLF_H
#define MELOETTA_MINIGOLF_H
#include <stdbool.h>
#define GOLF_W 400
#define GOLF_H 560
#define GOLF_HOLES 9
#define GOLF_MODES 3
#define GOLF_RADIUS 7.0f
typedef struct { float x, y, w, h; } golf_rect_t;
typedef struct {
    float sx, sy, hx, hy;
    int par, walls, sands;
    golf_rect_t wall[5], sand[2];
} golf_course_t;
typedef struct { float x, y, vx, vy; int strokes; bool sunk; } golf_ball_t;
extern const golf_course_t golf_courses[GOLF_HOLES];
/* 0: original nine, 1: extra nine, 2: deterministic local-calendar daily. */
void golf_course(int mode, int hole, int day, golf_course_t *out);
void golf_reset_course(golf_ball_t *b, const golf_course_t *course);
void golf_step_course(golf_ball_t *b, const golf_course_t *course, float dt);
void golf_reset(golf_ball_t *b, int hole);
bool golf_moving(const golf_ball_t *b);
bool golf_shoot(golf_ball_t *b, float dx, float dy);
void golf_step(golf_ball_t *b, int hole, float dt);
#endif
