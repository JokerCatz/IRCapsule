/* Persistent state in NVS: one fixed-size "meta" blob plus one raw-timing blob per signal. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MAX_SIGNALS   96
#define MAX_RULES     48
#define MAX_DEVICES   16
#define MAX_ACTIONS   8
#define NAME_LEN      32
#define MAX_RAW       1200      /* mark/space entries per signal */

enum { SIG_REAL = 0, SIG_TRIGGER = 1 };            /* a hint for the UI: sent to an appliance / heard from a remote */
enum { ST_DRAFT = 0, ST_ACTIVE = 1, ST_DISABLED = 2 };  /* a draft has no name yet and cannot be used */
#define MAX_DRAFTS 40
enum { ACT_FIRE = 0, ACT_DELAY_MS = 1, ACT_LAYER = 2 };

typedef struct {
    uint16_t id;                /* 0 = free slot */
    uint8_t  kind;              /* SIG_REAL: sent to an appliance; SIG_TRIGGER: heard from a remote */
    uint8_t  device;            /* 0 = none */
    uint8_t  state;             /* ST_* */
    uint8_t  hits;              /* how many times a draft was heard while recording */
    uint16_t carrier_khz;
    uint16_t n;                 /* entries in the raw blob */
    char     name[NAME_LEN];
    char     proto[24];         /* descriptive guess, e.g. "NEC a=01 c=51" */
} sig_meta_t;

typedef struct { uint8_t id; char name[NAME_LEN]; } dev_meta_t;

typedef struct { uint8_t type; uint16_t val; } action_t;

typedef struct {
    uint16_t id;                /* 0 = free slot */
    uint8_t  layer;
    uint8_t  enabled;
    uint8_t  nact;
    uint16_t trigger;           /* signal id that starts this rule */
    char     name[NAME_LEN];
    action_t act[MAX_ACTIONS];
} rule_meta_t;

typedef struct {
    uint32_t    magic;
    uint16_t    next_id;
    uint16_t    dedupe_ms;      /* ignore the same rule firing again within this time */
    char        device_name[24];
    sig_meta_t  sig[MAX_SIGNALS];
    dev_meta_t  dev[MAX_DEVICES];
    rule_meta_t rule[MAX_RULES];
} meta_t;

extern meta_t g_meta;

esp_err_t store_init(void);
esp_err_t store_save_meta(void);
esp_err_t store_factory_reset(void);

sig_meta_t  *store_sig(uint16_t id);
sig_meta_t  *store_sig_alloc(void);
esp_err_t    store_sig_write_raw(uint16_t id, const uint16_t *raw, size_t n);
esp_err_t    store_sig_read_raw(uint16_t id, uint16_t *raw, size_t max, size_t *n);
esp_err_t    store_sig_delete(uint16_t id);

rule_meta_t *store_rule(uint16_t id);
rule_meta_t *store_rule_alloc(void);
dev_meta_t  *store_dev(uint8_t id);
dev_meta_t  *store_dev_alloc(void);

uint8_t  store_get_layer(void);
void     store_set_layer(uint8_t layer);
uint16_t store_next_id(void);
