/* rp_sky.h: the map's sky (rp_sky.c). */

#ifndef RP_SKY_H
#define RP_SKY_H

#include "rp_shader.h"

/* The sky of shader sh around the view set, at time t (seconds): its far
   box and its cloud layers as polygons without depth, in the strips of
   beam racing given (where the view sees the map's sky surfaces), to be
   drawn after the world's surfaces that write depth, behind them. */
void rp_sky_draw(const struct rp_shader *sh, float t, u32 strips);

#endif
