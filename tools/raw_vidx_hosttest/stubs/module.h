#ifndef HOSTTEST_MODULE_H
#define HOSTTEST_MODULE_H
#include "dryos.h"
#define CBR_VSYNC 1
#define CBR_KEYPRESS 2
#define MODULE_KEY_PRESS_ZOOMIN 10
#define MODULE_KEY_LV 11
#define MODULE_KEY_REC 12
struct hosttest_cbr { int type; unsigned int (*fn)(unsigned int); };
#define MODULE_CBRS_START() struct hosttest_cbr hosttest_cbrs[] = {
#define MODULE_CBR(t, f, c) { t, f },
#define MODULE_CBRS_END() { 0, 0 } };
#endif
