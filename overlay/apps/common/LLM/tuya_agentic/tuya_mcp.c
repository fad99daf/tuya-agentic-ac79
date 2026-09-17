/* Device-side MCP registry and non-blocking dispatcher for AC79 Agentic. */
#include <stdio.h>
#include <string.h>

#include "system/includes.h"
#include "tuya_ai_select.h"
#include "tuya_mcp.h"

/* audio_input.h cannot be included here because its SDK FILE definitions
 * conflict with the Agentic Kit's newlib stdio headers. */
int _device_set_play_volume(int volume);
int _device_get_play_volume(void);

#define MCP_QUEUE_DEPTH       4
#define MCP_RESPONSE_MAX      768
#define MCP_ID_MAX            40
#define MCP_VOLUME_TOOL       "self.audio_speaker.set_volume"

typedef struct {
    char response[MCP_RESPONSE_MAX];
    unsigned int response_len;
    int volume;                 /* -1 means this item has no board action. */
    char id[MCP_ID_MAX];
} mcp_job_t;

static mcp_job_t s_jobs[MCP_QUEUE_DEPTH];
static volatile unsigned int s_head;
static volatile unsigned int s_tail;
static volatile unsigned int s_count;
static volatile unsigned int s_drops;

static const char *mcp_value_after_key(const char *json, const char *key)
{
    const char *p = json;
    size_t key_len = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        p += key_len;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            return p;
        }
    }
    return NULL;
}

static int mcp_copy_string(const char *json, const char *key, char *out, unsigned int cap)
{
    const char *p = mcp_value_after_key(json, key);
    unsigned int n = 0;

    if (!p || !cap || *p++ != '"') return -1;
    while (*p && *p != '"') {
        if (*p == '\\' || n + 1 >= cap) return -1;
        out[n++] = *p++;
    }
    if (*p != '"') return -1;
    out[n] = 0;
    return 0;
}

static void mcp_copy_id(const char *json, char *id, unsigned int cap)
{
    const char *p = mcp_value_after_key(json, "\"id\"");
    const char *start;
    unsigned int n;

    if (cap < 2) return;
    id[0] = '1';
    id[1] = 0;
    if (!p) return;
    start = p;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && *p != '\\') p++;
        if (*p != '"') return;
        p++;
    } else {
        while (*p >= '0' && *p <= '9') p++;
        if (p == start) return;
    }
    n = (unsigned int)(p - start);
    if (n + 1 > cap) return;
    memcpy(id, start, n);
    id[n] = 0;
}

static int mcp_get_volume(const char *json, int *volume)
{
    const char *p = mcp_value_after_key(json, "\"volume\"");
    int v = 0;

    if (!p || *p < '0' || *p > '9') return -1;
    do {
        v = v * 10 + (*p++ - '0');
        if (v > 100) return -1;
    } while (*p >= '0' && *p <= '9');

    /* omniClient may serialize an integer schema value as 50.0.  Permit only
     * a zero-only fractional suffix; never silently round 50.5. */
    if (*p == '.') {
        const char *fraction = ++p;
        while (*p >= '0' && *p <= '9') {
            if (*p++ != '0') return -1;
        }
        if (p == fraction) return -1;
    }
    if (*p && *p != ',' && *p != '}' && *p != ']' &&
        *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return -1;
    *volume = v;
    return 0;
}

static void mcp_enqueue(const char *response, unsigned int len, int volume, const char *id)
{
    mcp_job_t *job;

    if (len >= MCP_RESPONSE_MAX) return;
    OS_ENTER_CRITICAL();
    if (s_count == MCP_QUEUE_DEPTH) {
        /* Requests are ordered JSON-RPC calls.  Preserve the newer request
         * rather than blocking the transport callback. */
        s_head = (s_head + 1) % MCP_QUEUE_DEPTH;
        s_count--;
        s_drops++;
    }
    job = &s_jobs[s_tail];
    memcpy(job->response, response, len);
    job->response[len] = 0;
    job->response_len = len;
    job->volume = volume;
    strncpy(job->id, id, sizeof(job->id) - 1);
    job->id[sizeof(job->id) - 1] = 0;
    s_tail = (s_tail + 1) % MCP_QUEUE_DEPTH;
    s_count++;
    OS_EXIT_CRITICAL();
}

static void mcp_enqueue_result(const char *id, const char *result, int volume)
{
    char response[MCP_RESPONSE_MAX];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
    if (n > 0 && n < (int)sizeof(response)) {
        mcp_enqueue(response, (unsigned int)n, volume, id);
    }
}

static void mcp_enqueue_error(const char *id, int code, const char *message)
{
    char response[MCP_RESPONSE_MAX];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                     id, code, message);
    if (n > 0 && n < (int)sizeof(response)) {
        mcp_enqueue(response, (unsigned int)n, -1, id);
    }
}

