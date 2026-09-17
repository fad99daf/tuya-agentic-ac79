"""Static regression checks for the non-blocking AC79 volume MCP tool."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
MCP = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_mcp.c"
MCP_HEADER = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_mcp.h"
AUDIO = ROOT / "overlay/apps/common/LLM/audio/audio_input.c"
MAKEFILE = ROOT / "overlay/apps/wifi_story_machine/board/wl82/Makefile"


class McpVolumeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.demo = DEMO.read_text(encoding="utf-8")
        self.mcp = MCP.read_text(encoding="utf-8")
        self.header = MCP_HEADER.read_text(encoding="utf-8")
        self.audio = AUDIO.read_text(encoding="utf-8")
        self.makefile = MAKEFILE.read_text(encoding="utf-8")

    def test_mcp_module_is_built_and_advertised(self) -> None:
        self.assertIn("tuya_mcp.c", self.makefile)
        self.assertIn('static const char SA[] = "{\\"deviceMcp\\":{\\"supportCustomMCP\\":true}}";', self.demo)
        self.assertIn('static const char SESSION_ATTRS[] = "{\\"deviceMcp\\":{\\"supportCustomMCP\\":false}}";', self.demo)
        self.assertIn('MCP_VOLUME_TOOL       "self.audio_speaker.set_volume"', self.mcp)

    def test_standard_protocol_discovers_and_calls_volume_tool(self) -> None:
        self.assertIn('!strcmp(method, "initialize")', self.mcp)
        self.assertIn('!strcmp(method, "tools/list")', self.mcp)
        self.assertIn('!strcmp(method, "tools/call")', self.mcp)
        self.assertIn('\\"inputSchema\\"', self.mcp)
        self.assertIn('\\"minimum\\":0,\\"maximum\\":100', self.mcp)
        self.assertNotIn('!strcmp(method, "set_volume")', self.mcp)

    def test_worker_only_queues_and_session_task_executes(self) -> None:
        event = self.demo[self.demo.index("} else if (msg->event_type == TAI_EVT_MCP_CMD)"):]
        event = event[:event.index("\n    }\n}")]
        self.assertIn("tuya_mcp_on_command(msg->data, msg->len)", event)
        self.assertNotIn("tai_send_mcp_response", event)
        self.assertIn("tuya_mcp_pump(ctx)", self.demo)

        handler = self.mcp[self.mcp.index("void tuya_mcp_on_command"):self.mcp.index("int tuya_mcp_pump")]
        pump = self.mcp[self.mcp.index("int tuya_mcp_pump"):]
        self.assertIn("mcp_enqueue", handler)
        self.assertNotIn("_device_set_play_volume", handler)
        self.assertIn("_device_set_play_volume(job.volume)", pump)
        self.assertIn("tai_send_mcp_response(ctx, job.response)", pump)

    def test_mcp_creates_no_keep_alive_task_or_wait_loop(self) -> None:
        self.assertNotIn("thread_fork", self.mcp)
        self.assertNotIn("os_q_pend", self.mcp)
        self.assertNotIn("os_time_dly", self.mcp)
        self.assertNotIn("msleep", self.mcp)
        self.assertIn("called by the already-running Agentic session task and never waits.", self.header)

    def test_text_fallback_is_isolated_from_tcp_mcp_events(self) -> None:
        """TCP emits MCP responses as events, never through the STM TEXT fallback."""
        text_fallback_guard = "#if TUYA_TRANSPORT_STM_ENABLE && TUYA_STM_MCP_VIA_TEXT"
        self.assertEqual(self.demo.count(text_fallback_guard), 4)
        self.assertNotIn("\n#if TUYA_STM_MCP_VIA_TEXT\n", self.demo)

        audio = self.demo[self.demo.index("static void on_audio"):self.demo.index("/* ------------------------------------------------------------------------- */\n/* MCP response dispatch")]
        pump = self.demo[self.demo.index("static void mcp_resp_pump"):self.demo.index("#define TUYA_OPUS_FRAME_LEN")]
        self.assertIn(text_fallback_guard, audio)
        self.assertIn(text_fallback_guard, pump)

    def test_volume_validation_accepts_integer_json_doubles_only(self) -> None:
        getter = self.mcp[self.mcp.index("static int mcp_get_volume"):self.mcp.index("static void mcp_enqueue")]
        self.assertIn("if (v > 100) return -1", getter)
        self.assertIn("if (*p == '.')", getter)
        self.assertIn("if (*p++ != '0') return -1", getter)
        self.assertIn("if (p == fraction) return -1", getter)

    def test_audio_task_remains_the_only_audio_server_writer(self) -> None:
        task = self.audio[self.audio.index("case MSG_SET_NET_AUDIO_VOLUME"):self.audio.index("case MSG_START_AUDIO_RECORDER")]
        self.assertIn("AUDIO_DEC_SET_VOLUME", task)
        self.assertIn("g_audio_hdl.is_audio_play_open", task)
        self.assertIn("volume < 0 || volume > 100", task)
        self.assertNotIn("AUDIO_DEC_SET_VOLUME", self.mcp)


if __name__ == "__main__":
    unittest.main()
