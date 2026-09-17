#ifndef TUYA_MCP_H
#define TUYA_MCP_H

#include "tuya_ai.h"

/*
 * Device MCP bridge.
 *
 * The transport callback only parses and enqueues work.  tuya_mcp_pump() is
 * called by the already-running Agentic session task and never waits.
 */
void tuya_mcp_reset(void);
void tuya_mcp_on_command(const void *data, unsigned int len);

/* 1: one response sent, 0: no pending response, -1: send failed. */
int tuya_mcp_pump(tai_ctx_t *ctx);

#endif
