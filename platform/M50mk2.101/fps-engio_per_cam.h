#ifndef _fps_engio_per_cam_h_
#define _fps_engio_per_cam_h_

/* [RAW-VIDEO R2, docs/30] DIGIC 8 FPS register block (same as M50 mk1). A/B confirmed on
 * 1.0.1; CONFIRM_CHANGES sits in the same 0xD04061xx block (mk1 value; validate on-target). */
#define FPS_REGISTER_A 0xD0406198
#define FPS_REGISTER_B 0xD04061A4
#define FPS_REGISTER_CONFIRM_CHANGES 0xD0406190

int get_fps_register_a(void);
int get_fps_register_a_default(void);
int get_fps_register_b(void);

#endif
