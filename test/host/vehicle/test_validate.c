/* Validator tests: a known-good profile, then one mutation per forbidden case. */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "vp_validate.h"
#include "vt.h"
#include "vt_support.h"
#include "app_obd_dsp/signal_registry.h"

#ifndef VT_FIXTURE_DIR
#define VT_FIXTURE_DIR "fixtures"
#endif

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_ISSUES 32

static char *s_full_text;
static char *s_min_text;
static vp_validate_cfg_t s_cfg;

/* ---- tiny JSON path editor: "signals[3].src[0].req" ---- */

typedef struct {
    char key[32];
    int  idx;   /* >= 0: array index token */
} tok_t;

static int tokenize(const char *path, tok_t *t, int max)
{
    int n = 0;
    const char *p = path;
    while (*p && n < max) {
        size_t k = 0;
        if (*p == '.') {
            p++;
            continue;
        }
        if (*p == '[') {
            t[n].key[0] = '\0';
            t[n].idx = atoi(p + 1);
            n++;
            p = strchr(p, ']');
            if (!p) return -1;
            p++;
            continue;
        }
        while (*p && *p != '.' && *p != '[' && k < sizeof(t[n].key) - 1) t[n].key[k++] = *p++;
        t[n].key[k] = '\0';
        t[n].idx = -1;
        n++;
    }
    return n;
}

/* value_json NULL deletes; an index == size appends */
static int jset(cJSON *root, const char *path, const char *value_json)
{
    tok_t t[16];
    int n = tokenize(path, t, 16), i;
    cJSON *cur = root, *v = NULL;

    if (n <= 0) return 0;
    for (i = 0; i < n - 1; i++) {
        cur = t[i].idx >= 0 ? cJSON_GetArrayItem(cur, t[i].idx) : cJSON_GetObjectItemCaseSensitive(cur, t[i].key);
        if (!cur) return 0;
    }
    if (value_json) {
        v = cJSON_Parse(value_json);
        if (!v) return 0;
    }
    if (t[n - 1].idx >= 0) {
        if (!v) {
            cJSON_DeleteItemFromArray(cur, t[n - 1].idx);
            return 1;
        }
        if (t[n - 1].idx >= cJSON_GetArraySize(cur)) return cJSON_AddItemToArray(cur, v);
        return cJSON_ReplaceItemInArray(cur, t[n - 1].idx, v);
    }
    if (!v) {
        cJSON_DeleteItemFromObjectCaseSensitive(cur, t[n - 1].key);
        return 1;
    }
    if (cJSON_GetObjectItemCaseSensitive(cur, t[n - 1].key)) {
        return cJSON_ReplaceItemInObjectCaseSensitive(cur, t[n - 1].key, v);
    }
    return cJSON_AddItemToObject(cur, t[n - 1].key, v);
}

static size_t run(const cJSON *root, vp_issues_t *l, vp_issue_t *store)
{
    vp_issues_init(l, store, MAX_ISSUES);
    return vp_validate(root, &s_cfg, l);
}

static const vp_issue_t *find(const vp_issues_t *l, const char *code, const char *path)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (!strcmp(l->items[i].code, code) && !strncmp(l->items[i].path, path, strlen(path))) {
            return &l->items[i];
        }
    }
    return NULL;
}

static void test_good(void)
{
    vp_issue_t store[MAX_ISSUES];
    vp_issues_t l;
    cJSON *root = cJSON_Parse(s_full_text);
    VT_CHECK(root != NULL);
    VT_CHECK_INT(run(root, &l, store), 0);
    VT_CHECK_INT(l.n_warnings, 0);
    if (l.count) vt_print_issues(&l, "  unexpected: ");
    cJSON_Delete(root);

    vp_issues_init(&l, store, MAX_ISSUES);
    VT_CHECK_INT(vp_validate_json(s_min_text, strlen(s_min_text), &s_cfg, &l), 0);
    if (l.count) vt_print_issues(&l, "  unexpected: ");
}

typedef struct {
    const char *what;
    const char *path1, *val1;
    const char *path2, *val2;
    const char *code;        /* NULL: must stay valid */
    const char *at;          /* expected issue path prefix */
} mcase_t;

