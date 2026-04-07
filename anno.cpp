#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "anno.h"
#include "Process_Read.h"

anno_t *ha_anno = NULL;
int ha_anno_active = 0;

static uint64_t hash_str(const char *s, int len)
{
    uint64_t h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) + (uint8_t)s[i];
    return h;
}

typedef struct {
    char *name;
    anno_interval_t *intervals;
    int32_t n_intervals;
} anno_entry_t;

static anno_interval_t *parse_intervals(const char *p, int32_t *n_out, int64_t line_num)
{
    int n_fields = 0;
    const char *q = p;
    while (*q) {
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '\0') break;
        n_fields++;
        while (*q && *q != ' ' && *q != '\t') q++;
    }

    if (n_fields == 0) { *n_out = 0; return NULL; }

    if (n_fields % 3 != 0) {
        fprintf(stderr, "[E::%s] annotation line %lld: expected triplets (start end type), got %d fields\n",
                __func__, (long long)line_num, n_fields);
        *n_out = -1;
        return NULL;
    }

    int32_t n = n_fields / 3;
    anno_interval_t *a = (anno_interval_t *)malloc(n * sizeof(anno_interval_t));

    q = p;
    for (int32_t i = 0; i < n; i++) {
        char *endp;
        while (*q == ' ' || *q == '\t') q++;
        a[i].start = (int32_t)strtol(q, &endp, 10); q = endp;
        while (*q == ' ' || *q == '\t') q++;
        a[i].end = (int32_t)strtol(q, &endp, 10); q = endp;
        while (*q == ' ' || *q == '\t') q++;
        a[i].type = (int32_t)strtol(q, &endp, 10); q = endp;

        if (a[i].type != 0 && a[i].type != 1 && a[i].type != -1) {
            fprintf(stderr, "[E::%s] line %lld, interval %d: type must be -1, 0, or 1, got %d\n",
                    __func__, (long long)line_num, i, a[i].type);
            free(a); *n_out = -1; return NULL;
        }
        if (a[i].start >= a[i].end) {
            fprintf(stderr, "[E::%s] line %lld, interval %d: start (%d) >= end (%d)\n",
                    __func__, (long long)line_num, i, a[i].start, a[i].end);
            free(a); *n_out = -1; return NULL;
        }
    }

    if (a[0].start != 0) {
        fprintf(stderr, "[E::%s] line %lld: first interval must start at 0, got %d\n",
                __func__, (long long)line_num, a[0].start);
        free(a); *n_out = -1; return NULL;
    }
    for (int32_t i = 1; i < n; i++) {
        if (a[i].start != a[i - 1].end) {
            fprintf(stderr, "[E::%s] line %lld: intervals not contiguous (prev end=%d, start=%d)\n",
                    __func__, (long long)line_num, a[i - 1].end, a[i].start);
            free(a); *n_out = -1; return NULL;
        }
    }

    *n_out = n;
    return a;
}

static void free_map(anno_entry_t *map, int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        free(map[i].name);
        free(map[i].intervals);
    }
    free(map);
}

int anno_load(const char *fn, int64_t expected_n_reads)
{
    FILE *fp;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    int64_t line_num = 0;
    int64_t map_n = 0, map_m = 1024;
    anno_entry_t *map = NULL;
    anno_t *a = NULL;
    int64_t *ht = NULL;
    int64_t ht_size, matched, unmatched;

    fp = fopen(fn, "r");
    if (!fp) {
        fprintf(stderr, "[E::%s] failed to open annotation file '%s': %s\n",
                __func__, fn, strerror(errno));
        return -1;
    }

    map = (anno_entry_t *)calloc(map_m, sizeof(anno_entry_t));

    /* First pass: parse all entries keyed by read name */
    while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
        line_num++;
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line[--line_len] = '\0';
        if (line_len == 0) continue;

        /* First field = read name */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        const char *name_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int name_len = (int)(p - name_start);
        if (name_len == 0) continue;

        int32_t n_intervals = 0;
        anno_interval_t *intervals = parse_intervals(p, &n_intervals, line_num);
        if (n_intervals < 0) goto fail;
        if (n_intervals == 0) continue;

        if (map_n >= map_m) {
            map_m = map_m + (map_m >> 1);
            map = (anno_entry_t *)realloc(map, map_m * sizeof(anno_entry_t));
        }
        map[map_n].name = (char *)malloc(name_len + 1);
        memcpy(map[map_n].name, name_start, name_len);
        map[map_n].name[name_len] = '\0';
        map[map_n].intervals = intervals;
        map[map_n].n_intervals = n_intervals;
        map_n++;
    }
    fclose(fp); fp = NULL;
    free(line); line = NULL;

    fprintf(stderr, "[M::%s] parsed %lld annotation entries from '%s'\n",
            __func__, (long long)map_n, fn);

    /* Build hash table from map for O(1) lookup by name */
    ht_size = map_n * 2 + 1;
    ht = (int64_t *)malloc(ht_size * sizeof(int64_t));
    memset(ht, -1, ht_size * sizeof(int64_t));
    for (int64_t i = 0; i < map_n; i++) {
        uint64_t h = hash_str(map[i].name, strlen(map[i].name)) % ht_size;
        while (ht[h] >= 0) h = (h + 1) % ht_size;
        ht[h] = i;
    }

    /* Build anno_t indexed by hifiasm read ID using R_INF names */
    a = (anno_t *)calloc(1, sizeof(anno_t));
    a->n_reads = expected_n_reads;
    a->reads = (anno_read_t *)calloc(expected_n_reads, sizeof(anno_read_t));

    matched = 0;
    for (int64_t rid = 0; rid < expected_n_reads; rid++) {
        const char *rname = Get_NAME(R_INF, rid);
        int rname_len = Get_NAME_LENGTH(R_INF, rid);

        uint64_t h = hash_str(rname, rname_len) % ht_size;
        int64_t found = -1;
        while (ht[h] >= 0) {
            int64_t idx = ht[h];
            if ((int)strlen(map[idx].name) == rname_len &&
                memcmp(map[idx].name, rname, rname_len) == 0) {
                found = idx;
                break;
            }
            h = (h + 1) % ht_size;
        }

        if (found >= 0) {
            a->reads[rid].a = map[found].intervals;
            a->reads[rid].n = map[found].n_intervals;
            map[found].intervals = NULL; /* ownership transferred */
            matched++;
        }
    }
    unmatched = map_n - matched;

    free(ht);
    free_map(map, map_n);

    ha_anno = a;
    fprintf(stderr, "[M::%s] matched %lld/%lld reads (%lld in file, %lld unmatched/filtered)\n",
            __func__, (long long)matched, (long long)expected_n_reads,
            (long long)map_n, (long long)unmatched);
    return 0;

fail:
    if (fp) fclose(fp);
    free(line);
    free(ht);
    if (map) free_map(map, map_n);
    if (a) {
        for (int64_t i = 0; i < a->n_reads; i++) free(a->reads[i].a);
        free(a->reads);
        free(a);
    }
    return -1;
}

void anno_destroy(anno_t *a)
{
    if (!a) return;
    for (int64_t i = 0; i < a->n_reads; i++)
        free(a->reads[i].a);
    free(a->reads);
    free(a);
    if (ha_anno == a) ha_anno = NULL;
}
