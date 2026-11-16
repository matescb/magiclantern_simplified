/* Host stand-in for ML's dryos.h, just enough for raw_vidx worker.c / event_pusher.c.
 * Deliberately does NOT include <stdio.h>: ML has its own FILE type. */
#ifndef HOSTTEST_DRYOS_H
#define HOSTTEST_DRYOS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

typedef struct ml_file FILE;
struct msg_queue;
struct semaphore;

#define TASK_LOOP for (;;)
#define SEM_CREATE_UNLOCKED 1
#define SEM_CREATE_LOCKED 0
#define UNCACHEABLE(x) (x)
#define CACHEABLE(x) (x)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define O_RDWR 02
#define O_SYNC 010000
#define FAST

struct semaphore *create_named_semaphore(const char *name, int initial);
int take_semaphore(struct semaphore *s, int timeout_ms);
int take_semaphore_now(struct semaphore *s);
int give_semaphore(struct semaphore *s);

struct msg_queue *msg_queue_create(const char *name, uint32_t len);
int msg_queue_post(struct msg_queue *q, uint32_t msg);
int msg_queue_receive(struct msg_queue *q, void *msg_out, uint32_t timeout);

void *task_create(const char *name, uint32_t prio, uint32_t stack, void *entry, void *arg);
void msleep(int ms);
uint32_t get_ms_clock(void);
uint64_t get_us_clock(void);

/* interrupt mask: nothing to mask on the host, but the harness counts the calls */
uint32_t cli(void);
void sei(uint32_t old);

void *fio_malloc(size_t n);
void fio_free(void *p);

FILE *FIO_CreateFile(const char *name);
FILE *FIO_OpenFile(const char *name, unsigned mode);
int FIO_WriteFile(FILE *f, const void *p, size_t n);
int64_t FIO_SeekSkipFile(FILE *f, int64_t pos, int whence);
void FIO_CloseFile(FILE *f);
int get_numbered_file_name(const char *pattern, int nmax, char *filename, int maxlen);
int snprintf(char *, size_t, const char *, ...);
#endif
