#ifndef HOSTTEST_RAW_H
#define HOSTTEST_RAW_H
struct raw_info { void *buffer; int height, width, pitch; };
extern struct raw_info raw_info;
void raw_lv_request(void);
void raw_lv_release(void);
void raw_lv_redirect_edmac(void *ptr);
#endif
