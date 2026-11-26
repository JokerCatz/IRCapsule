/* Frame ring, trigger matching, macro executor and the device log. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "store.h"

typedef struct {
    uint32_t seq;               /* 0 = empty */
    int64_t  t_ms;
    int32_t  gap_ms;
    uint16_t n;
    char     proto[24];
    uint16_t match;             /* rule id that fired, 0 if none */
    uint16_t *raw;              /* normalised timings, heap */
} frame_t;

typedef struct { uint32_t seq; int64_t t_ms; char m[96]; } log_t;

enum { JOB_RULE = 0, JOB_SIGNAL = 1, JOB_RAW = 2, JOB_LEDTEST = 3 };

void engine_init(void);
/* Called by the capture task for every frame heard. Takes a copy. */
void engine_on_frame(const uint16_t *raw, size_t n, int64_t t_us);
/* Copy of a ring entry (seq 0 = newest) into *out; raw is copied into rawbuf. false if not found. */
bool engine_frame_copy(uint32_t seq, frame_t *out, uint16_t *rawbuf, size_t max);

bool engine_enqueue(int type, uint16_t id);
bool engine_enqueue_raw(const uint16_t *raw, size_t n, uint16_t khz);

/* Blink all IR LEDs (0.5 s on, 0.5 s off) until switched off or for 60 s. false if the queue is full. */
bool engine_ledtest(bool on);

/* Recording session: heard frames are saved as numbered drafts and rules do not fire. */
void engine_record(bool on);
bool engine_recording(void);
uint8_t engine_layer(void);
void engine_set_layer(uint8_t l);

void ilog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  engine_logs(log_t *out, int max);

/* A VS1838B on long, dense frames (air conditioners) outputs marks that get shorter while the spaces grow by
 * as much. Rebuild every mark at its normal length and give the difference back to the space. Frames whose
 * marks do not drift are left as they are. */
void engine_normalise(uint16_t *raw, size_t n);
