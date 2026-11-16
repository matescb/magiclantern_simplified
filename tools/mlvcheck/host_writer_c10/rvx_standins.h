#ifndef _rvx_standins_h_
#define _rvx_standins_h_

#include <stdint.h>

void rvx_sim_set_geometry(void *buffer, int width, int height,
                          int skip_left, int skip_right, int skip_top, int skip_bottom,
                          int black, int white);
int rvx_sim_pitch(void);
void rvx_sim_advance_us(uint64_t us);

#endif
