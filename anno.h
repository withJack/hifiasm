#ifndef __ANNO_H__
#define __ANNO_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-read annotation: a list of intervals covering the entire read.
 * type=-1 → default (drop during annotation post-filter)
 * type=0  → suppress (drop during annotation post-filter)
 * type=1  → keep this minimizer for overlap finding
 *
 * When annotation post-filtering is active (ha_anno_active == 1),
 * ONLY minimizers at type==1 positions survive after sketching.
 * All others (type==0, type==-1) are removed.
 * This runs AFTER all normal hifiasm minimizer filtering.
 */
typedef struct {
    int32_t start;
    int32_t end;   /* exclusive: [start, end) */
    int32_t type;  /* -1, 0, or 1 */
} anno_interval_t;

typedef struct {
    anno_interval_t *a;
    int32_t n;  /* number of intervals */
} anno_read_t;

typedef struct {
    anno_read_t *reads;
    int64_t n_reads;
} anno_t;

extern anno_t *ha_anno;
extern int ha_anno_active;

int anno_load(const char *fn, int64_t expected_n_reads);
void anno_destroy(anno_t *a);

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