static const mcase_t k_cases[] = {
    /* still valid */
    { "minor version", "schema", "\"skygauge.vehicle/1.3\"", NULL, NULL, NULL, NULL },
    { "explicit KWP init headers", "bus.proto", "5", "bus.init",
      "[\"ATSH8111F1\",\"ATSH8101F1\",\"ATSH8001F1\"]", NULL, NULL },
    { "KWP header on CAN refused", "bus.proto", "6", "bus.init",
      "[\"ATSH8111F1\"]", "E_AT", "bus.init[0]" },
    { "KWP header on auto refused", "bus.proto", "0", "bus.init",
      "[\"ATSH8111F1\"]", "E_AT", "bus.init[0]" },
    { "lowercase hdr", "signals[5].src[0].hdr", "\"7e0\"", NULL, NULL, NULL, NULL },
    { "legacy 11-bit tx header", "signals[5].src[0].hdr", "\"740\"", NULL, NULL, NULL, NULL },
    { "legacy 11-bit rx filter", "signals[6].src[0].rx", "\"748\"", NULL, NULL, NULL, NULL },
    { "functional bus, 21 01 physical", "bus.hdr", "\"7DF\"", "signals[3].src[1].hdr", "\"7E0\"", NULL, NULL },
    { "29-bit functional bus, physical 22", "bus.hdr", "\"18DB33F1\"", "signals[3].src[1].hdr", "\"18DA10F1\"", NULL, NULL },
    { "normal-addressed raw", "signals[6].src[0].raw", "\"03 22 DA 2E\"", NULL, NULL, NULL, NULL },
    { "compact raw", "signals[6].src[0].raw", "\"180322DA2E\"", NULL, NULL, NULL, NULL },
    { "raw with matching req", "signals[6].src[0].req", "\"22DA2E\"", NULL, NULL, NULL, NULL },
    { "29-bit rx", "signals[6].src[0].rx", "\"18DAF110\"", NULL, NULL, NULL, NULL },
    { "init whitelist", "bus.init", "[\"ATSH 7DF\",\"ATCRA\",\"ATST0A\",\"ATSPA6\",\"ATCRA 7E8\",\"ATSH18DA10F1\"]",
      NULL, NULL, NULL, NULL },
    { "service 09", "signals[9].src[0].req", "\"0902\"", NULL, NULL, NULL, NULL },
    { "service 21", "signals[9].src[0].req", "\"2101\"", NULL, NULL, NULL, NULL },
    { "service 22", "signals[9].src[0].req", "\"22F190\"", NULL, NULL, NULL, NULL },
    { "registry unit", "signals[5].unit", "\"hPa\"", NULL, NULL, NULL, NULL },
    { "registry unit 2", "signals[1].unit", "\"mph\"", NULL, NULL, NULL, NULL },
    { "veh unit free", "signals[7].unit", "\"degF\"", NULL, NULL, NULL, NULL },
    { "29-bit can id", "signals[2].src[0].can", "\"18FF1234\"", NULL, NULL, NULL, NULL },
    { "be signed can field", "signals[2].src[0].endian", "\"be\"", "signals[2].src[0].signed", "true", NULL, NULL },

    /* root / schema */
    { "schema major 2", "schema", "\"skygauge.vehicle/2\"", NULL, NULL, "E_SCHEMA", "schema" },
    { "schema minor junk", "schema", "\"skygauge.vehicle/1.x\"", NULL, NULL, "E_SCHEMA", "schema" },
    { "schema other", "schema", "\"other/1\"", NULL, NULL, "E_SCHEMA", "schema" },
    { "schema missing", "schema", NULL, NULL, NULL, "E_SCHEMA", "schema" },
    { "id format", "id", "\"Bad ID\"", NULL, NULL, "E_NAME", "id" },
    { "id too long", "id", "\"a23456789012345678901234567890123\"", NULL, NULL, "E_NAME", "id" },
    { "id missing", "id", NULL, NULL, NULL, "E_SCHEMA", "id" },
    { "rev zero", "rev", "0", NULL, NULL, "E_SCHEMA", "rev" },
    { "rev string", "rev", "\"1\"", NULL, NULL, "E_SCHEMA", "rev" },
    { "name too long", "name", "\"123456789012345678901234\"", NULL, NULL, "E_NAME", "name" },
    { "name non-ascii", "name", "\"Caf\\u00e9\"", NULL, NULL, "E_NAME", "name" },
    { "bus missing", "bus", NULL, NULL, NULL, NULL, NULL },
    { "unknown capability", "requires.elm", "[\"ATXX\"]", NULL, NULL, "E_SCHEMA", "requires.elm[0]" },
    { "signals empty", "signals", "[]", NULL, NULL, "E_SCHEMA", "signals" },

    /* services */
    { "service 04 clear DTC", "signals[0].src[0].req", "\"04\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 10 session", "signals[0].src[0].req", "\"1003\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 11 reset", "signals[0].src[0].req", "\"1101\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 27 security", "signals[0].src[0].req", "\"2701\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 2E write", "signals[0].src[0].req", "\"2EF190\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 31 routine", "signals[0].src[0].req", "\"3101FF00\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 3E tester", "signals[0].src[0].req", "\"3E00\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 14 clear", "signals[0].src[0].req", "\"14FFFFFF\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 03 dtc", "signals[0].src[0].req", "\"03\"", NULL, NULL, NULL, NULL },
    { "service 02 freeze", "signals[0].src[0].req", "\"0201\"", NULL, NULL, NULL, NULL },
    { "service 85 dtc ctl", "signals[0].src[0].req", "\"8502\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "22 short", "signals[0].src[0].req", "\"22DA\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 2F IO control", "signals[0].src[0].req", "\"2FF19003\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 34 download", "signals[0].src[0].req", "\"3400441000\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 35 upload", "signals[0].src[0].req", "\"3500441000\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 36 transfer", "signals[0].src[0].req", "\"360100\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 37 transfer exit", "signals[0].src[0].req", "\"37\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 3B write local id", "signals[0].src[0].req", "\"3B9001\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 19 read DTC info", "signals[0].src[0].req", "\"190209\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 23 read memory", "signals[0].src[0].req", "\"2312345678\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 3D write memory", "signals[0].src[0].req", "\"3D1200010000\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 28 comm control", "signals[0].src[0].req", "\"280301\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 07 pending DTC", "signals[0].src[0].req", "\"07\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 0A permanent DTC", "signals[0].src[0].req", "\"0A\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 08 on-board control", "signals[0].src[0].req", "\"0801\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "01 no pid", "signals[0].src[0].req", "\"01\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "09 no pid", "signals[0].src[0].req", "\"09\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "21 no pid", "signals[0].src[0].req", "\"21\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "service 06 monitor", "signals[0].src[0].req", "\"0601\"", NULL, NULL, NULL, NULL },
    { "service 07 pending DTC pid", "signals[0].src[0].req", "\"0701\"", NULL, NULL, NULL, NULL },
    { "service 0A permanent DTC pid", "signals[0].src[0].req", "\"0A01\"", NULL, NULL, NULL, NULL },
    { "service 1A local id", "signals[0].src[0].req", "\"1A01\"", NULL, NULL, NULL, NULL },
    { "service 23 memory address", "signals[0].src[0].req", "\"231234\"", NULL, NULL, NULL, NULL },
    { "raw ext IO control", "signals[6].src[0].raw", "\"18 04 2F DA 2E 03\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext routine", "signals[6].src[0].raw", "\"18 05 31 01 FF 00 00\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext security", "signals[6].src[0].raw", "\"18 02 27 01\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext download", "signals[6].src[0].raw", "\"18 02 34 00\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext write local id", "signals[6].src[0].raw", "\"18 03 3B 90 01\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext clear DTC", "signals[6].src[0].raw", "\"18 04 14 FF FF FF\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw normal write", "signals[6].src[0].raw", "\"04 2E DA 2E 00\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw normal session", "signals[6].src[0].raw", "\"02 10 03\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw normal reset", "signals[6].src[0].raw", "\"02 11 01\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw normal IO control", "signals[6].src[0].raw", "\"03 2F DA 2E\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "01 multi pid", "signals[0].src[0].req", "\"010C0D\"", NULL, NULL, "E_SERVICE", "signals[0].src[0].req" },
    { "req spaces", "signals[0].src[0].req", "\"01 0C\"", NULL, NULL, "E_SCHEMA", "signals[0].src[0].req" },
    { "req non-hex", "signals[0].src[0].req", "\"01G0\"", NULL, NULL, "E_SCHEMA", "signals[0].src[0].req" },
    { "req number", "signals[0].src[0].req", "268", NULL, NULL, "E_SCHEMA", "signals[0].src[0].req" },

    /* headers */
    { "21 on functional bus", "bus.hdr", "\"7DF\"", NULL, NULL, "E_HDR", "signals[3].src[1].req" },
    { "22 on 29-bit functional", "signals[5].src[0].hdr", "\"18DB33F1\"", NULL, NULL, "E_HDR", "signals[5].src[0].req" },
    { "hdr rx id", "signals[5].src[0].hdr", "\"7E8\"", NULL, NULL, "E_HDR", "signals[5].src[0].hdr" },
    { "hdr random", "signals[5].src[0].hdr", "\"123\"", NULL, NULL, "E_HDR", "signals[5].src[0].hdr" },
    { "hdr 29-bit bad tester", "signals[5].src[0].hdr", "\"18DA10F2\"", NULL, NULL, "E_HDR", "signals[5].src[0].hdr" },
    { "hdr 4 digits", "signals[5].src[0].hdr", "\"07E0\"", NULL, NULL, "E_HDR", "signals[5].src[0].hdr" },
    { "bus hdr below physical range", "bus.hdr", "\"500\"", NULL, NULL, "E_HDR", "bus.hdr" },
    { "rx tx id", "signals[6].src[0].rx", "\"7E0\"", NULL, NULL, "E_HDR", "signals[6].src[0].rx" },
    { "rx 29-bit wrong", "signals[6].src[0].rx", "\"18DA10F1\"", NULL, NULL, "E_HDR", "signals[6].src[0].rx" },
    { "cea without ATCEA", "requires.elm", "[\"ATMA\"]", NULL, NULL, "E_SCHEMA", "signals[7].src[0].cea" },
    { "cea format", "signals[7].src[0].cea", "\"1\"", NULL, NULL, "E_HDR", "signals[7].src[0].cea" },
    { "cea functional", "signals[7].src[0].hdr", "\"7DF\"", NULL, NULL, "E_HDR", "signals[7].src[0]" },
    { "cea with raw", "signals[6].src[0].cea", "\"10\"", NULL, NULL, "E_RAWFRAME", "signals[6].src[0].cea" },
    { "can id 800", "signals[2].src[0].can", "\"800\"", NULL, NULL, "E_HDR", "signals[2].src[0].can" },
    { "can id 9 digits", "signals[2].src[0].can", "\"1FFFFFFFF\"", NULL, NULL, "E_HDR", "signals[2].src[0].can" },

    /* raw frames */
    { "raw first frame", "signals[6].src[0].raw", "\"18 10 22 DA 2E\"", NULL, NULL, "E_RAWFRAME", "signals[6].src[0].raw" },
    { "raw ext 2E write", "signals[6].src[0].raw", "\"18 03 2E DA 2E\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ambiguous", "signals[6].src[0].raw", "\"02 01 0C\"", "signals[6].src[0].expect", "\"410C\"", "E_RAWFRAME",
      "signals[6].src[0].raw" },
    { "raw 22 too long", "signals[6].src[0].raw", "\"18 04 22 DA 2E 01\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw normal FF", "signals[6].src[0].raw", "\"10 08 22 DA 2E 00 00 00\"", NULL, NULL, "E_RAWFRAME",
      "signals[6].src[0].raw" },
    { "raw flow control", "signals[6].src[0].raw", "\"30 00 00\"", NULL, NULL, "E_RAWFRAME", "signals[6].src[0].raw" },
    { "raw 9 bytes", "signals[6].src[0].raw", "\"18 03 22 DA 2E 00 00 00 00\"", NULL, NULL, "E_RAWFRAME",
      "signals[6].src[0].raw" },
    { "raw ext tester present", "signals[6].src[0].raw", "\"18 02 3E 00\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext session", "signals[6].src[0].raw", "\"18 02 10 03\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext reset", "signals[6].src[0].raw", "\"18 02 11 01\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw ext clear dtc", "signals[6].src[0].raw", "\"18 01 04\"", NULL, NULL, "E_SERVICE", "signals[6].src[0].raw" },
    { "raw double space", "signals[6].src[0].raw", "\"18  03 22 DA 2E\"", NULL, NULL, "E_RAWFRAME", "signals[6].src[0].raw" },
    { "raw needs hdr", "signals[6].src[0].hdr", NULL, NULL, NULL, "E_HDR", "signals[6].src[0].hdr" },
    { "raw req mismatch", "signals[6].src[0].req", "\"22DA2F\"", NULL, NULL, "E_RAWFRAME", "signals[6].src[0].req" },
    { "expect wrong service", "signals[6].src[0].expect", "\"41\"", NULL, NULL, "E_SCHEMA", "signals[6].src[0].expect" },
    { "both req and can", "signals[2].src[0].req", "\"0105\"", NULL, NULL, "E_SCHEMA", "signals[2].src[0]" },

    /* AT whitelist */
    { "ATPP", "bus.init[0]", "\"ATPP 0E SV 8A\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATBRD", "bus.init[0]", "\"ATBRD 08\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATWS", "bus.init[0]", "\"ATWS\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATMA", "bus.init[0]", "\"ATMA\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATLP", "bus.init[0]", "\"ATLP\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATCAF0 in init", "bus.init[0]", "\"ATCAF0\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATCEA in init", "bus.init[0]", "\"ATCEA12\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "STN command", "bus.init[0]", "\"STPX h:7E0, d:0104\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATSP D", "bus.init[0]", "\"ATSP D\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATST 01", "bus.init[0]", "\"ATST 01\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATCRA functional", "bus.init[0]", "\"ATCRA 7DF\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "ATSH rx id", "bus.init[0]", "\"ATSH 7E8\"", NULL, NULL, "E_AT", "bus.init[0]" },
    { "init not string", "bus.init[0]", "5", NULL, NULL, "E_SCHEMA", "bus.init[0]" },
    { "init too many", "bus.init", "[\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\","
      "\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\",\"ATCAF1\"]",
      NULL, NULL, "E_LIMIT", "bus.init" },
    { "proto 13", "bus.proto", "13", NULL, NULL, "E_AT", "bus.proto" },
    { "atst 1", "bus.atst", "1", NULL, NULL, "E_AT", "bus.atst" },
    { "gap 5000", "bus.gap_ms", "5000", NULL, NULL, "E_RATE", "bus.gap_ms" },
    { "check connection service", "bus.check_connection", "\"0902\"", NULL, NULL, NULL, NULL },
    { "tester present", "bus.tester_present", "\"3E00\"", "bus.tester_present_every_ms", "2000", NULL, NULL },
    { "check connection malformed", "bus.check_connection", "\"010\"", NULL, NULL, "E_REQUEST", "bus.check_connection" },
    { "tester present wrong service", "bus.tester_present", "\"2200\"", NULL, NULL, "E_REQUEST", "bus.tester_present" },
    { "tester present too fast", "bus.tester_present_every_ms", "-1", NULL, NULL, "E_RATE", "bus.tester_present_every_ms" },

    /* RPN */
    { "rpn underflow", "signals[0].src[0].rpn", "[\"+\"]", NULL, NULL, "E_RPN_STACK", "signals[0].src[0].rpn[0]" },
    { "rpn overflow", "signals[0].src[0].rpn",
      "[[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1],[\"k\",1]]", NULL, NULL,
      "E_RPN_STACK", "signals[0].src[0].rpn[8]" },
    { "rpn two results", "signals[0].src[0].rpn", "[[\"A\",0],[\"A\",1]]", NULL, NULL, "E_RPN_STACK",
      "signals[0].src[0].rpn" },
    { "rpn empty", "signals[0].src[0].rpn", "[]", NULL, NULL, "E_RPN_STACK", "signals[0].src[0].rpn" },
    { "rpn index", "signals[0].src[0].rpn", "[[\"A\",2]]", NULL, NULL, "E_RPN_INDEX", "signals[0].src[0].rpn[0]" },
    { "rpn no len", "signals[0].src[0].len", NULL, NULL, NULL, "E_RPN_INDEX", "signals[0].src[0].rpn[0]" },
    { "rpn raw field", "signals[5].src[0].rpn", "[[\"R\",8,16]]", NULL, NULL, "E_RPN_INDEX", "signals[5].src[0].rpn[0]" },
    { "X in plain req", "signals[1].src[0].rpn", "[[\"X\"]]", NULL, NULL, "E_RPN_INDEX", "signals[1].src[0].rpn[0]" },
    { "A in byte_from_end", "signals[3].src[1].rpn", "[[\"A\",0],[\"k\",40],\"-\"]", NULL, NULL, "E_RPN_INDEX",
      "signals[3].src[1].rpn[0]" },
    { "rpn const range", "signals[0].src[0].rpn", "[[\"k\",1e8]]", NULL, NULL, "E_RPN_CONST", "signals[0].src[0].rpn[0]" },
    { "rpn div const 0", "signals[0].src[0].rpn", "[[\"A\",0],[\"k\",0],\"/\"]", NULL, NULL, "E_RPN_CONST",
      "signals[0].src[0].rpn[2]" },
    { "rpn unknown op", "signals[0].src[0].rpn", "[[\"A\",0],\"pow\"]", NULL, NULL, "E_RPN_OP", "signals[0].src[0].rpn[1]" },
    { "rpn lut index", "signals[6].src[0].rpn", "[[\"A\",0],[\"lut\",3]]", NULL, NULL, "E_RPN_INDEX",
      "signals[6].src[0].rpn[1]" },
    { "rpn missing, expr only", "signals[0].src[0].rpn", NULL, NULL, NULL, "E_SCHEMA", "signals[0].src[0].rpn" },
    { "rpn not array", "signals[0].src[0].rpn", "\"(A*256+B)/4\"", NULL, NULL, "E_SCHEMA", "signals[0].src[0].rpn" },
    { "can field past 64", "signals[2].src[0].bit", "60", NULL, NULL, "E_RPN_INDEX", "signals[2].src[0].bits" },
    { "can bits 0", "signals[2].src[0].bits", "0", NULL, NULL, "E_RPN_INDEX", "signals[2].src[0].bits" },
    { "can bit missing", "signals[2].src[0].bit", NULL, NULL, NULL, "E_SCHEMA", "signals[2].src[0].bit" },

    /* signals */
    { "unknown obd signal", "signals[9].id", "\"obd.unknown\"", NULL, NULL, "E_SIGNAL", "signals[9].id" },
    { "sys signal", "signals[9].id", "\"sys.uptime\"", NULL, NULL, "E_SIGNAL", "signals[9].id" },
    { "duplicate signal", "signals[9].id", "\"obd.rpm\"", NULL, NULL, "E_SIGNAL", "signals[9].id" },
    { "veh upper case", "signals[9].id", "\"veh.Bad\"", NULL, NULL, "E_NAME", "signals[9].id" },
    { "veh empty", "signals[9].id", "\"veh.\"", NULL, NULL, "E_NAME", "signals[9].id" },
    { "signal id missing", "signals[9].id", NULL, NULL, NULL, "E_SCHEMA", "signals[9].id" },
    { "signal name long", "signals[7].name", "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"", NULL, NULL, "E_NAME", "signals[7].name" },
    { "src > 4", "signals[0].src", "[{\"req\":\"010C\",\"len\":2,\"rpn\":[[\"A\",0]]},"
      "{\"req\":\"010C\",\"len\":2,\"rpn\":[[\"A\",0]]},{\"req\":\"010C\",\"len\":2,\"rpn\":[[\"A\",0]]},"
      "{\"req\":\"010C\",\"len\":2,\"rpn\":[[\"A\",0]]},{\"req\":\"010C\",\"len\":2,\"rpn\":[[\"A\",0]]}]",
      NULL, NULL, "E_LIMIT", "signals[0].src" },
    { "src empty", "signals[0].src", "[]", NULL, NULL, "E_SCHEMA", "signals[0].src" },
    { "src missing", "signals[0].src", NULL, NULL, NULL, "E_SCHEMA", "signals[0].src" },
    { "derive non-gear", "signals[4].derive", "\"ratio\"", NULL, NULL, "E_SCHEMA", "signals[4].derive" },
    { "derive with src", "signals[6].derive", "\"ratio\"", NULL, NULL, "E_SCHEMA", "signals[6].src" },
    { "ratio without drivetrain", "drivetrain", NULL, NULL, NULL, "E_SCHEMA", "signals[6].fallback" },
    { "ratio without rpm", "signals[0].id", "\"veh.rpm2\"", NULL, NULL, "E_SCHEMA", "signals[6].fallback" },
    { "zero_if_rpm without rpm", "signals[0].id", "\"veh.rpm2\"", NULL, NULL, "E_SCHEMA",
      "signals[1].post.zero_if_rpm_lt" },
    { "bad rate", "signals[0].rate", "\"fast\"", NULL, NULL, "E_SCHEMA", "signals[0].rate" },
    { "bad filter", "signals[0].filter", "\"kalman\"", NULL, NULL, "E_SCHEMA", "signals[0].filter" },
    { "valid reversed", "signals[0].valid", "[5,1]", NULL, NULL, "E_SCHEMA", "signals[0].valid" },
    { "clamp malformed", "signals[5].post.clamp", "[0]", NULL, NULL, "E_SCHEMA", "signals[5].post.clamp" },
    { "div without slow", "signals[7].rate", "\"round\"", NULL, NULL, "E_SCHEMA", "signals[7].div" },
    { "unit too long", "signals[7].unit", "\"degC/100\"", NULL, NULL, "E_SCHEMA", "signals[7].unit" },
    { "unit not in registry", "signals[5].unit", "\"mbar\"", NULL, NULL, "E_SCHEMA", "signals[5].unit" },
    { "unit of other signal", "signals[1].unit", "\"C\"", NULL, NULL, "E_SCHEMA", "signals[1].unit" },
    { "unit case", "signals[0].unit", "\"RPM\"", NULL, NULL, "E_SCHEMA", "signals[0].unit" },

    /* rates */
    { "three every", "signals[1].rate", "\"every\"", "signals[2].rate", "\"every\"", "E_RATE", "signals" },
    { "period too short", "signals[8].period_ms", "10", NULL, NULL, "E_RATE", "signals[8].period_ms" },
    { "period with every", "signals[0].period_ms", "100", NULL, NULL, "E_RATE", "signals[0].period_ms" },
    { "monitor every 100", "monitor.every_ms", "100", NULL, NULL, "E_RATE", "monitor.every_ms" },
    { "monitor window 300", "monitor.window_ms", "300", NULL, NULL, "E_RATE", "monitor.window_ms" },
    { "monitor continuous with requests", "monitor.mode", "\"continuous\"", NULL, NULL, "E_SCHEMA", "monitor.mode" },
    { "monitor mode missing", "monitor.mode", NULL, NULL, NULL, "E_SCHEMA", "monitor.mode" },
    { "monitor without ATMA", "requires.elm", "[\"ATCEA\"]", NULL, NULL, "E_SCHEMA", "monitor" },
    { "can without monitor", "monitor", NULL, NULL, NULL, "E_SCHEMA", "signals[2].src[0].can" },

    /* sizes */
    { "lut lo > hi", "luts[0][0]", "[3,1,0]", NULL, NULL, "E_SCHEMA", "luts[0][0]" },
    { "lut entry arity", "luts[0][0]", "[1]", NULL, NULL, "E_SCHEMA", "luts[0][0]" },
    { "gears 9", "drivetrain.gears", "[1,1,1,1,1,1,1,1,1]", NULL, NULL, "E_LIMIT", "drivetrain.gears" },
    { "gear zero", "drivetrain.gears[0]", "0", NULL, NULL, "E_SCHEMA", "drivetrain.gears[0]" },
    { "hold incomplete", "signals[3].src[1].hold.gap_ms", NULL, NULL, NULL, "E_SCHEMA", "signals[3].src[1].hold" },
    { "byte_from_end w/o multiframe", "signals[3].src[1].multiframe", NULL, NULL, NULL, "E_SCHEMA",
      "signals[3].src[1].byte_from_end" },
    { "len 65", "signals[0].src[0].len", "65", NULL, NULL, "E_LIMIT", "signals[0].src[0].len" },
    { "fail_after 0", "signals[0].src[0].fail_after", "0", NULL, NULL, "E_SCHEMA", "signals[0].src[0].fail_after" },
};

static void test_mutations(void)
{
    size_t i;
    for (i = 0; i < N(k_cases); i++) {
        const mcase_t *mc = &k_cases[i];
        vp_issue_t store[MAX_ISSUES];
        vp_issues_t l;
        cJSON *root = cJSON_Parse(s_full_text);
        size_t errs;

        VT_CHECK_MSG(jset(root, mc->path1, mc->val1), "%s: cannot apply %s", mc->what, mc->path1);
        if (mc->path2) VT_CHECK_MSG(jset(root, mc->path2, mc->val2), "%s: cannot apply %s", mc->what, mc->path2);
        errs = run(root, &l, store);
        if (!mc->code) {
            VT_CHECK_MSG(errs == 0, "%s: expected valid, got %u errors", mc->what, (unsigned)errs);
            if (errs) vt_print_issues(&l, "    ");
        } else {
            VT_CHECK_MSG(errs > 0, "%s: expected %s, profile passed", mc->what, mc->code);
            VT_CHECK_MSG(find(&l, mc->code, mc->at) != NULL, "%s: no %s at %s", mc->what, mc->code, mc->at);
            if (!find(&l, mc->code, mc->at)) vt_print_issues(&l, "    got: ");
        }
        cJSON_Delete(root);
    }
    printf("  %u mutation cases\n", (unsigned)N(k_cases));
}

/* ---- programmatic limit cases ---- */

enum { VEH_SHARED_REQ, VEH_DISTINCT_REQ, VEH_CAN };

static cJSON *add_veh(cJSON *root, int n, int kind)
{
    cJSON *signals = cJSON_GetObjectItemCaseSensitive(root, "signals");
    int i;
    for (i = 0; i < n; i++) {
        char buf[160];
        if (kind == VEH_SHARED_REQ) {
            snprintf(buf, sizeof(buf), "{\"id\":\"veh.x%d\",\"src\":[{\"req\":\"0105\",\"len\":1,"
                     "\"rpn\":[[\"A\",0]]}]}", i);
        } else if (kind == VEH_DISTINCT_REQ) {
            snprintf(buf, sizeof(buf), "{\"id\":\"veh.x%d\",\"src\":[{\"req\":\"2201%02X\",\"hdr\":\"7E0\","
                     "\"len\":1,\"rpn\":[[\"A\",0]]}]}", i, i + 0x10);
        } else {
            snprintf(buf, sizeof(buf), "{\"id\":\"veh.x%d\",\"src\":[{\"can\":\"%03X\",\"bit\":0,"
                     "\"bits\":8,\"rpn\":[[\"X\"]]}]}", i, i + 0x10);
        }
        cJSON_AddItemToArray(signals, cJSON_Parse(buf));
    }
    return root;
}

static void expect_limit(cJSON *root, const char *code, const char *msg_part, const char *what)
{
    vp_issue_t store[MAX_ISSUES];
    vp_issues_t l;
    size_t i;
    bool hit = false;
    run(root, &l, store);
    for (i = 0; i < l.count; i++) {
        if (!strcmp(l.items[i].code, code) && strstr(l.items[i].msg, msg_part)) hit = true;
    }
    VT_CHECK_MSG(hit, "%s: no %s \"%s\"", what, code, msg_part);
    if (!hit) vt_print_issues(&l, "    got: ");
    cJSON_Delete(root);
}

static void test_limits(void)
{
    /* 33 veh signals sharing one request */
    expect_limit(add_veh(cJSON_Parse(s_full_text), 30, VEH_SHARED_REQ),
                 "E_LIMIT", "veh.* signals", "33 veh");
    /* 49 signals */
    {
        cJSON *root = add_veh(cJSON_Parse(s_full_text), 39, VEH_SHARED_REQ);
        expect_limit(root, "E_LIMIT", "signals, max", "49 signals");
    }
    /* 9 + 24 distinct requests */
    expect_limit(add_veh(cJSON_Parse(s_full_text), 24, VEH_DISTINCT_REQ),
                 "E_LIMIT", "distinct requests", "33 requests");
    /* 23 more distinct requests is still fine (32 total) */
    {
        vp_issue_t store[MAX_ISSUES];
        vp_issues_t l;
        cJSON *root = add_veh(cJSON_Parse(s_full_text), 23, VEH_DISTINCT_REQ);
        VT_CHECK_INT(run(root, &l, store), 0);
        if (l.count) vt_print_issues(&l, "    ");
        cJSON_Delete(root);
    }
    /* 2 + 15 can sources */
    expect_limit(add_veh(cJSON_Parse(s_full_text), 15, VEH_CAN),
                 "E_LIMIT", "can sources", "17 can");
    /* request rate: three period-limited signals at 20 ms = 150 req/s */
    {
        cJSON *root = cJSON_Parse(s_full_text);
        jset(root, "signals[8].period_ms", "20");
        jset(root, "signals[9].period_ms", "20");
        jset(root, "signals[4].period_ms", "20");
        expect_limit(root, "E_RATE", "req/s", "rate sum");
    }
    /* lut tables / entries */
    {
        cJSON *root = cJSON_Parse(s_full_text);
        jset(root, "luts", "[[[0,0]],[[0,0]],[[0,0]],[[0,0]],[[0,0]],[[0,0]],[[0,0]],[[0,0]],[[0,0]]]");
        expect_limit(root, "E_LIMIT", "tables", "9 luts");
    }
    {
        cJSON *root = cJSON_Parse(s_full_text);
        jset(root, "luts[0]", "[[0,0],[1,0],[2,0],[3,0],[4,0],[5,0],[6,0],[7,0],[8,0],[9,0],[10,0],[11,0],"
                              "[12,0],[13,0],[14,0],[15,0],[16,0]]");
        expect_limit(root, "E_LIMIT", "entries", "17 lut entries");
    }
    /* 33 instructions */
    {
        char buf[512];
        int i, len = snprintf(buf, sizeof(buf), "[[\"A\",0]");
        cJSON *root = cJSON_Parse(s_full_text);
        for (i = 0; i < 32; i++) len += snprintf(buf + len, sizeof(buf) - (size_t)len, ",\"neg\"");
        snprintf(buf + len, sizeof(buf) - (size_t)len, "]");
        jset(root, "signals[0].src[0].rpn", buf);
        expect_limit(root, "E_LIMIT", "instructions", "33 insns");
    }
}

static void test_misc(void)
{
    vp_issue_t store[MAX_ISSUES];
    vp_issues_t l;
    cJSON *root;
    char *big;

    /* unknown keys are warnings only */
    root = cJSON_Parse(s_full_text);
    jset(root, "colour", "\"red\"");
    jset(root, "signals[0].src[0].comment", "\"x\"");
    VT_CHECK_INT(run(root, &l, store), 0);
    VT_CHECK_INT(l.n_warnings, 2);
    VT_CHECK(find(&l, "W_UNKNOWN", "colour") != NULL);
    VT_CHECK(find(&l, "W_UNKNOWN", "signals[0].src[0].comment") != NULL);
    VT_CHECK_INT(l.items[0].severity, VP_SEV_WARN);
    cJSON_Delete(root);

    /* monitor without can sources: warning */
    root = cJSON_Parse(s_min_text);
    jset(root, "requires", "{\"elm\":[\"ATMA\"]}");
    jset(root, "monitor", "{\"mode\":\"probe\",\"every_ms\":500,\"window_ms\":100}");
    VT_CHECK_INT(run(root, &l, store), 0);
    VT_CHECK(find(&l, "W_UNUSED", "monitor") != NULL);
    cJSON_Delete(root);

    /* not an object */
    root = cJSON_Parse("[1,2]");
    VT_CHECK_INT(run(root, &l, store), 1);
    VT_CHECK(find(&l, "E_SCHEMA", "$") != NULL);
    cJSON_Delete(root);
    VT_CHECK(vp_validate(NULL, &s_cfg, NULL) == 1);

    /* text entry points */
    vp_issues_init(&l, store, MAX_ISSUES);
    VT_CHECK_INT(vp_validate_json("{\"schema\":", 10, &s_cfg, &l), 1);
    VT_CHECK(find(&l, "E_SCHEMA", "$") != NULL);
    big = (char *)malloc(VP_MAX_JSON_BYTES + 2);
    memset(big, ' ', VP_MAX_JSON_BYTES + 1);
    big[VP_MAX_JSON_BYTES + 1] = '\0';
    vp_issues_init(&l, store, MAX_ISSUES);
    VT_CHECK_INT(vp_validate_json(big, VP_MAX_JSON_BYTES + 1, &s_cfg, &l), 1);
    VT_CHECK(find(&l, "E_SIZE", "$") != NULL);
    free(big);
    /* no NUL terminator needed */
    {
        size_t n = strlen(s_min_text);
        char *copy = (char *)malloc(n);
        memcpy(copy, s_min_text, n);
        vp_issues_init(&l, store, MAX_ISSUES);
        VT_CHECK_INT(vp_validate_json(copy, n, &s_cfg, &l), 0);
        free(copy);
    }

    /* full issue list keeps counting */
    {
        vp_issue_t two[2];
        root = cJSON_Parse(s_full_text);
        jset(root, "id", "\"BAD\"");
        jset(root, "name", "\"\"");
        jset(root, "bus.proto", "99");
        jset(root, "signals[0].src[0].req", "\"04\"");
        vp_issues_init(&l, two, 2);
        VT_CHECK_INT(vp_validate(root, &s_cfg, &l), 4);
        VT_CHECK_INT(l.count, 2);
        VT_CHECK_INT(l.n_errors, 4);
        cJSON_Delete(root);
    }

    /* empty registry rejects every obd.* id */
    {
        vp_validate_cfg_t none = { NULL, 0, 0, NULL };
        root = cJSON_Parse(s_min_text);
        vp_issues_init(&l, store, MAX_ISSUES);
        VT_CHECK_INT(vp_validate(root, &none, &l), 1);
        VT_CHECK(find(&l, "E_SIGNAL", "signals[0].id") != NULL);
        cJSON_Delete(root);
    }
}

static void test_default_cfg(void)
{
    const vp_validate_cfg_t *c = vp_validate_default_cfg();
    size_t i;

    VT_CHECK(c != NULL && c == vp_validate_default_cfg());
    VT_CHECK_INT(c->n_registry, SIG_OBD_COUNT);
    VT_CHECK_INT(c->max_json_bytes, VP_MAX_JSON_BYTES);
    VT_CHECK(c->unit_ok != NULL);
    for (i = 0; i < c->n_registry; i++) {
        const sig_def_t *d = &sig_registry[SIG_OBD_FIRST + i];
        const vp_sig_def_t *r = &c->registry[i];
        VT_CHECK(!strcmp(r->key, d->key));
        VT_CHECK_INT(r->num_id, d->num_id);
        VT_CHECK_INT(r->editor_index, d->editor_index);
        VT_CHECK_INT(r->scale, d->scale);
        VT_CHECK_INT(r->n_units, d->n_units);
        VT_CHECK(r->unit_default && !strcmp(r->unit_default, d->units[0].unit));
        VT_CHECK(c->unit_ok(i, r->unit_default));
        VT_CHECK(!c->unit_ok(i, "nope"));
    }
    VT_CHECK(!strcmp(c->registry[0].key, "obd.rpm"));
    VT_CHECK_INT(c->registry[0].num_id, 0x0001);
    VT_CHECK(!strcmp(c->registry[SIG_OBD_OIL_PRESSURE - SIG_OBD_FIRST].unit_default, "bar"));
    VT_CHECK(c->unit_ok(SIG_OBD_OIL_PRESSURE - SIG_OBD_FIRST, "hPa"));
    VT_CHECK(c->unit_ok(SIG_OBD_BATTERY_VOLTAGE - SIG_OBD_FIRST, "mV"));
    VT_CHECK(!c->unit_ok(SIG_OBD_RPM - SIG_OBD_FIRST, "C"));
    VT_CHECK(!c->unit_ok(c->n_registry, "rpm"));
    VT_CHECK(!c->unit_ok(0, NULL));
}

static void test_at_check(void)
{
    static const struct {
        const char *cmd;
        int         ctx;
        bool        ok;
        const char *norm;
    } cases[] = {
        { "ATSH7E0", VP_AT_CTX_INIT, true, "ATSH7E0" },
        { "ATSH8111F1", VP_AT_CTX_INIT, true, "ATSH8111F1" },
        { "ATSH8101F1", VP_AT_CTX_INIT, true, "ATSH8101F1" },
        { "ATSH8001F1", VP_AT_CTX_INIT, true, "ATSH8001F1" },
        { "ATSH8111F1", VP_AT_CTX_REQUEST, false, NULL },
        { "ATSH8111F1", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSH8122F1", VP_AT_CTX_INIT, false, NULL },
        { "at sh 7df\r", VP_AT_CTX_INIT, true, "ATSH7DF" },
        { "ATSH 18DA10F1", VP_AT_CTX_INIT, true, "ATSH18DA10F1" },
        { "ATSH 6F1", VP_AT_CTX_INIT, true, "ATSH6F1" },
        { "ATSH 7E8", VP_AT_CTX_INIT, false, NULL },
        { "ATSH 01 02 03", VP_AT_CTX_INIT, false, NULL },
        { "ATCRA", VP_AT_CTX_INIT, true, "ATCRA" },
        { "ATCRA 618", VP_AT_CTX_INIT, true, "ATCRA618" },
        { "ATCRA 7XX", VP_AT_CTX_INIT, false, NULL },
        { "ATCEA", VP_AT_CTX_ENGINE, true, "ATCEA" },
        { "ATCEA 18", VP_AT_CTX_ENGINE, true, "ATCEA18" },
        { "ATCEA 1", VP_AT_CTX_ENGINE, false, NULL },
        { "ATCEA 18", VP_AT_CTX_INIT, false, NULL },
        { "ATCAF0", VP_AT_CTX_ENGINE, true, "ATCAF0" },
        { "ATCAF0", VP_AT_CTX_INIT, false, NULL },
        { "ATCAF0", VP_AT_CTX_REQUEST, true, "ATCAF0" },
        { "ATCAF1", VP_AT_CTX_INIT, true, "ATCAF1" },
        { "ATCAF1", VP_AT_CTX_REQUEST, true, "ATCAF1" },
        { "ATCFC0", VP_AT_CTX_REQUEST, true, "ATCFC0" },
        { "ATCFC1", VP_AT_CTX_REQUEST, true, "ATCFC1" },
        { "ATAL", VP_AT_CTX_REQUEST, true, "ATAL" },
        { "ATCAF2", VP_AT_CTX_INIT, false, NULL },
        { "ATST0A", VP_AT_CTX_INIT, true, "ATST0A" },
        { "ATST FF", VP_AT_CTX_INIT, true, "ATSTFF" },
        { "ATST 02", VP_AT_CTX_INIT, true, "ATST02" },
        { "ATST 00", VP_AT_CTX_INIT, false, NULL },
        { "ATST 1", VP_AT_CTX_INIT, false, NULL },
        { "ATSP0", VP_AT_CTX_INIT, true, "ATSP0" },
        { "ATSPA", VP_AT_CTX_INIT, true, "ATSPA" },
        { "ATSP C", VP_AT_CTX_INIT, true, "ATSPC" },
        { "ATSP A6", VP_AT_CTX_INIT, true, "ATSPA6" },
        { "ATSP AA", VP_AT_CTX_INIT, true, "ATSPAA" },
        { "ATSP A0", VP_AT_CTX_INIT, false, NULL },
        { "ATSPA0", VP_AT_CTX_INIT, false, NULL },
        { "ATSP 66", VP_AT_CTX_INIT, false, NULL },
        { "ATSP6", VP_AT_CTX_REQUEST, false, NULL },
        { "ATDPN", VP_AT_CTX_ENGINE, true, "ATDPN" },
        { "ATDPN", VP_AT_CTX_INIT, false, NULL },
        { "ATDPN", VP_AT_CTX_REQUEST, false, NULL },
        { "ATAT0", VP_AT_CTX_INIT, true, "ATAT0" },
        { "ATAT2", VP_AT_CTX_INIT, true, "ATAT2" },
        { "ATE0", VP_AT_CTX_INIT, true, "ATE0" },
        { "ATH1", VP_AT_CTX_INIT, true, "ATH1" },
        { "ATS0", VP_AT_CTX_INIT, true, "ATS0" },
        { "ATM0", VP_AT_CTX_INIT, true, "ATM0" },
        { "ATAT1", VP_AT_CTX_INIT, true, "ATAT1" },
        { "ATFCSH 18DADAF1", VP_AT_CTX_INIT, true, "ATFCSH18DADAF1" },
        { "ATFCSD 30 00 00", VP_AT_CTX_INIT, true, "ATFCSD300000" },
        { "ATFCSM1", VP_AT_CTX_INIT, true, "ATFCSM1" },
        { "ATCP 18", VP_AT_CTX_INIT, true, "ATCP18" },
        { "ATIB10", VP_AT_CTX_INIT, true, "ATIB10" },
        { "ATSW 05", VP_AT_CTX_INIT, true, "ATSW05" },
        { "ATIIA 7A", VP_AT_CTX_INIT, true, "ATIIA7A" },
        { "ATSI", VP_AT_CTX_INIT, true, "ATSI" },
        { "ATFI", VP_AT_CTX_INIT, true, "ATFI" },
        { "ATWM 81 7A F1 3E", VP_AT_CTX_INIT, true, "ATWM817AF13E" },
        { "ATTAF1", VP_AT_CTX_INIT, true, "ATTAF1" },
        { "ATCER", VP_AT_CTX_INIT, true, "ATCER" },
        { "ATPP 0E SV 8A", VP_AT_CTX_ENGINE, false, NULL },
        { "ATPPS", VP_AT_CTX_ENGINE, false, NULL },
        { "ATBRD 45", VP_AT_CTX_ENGINE, false, NULL },
        { "ATBRT 0F", VP_AT_CTX_ENGINE, false, NULL },
        /* fixed link-setup / monitor / voltage commands: firmware only */
        { "ATZ", VP_AT_CTX_ENGINE, true, "ATZ" },
        { "AT E0", VP_AT_CTX_ENGINE, true, "ATE0" },
        { "ATL0", VP_AT_CTX_ENGINE, true, "ATL0" },
        { "ATS1", VP_AT_CTX_ENGINE, true, "ATS1" },
        { "ATH0", VP_AT_CTX_ENGINE, true, "ATH0" },
        { "ATH1", VP_AT_CTX_ENGINE, true, "ATH1" },
        { "ATAT1", VP_AT_CTX_ENGINE, true, "ATAT1" },
        { "ATMA", VP_AT_CTX_ENGINE, true, "ATMA" },
        { "AT AR", VP_AT_CTX_ENGINE, true, "ATAR" },
        { "ATRV", VP_AT_CTX_ENGINE, true, "ATRV" },
        { "ATI", VP_AT_CTX_ENGINE, true, "ATI" },
        { "ATZ", VP_AT_CTX_INIT, true, "ATZ" },
        { "ATD", VP_AT_CTX_INIT, true, "ATD" },
        { "ATD0", VP_AT_CTX_INIT, true, "ATD0" },
        { "ATAL", VP_AT_CTX_INIT, true, "ATAL" },
        { "2212010401", VP_AT_CTX_INIT, true, "2212010401" },
        { "22 12 01 04 01", VP_AT_CTX_INIT, true, "2212010401" },
        { "2212010401", VP_AT_CTX_ENGINE, false, NULL },
        { "2212010401", VP_AT_CTX_REQUEST, false, NULL },
        { "ATMA", VP_AT_CTX_INIT, false, NULL },
        { "ATAR", VP_AT_CTX_INIT, false, NULL },
        { "ATRV", VP_AT_CTX_INIT, false, NULL },
        { "ATAT2", VP_AT_CTX_ENGINE, false, NULL },
        { "ATWS", VP_AT_CTX_ENGINE, false, NULL },
        { "ATD", VP_AT_CTX_ENGINE, false, NULL },
        { "ATLP", VP_AT_CTX_ENGINE, false, NULL },
        { "ATE1", VP_AT_CTX_ENGINE, false, NULL },
        { "ATMR 10", VP_AT_CTX_ENGINE, false, NULL },
        { "ATMT 10", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSD 00 11", VP_AT_CTX_ENGINE, false, NULL },
        { "ATWM 81 10 F1 3E", VP_AT_CTX_ENGINE, false, NULL },
        { "ATKW0", VP_AT_CTX_ENGINE, false, NULL },
        { "ATRD", VP_AT_CTX_ENGINE, false, NULL },
        { "ATPPFF OFF", VP_AT_CTX_INIT, false, NULL },
        { "ATBRD 45", VP_AT_CTX_INIT, false, NULL },
        { "ATCEA", VP_AT_CTX_INIT, false, NULL },
        { "ATCSM0", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSW 00", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSW 00", VP_AT_CTX_REQUEST, false, NULL },
        { "ATWM 3E 01", VP_AT_CTX_REQUEST, false, NULL },
        { "STSBR 2000000", VP_AT_CTX_ENGINE, false, NULL },
        { "0104", VP_AT_CTX_ENGINE, false, NULL },
        { "AT", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSH7E0\x01", VP_AT_CTX_ENGINE, false, NULL },
        { "ATSH 18DA10F1 0000", VP_AT_CTX_ENGINE, false, NULL },
        { "", VP_AT_CTX_ENGINE, false, NULL },
    };
    size_t i;
    char norm[VP_AT_MAX_LEN + 1];

    for (i = 0; i < N(cases); i++) {
        const char *why = NULL;
        bool ok = vp_at_check(cases[i].cmd, (vp_at_ctx_t)cases[i].ctx, norm, sizeof(norm), &why);
        VT_CHECK_MSG(ok == cases[i].ok, "\"%s\" ctx %d -> %d (%s)", cases[i].cmd, cases[i].ctx, ok, why ? why : "");
        if (ok && cases[i].norm) VT_CHECK_MSG(!strcmp(norm, cases[i].norm), "\"%s\" -> \"%s\"", cases[i].cmd, norm);
        if (!ok) VT_CHECK_MSG(why != NULL, "\"%s\" refused without reason", cases[i].cmd);
    }
    VT_CHECK(!vp_at_check(NULL, VP_AT_CTX_ENGINE, NULL, 0, NULL));
}

int main(void)
{
    s_full_text = vt_read_file(VT_FIXTURE_DIR "/good_full.json", NULL);
    s_min_text = vt_read_file(VT_FIXTURE_DIR "/good_min.json", NULL);
    if (!s_full_text || !s_min_text) {
        fprintf(stderr, "cannot read fixtures from %s\n", VT_FIXTURE_DIR);
        return 2;
    }
    s_cfg = *vp_validate_default_cfg();

    VT_RUN(test_good);
    VT_RUN(test_mutations);
    VT_RUN(test_limits);
    VT_RUN(test_misc);
    VT_RUN(test_default_cfg);
    VT_RUN(test_at_check);
    free(s_full_text);
    free(s_min_text);
    return VT_SUMMARY("test_validate");
}
