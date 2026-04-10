#ifndef __ANNO_H__
#define __ANNO_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t start;
    int32_t end;   /* exclusive: [start, end) */
    int32_t type;  /* -1, 0, or 1 */
} anno_interval_t;

typedef struct {
    anno_interval_t *a;
    int32_t n;
} anno_read_t;

typedef struct {
    anno_read_t *reads;
    int64_t n_reads;
} anno_t;

extern anno_t *ha_anno;
extern int ha_anno_active;

/*
 * Phase A: parse annotation file into a name-keyed hash map.
 * Call before ha_ft_gen. Does not need R_INF.
 */
int anno_parse_file(const char *fn);

/*
 * Phase B: build the rid-indexed ha_anno array by re-scanning
 * the FASTQ files with the same filtering logic as hifiasm.
 * Call after ha_ft_gen when R_INF.total_reads is known.
 */
int anno_assign_rids(int num_files, char **filenames, int64_t expected_n_reads,
                     int adapter_len, int64_t rl_cut, int is_sc, int64_t sc_cut);

void anno_destroy(anno_t *a);

/* Free the Phase A hash map (called after Phase B completes) */
void anno_free_map(void);

static inline int32_t anno_query(const anno_t *a, int64_t rid, int32_t pos)
{
    if (!a || rid < 0 || rid >= a->n_reads) return -1;
    const anno_read_t *r = &a->reads[rid];
    if (r->n == 0) return -1;
    int32_t lo = 0, hi = r->n - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) >> 1;
        if (pos < r->a[mid].start) hi = mid - 1;
        else if (pos >= r->a[mid].end) lo = mid + 1;
        else return r->a[mid].type;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* __ANNO_H__ */
