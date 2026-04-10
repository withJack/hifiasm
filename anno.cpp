#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>
#include "anno.h"
#include "CommandLines.h"
#include "kseq.h"

KSEQ_INIT(gzFile, gzread)

anno_t *ha_anno = NULL;
int ha_anno_active = 0;

/* ======== Phase A: name-keyed hash map ======== */

typedef struct {
    char *name;
    anno_interval_t *intervals;
    int32_t n_intervals;
} anno_entry_t;

static anno_entry_t *g_anno_map = NULL;
static int64_t g_anno_map_n = 0;
static int64_t g_anno_map_m = 0;

/* hash table for name lookup */
static int64_t *g_ht = NULL;
static int64_t g_ht_size = 0;

static uint64_t hash_str(const char *s, int len)
{
    uint64_t h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) + (uint8_t)s[i];
    return h;
}

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
        fprintf(stderr, "[E::%s] annotation line %lld: expected triplets, got %d fields\n",
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

int anno_parse_file(const char *fn)
{
    FILE *fp;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    int64_t line_num = 0;

    fp = fopen(fn, "r");
    if (!fp) {
        fprintf(stderr, "[E::%s] failed to open annotation file '%s': %s\n",
                __func__, fn, strerror(errno));
        return -1;
    }

    g_anno_map_m = 1024;
    g_anno_map = (anno_entry_t *)calloc(g_anno_map_m, sizeof(anno_entry_t));
    g_anno_map_n = 0;

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
        if (n_intervals < 0) { free(line); fclose(fp); return -1; }
        if (n_intervals == 0) continue;

        if (g_anno_map_n >= g_anno_map_m) {
            g_anno_map_m = g_anno_map_m + (g_anno_map_m >> 1);
            g_anno_map = (anno_entry_t *)realloc(g_anno_map, g_anno_map_m * sizeof(anno_entry_t));
        }
        g_anno_map[g_anno_map_n].name = (char *)malloc(name_len + 1);
        memcpy(g_anno_map[g_anno_map_n].name, name_start, name_len);
        g_anno_map[g_anno_map_n].name[name_len] = '\0';
        g_anno_map[g_anno_map_n].intervals = intervals;
        g_anno_map[g_anno_map_n].n_intervals = n_intervals;
        g_anno_map_n++;
    }

    free(line);
    fclose(fp);

    /* Build hash table for O(1) lookup */
    g_ht_size = g_anno_map_n * 2 + 1;
    g_ht = (int64_t *)malloc(g_ht_size * sizeof(int64_t));
    memset(g_ht, -1, g_ht_size * sizeof(int64_t));
    for (int64_t i = 0; i < g_anno_map_n; i++) {
        uint64_t h = hash_str(g_anno_map[i].name, strlen(g_anno_map[i].name)) % g_ht_size;
        while (g_ht[h] >= 0) h = (h + 1) % g_ht_size;
        g_ht[h] = i;
    }

    fprintf(stderr, "[M::%s] parsed %lld annotation entries from '%s'\n",
            __func__, (long long)g_anno_map_n, fn);
    return 0;
}

/* Look up a read name in the Phase A hash map. Returns index or -1. */
static int64_t anno_map_lookup(const char *name, int name_len)
{
    if (!g_ht || g_ht_size == 0) return -1;
    uint64_t h = hash_str(name, name_len) % g_ht_size;
    while (g_ht[h] >= 0) {
        int64_t idx = g_ht[h];
        if ((int)strlen(g_anno_map[idx].name) == name_len &&
            memcmp(g_anno_map[idx].name, name, name_len) == 0)
            return idx;
        h = (h + 1) % g_ht_size;
    }
    return -1;
}

/* ======== Replicate hifiasm's flt_quals logic ======== */
static inline int flt_quals_anno(char *sc_a, int64_t sc_l, int64_t sc_off, int64_t sc_cut)
{
    int64_t sc_min = sc_l * sc_cut, sc_tot;
    int64_t k;
    for (k = sc_tot = 0; (k < sc_l) && (sc_tot < sc_min); k++)
        sc_tot += (((uint8_t)sc_a[k]) - sc_off);
    return (sc_tot >= sc_min) ? 1 : 0;
}

/* ======== Phase B: FASTQ re-scan to assign rid ======== */

int anno_assign_rids(int num_files, char **filenames, int64_t expected_n_reads,
                     int adapter_len, int64_t rl_cut, int is_sc, int64_t sc_cut)
{
    int64_t rid = 0, matched = 0;
    anno_t *a;

    a = (anno_t *)calloc(1, sizeof(anno_t));
    a->n_reads = expected_n_reads;
    a->reads = (anno_read_t *)calloc(expected_n_reads, sizeof(anno_read_t));

    for (int fi = 0; fi < num_files; fi++) {
        gzFile fp = gzopen(filenames[fi], "r");
        if (!fp) {
            fprintf(stderr, "[E::%s] failed to open '%s'\n", __func__, filenames[fi]);
            free(a->reads); free(a);
            return -1;
        }
        kseq_t *ks = kseq_init(fp);

        while (kseq_read(ks) >= 0) {
            /* Replicate hifiasm's exact filtering logic */
            int l = (int)(ks->seq.l) - adapter_len - adapter_len;
            if (l <= 0 || l < rl_cut) continue;
            if (is_sc && sc_cut > 0 &&
                !flt_quals_anno(ks->qual.s + adapter_len, l, 33, sc_cut)) continue;

            /* This read survived filtering — it gets rid */
            if (rid >= expected_n_reads) {
                fprintf(stderr, "[E::%s] FASTQ re-scan found more reads (%lld+) than expected (%lld)\n",
                        __func__, (long long)(rid + 1), (long long)expected_n_reads);
                kseq_destroy(ks); gzclose(fp);
                free(a->reads); free(a);
                return -1;
            }

            /* Look up in annotation hash map */
            int64_t idx = anno_map_lookup(ks->name.s, ks->name.l);
            if (idx >= 0) {
                a->reads[rid].a = g_anno_map[idx].intervals;
                a->reads[rid].n = g_anno_map[idx].n_intervals;
                g_anno_map[idx].intervals = NULL; /* transfer ownership */
                matched++;
            }
            rid++;
        }

        kseq_destroy(ks);
        gzclose(fp);
    }

    if (rid != expected_n_reads) {
        fprintf(stderr, "[E::%s] FASTQ re-scan found %lld reads but expected %lld\n",
                __func__, (long long)rid, (long long)expected_n_reads);
        free(a->reads); free(a);
        return -1;
    }

    ha_anno = a;
    fprintf(stderr, "[M::%s] matched %lld/%lld reads (%lld in annotation file, %lld unmatched)\n",
            __func__, (long long)matched, (long long)expected_n_reads,
            (long long)g_anno_map_n, (long long)(g_anno_map_n - matched));
    return 0;
}

void anno_free_map(void)
{
    if (g_anno_map) {
        for (int64_t i = 0; i < g_anno_map_n; i++) {
            free(g_anno_map[i].name);
            free(g_anno_map[i].intervals); /* NULL if transferred */
        }
        free(g_anno_map);
        g_anno_map = NULL;
        g_anno_map_n = g_anno_map_m = 0;
    }
    free(g_ht);
    g_ht = NULL;
    g_ht_size = 0;
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