void tuya_mcp_reset(void)
{
    OS_ENTER_CRITICAL();
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_drops = 0;
    OS_EXIT_CRITICAL();
}

void tuya_mcp_on_command(const void *data, unsigned int len)
{
    char request[512];
    char id[MCP_ID_MAX];
    char method[40];
    char tool_name[64];
    int volume;

    if (!data || !len) return;
    if (len >= sizeof(request)) {
        mcp_enqueue_error("1", -32600, "request too large");
        return;
    }
    memcpy(request, data, len);
    request[len] = 0;
    mcp_copy_id(request, id, sizeof(id));
    if (mcp_copy_string(request, "\"method\"", method, sizeof(method)) != 0) {
        mcp_enqueue_error(id, -32600, "invalid request");
    } else if (!strcmp(method, "initialize")) {
        mcp_enqueue_result(id,
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{\"name\":\"ac79\",\"version\":\"1.0\"}}", -1);
    } else if (!strcmp(method, "tools/list")) {
        mcp_enqueue_result(id,
            "{\"tools\":[{\"name\":\"" MCP_VOLUME_TOOL "\","
            "\"description\":\"Set the AC79 TTS speaker volume from 0 to 100 percent.\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},\"required\":[\"volume\"]}}]}", -1);
    } else if (!strcmp(method, "tools/call")) {
        if (mcp_copy_string(request, "\"name\"", tool_name, sizeof(tool_name)) != 0 ||
            strcmp(tool_name, MCP_VOLUME_TOOL)) {
            mcp_enqueue_error(id, -32601, "tool not found");
        } else if (mcp_get_volume(request, &volume) != 0) {
            mcp_enqueue_error(id, -32602, "volume must be an integer from 0 to 100");
        } else {
            /* Defer the actual board operation to the session task. */
            mcp_enqueue_result(id, "{\"content\":[{\"type\":\"text\",\"text\":\"Volume change queued\"}]}", volume);
        }
    } else {
        mcp_enqueue_error(id, -32601, "method not found");
    }
}

int tuya_mcp_pump(tai_ctx_t *ctx)
{
    mcp_job_t job;
    unsigned int drops;
    int rc;

    OS_ENTER_CRITICAL();
    if (!s_count) {
        OS_EXIT_CRITICAL();
        return 0;
    }
    memcpy(&job, &s_jobs[s_head], sizeof(job));
    s_head = (s_head + 1) % MCP_QUEUE_DEPTH;
    s_count--;
    drops = s_drops;
    s_drops = 0;
    OS_EXIT_CRITICAL();

    if (job.volume >= 0) {
        if (_device_set_play_volume(job.volume) != 0) {
            mcp_enqueue_error(job.id, -32603, "could not queue volume change");
            return -1;
        }
        printf("[TUYA-MCP] %s=%d queued\r\n", MCP_VOLUME_TOOL, job.volume);
    }
    rc = tai_send_mcp_response(ctx, job.response);
    printf("[TUYA-MCP] resp(%u B) rc=%d%s\r\n", job.response_len, rc,
           drops ? " (oldest pending request dropped)" : "");
    return rc == TAI_OK ? 1 : -1;
}
