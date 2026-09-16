"""Static regression checks for the device-side MCP volume tool."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
AUDIO = ROOT / "overlay/apps/common/LLM/audio/audio_input.c"
HEADER = ROOT / "overlay/apps/common/LLM/audio/audio_input.h"


class McpVolumeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.demo = DEMO.read_text(encoding="utf-8")
        self.audio = AUDIO.read_text(encoding="utf-8")
        self.header = HEADER.read_text(encoding="utf-8")
        start = self.demo.index("static void mcp_handle_request")
        self.handler = self.demo[start:self.demo.index("static void mcp_resp_defer", start)]

    def test_custom_mcp_is_advertised_and_lists_the_volume_tool(self) -> None:
        self.assertIn('"{\\"deviceMcp\\":{\\"supportCustomMCP\\":true}}"', self.demo)
        self.assertNotIn('supportCustomMCP\\":false', self.demo)
        self.assertIn('!strcmp(method, "tools/list")', self.handler)
        self.assertIn('"\\"name\\":\\"set_volume\\"', self.handler)
        self.assertIn('\\"minimum\\":0,\\"maximum\\":100', self.handler)

    def test_tools_call_validates_and_queues_an_integer_volume(self) -> None:
        self.assertIn('!strcmp(method, "tools/call")', self.handler)
        self.assertIn('!strcmp(tool_name, "set_volume")', self.handler)
        self.assertIn("mcp_get_volume(json, &volume) != 0", self.handler)
        self.assertIn("_device_set_play_volume(volume)", self.handler)
        self.assertIn("-32602", self.handler)
        self.assertIn("-32603", self.handler)
        getter = self.demo[self.demo.index("static int mcp_get_volume"):
                           self.demo.index("static void mcp_defer_error")]
        self.assertIn("if (v > 100) return -1", getter)

    def test_response_keeps_json_rpc_id_and_is_sent_off_the_worker_thread(self) -> None:
        self.assertIn("mcp_copy_id(json, id, sizeof(id))", self.handler)
        self.assertIn('\\"id\\":%s', self.handler)
        event = self.demo[self.demo.index("} else if (msg->event_type == TAI_EVT_MCP_CMD)"):]
        event = event[:event.index("\n    }\n}")]
        self.assertIn("mcp_handle_request(pbuf)", event)
        self.assertNotIn("tai_send_mcp_response", event)
        pump = self.demo[self.demo.index("static void mcp_resp_pump"):
                         self.demo.index("#define TUYA_OPUS_FRAME_LEN")]
        self.assertIn("tai_send_mcp_response(ctx, buf)", pump)

    def test_audio_task_serializes_live_update_and_persists_for_next_tts(self) -> None:
        self.assertIn("int _device_set_play_volume(int volume);", self.header)
        self.assertIn("static volatile int g_audio_play_volume", self.audio)
        self.assertIn("req.dec.volume          = g_audio_play_volume", self.audio)
        self.assertIn("return _send_audio_msg(MSG_SET_NET_AUDIO_VOLUME", self.audio)
        task = self.audio[self.audio.index("case MSG_SET_NET_AUDIO_VOLUME"):
                          self.audio.index("case MSG_START_AUDIO_RECORDER")]
        self.assertIn("AUDIO_DEC_SET_VOLUME", task)
        self.assertIn("g_audio_hdl.is_audio_play_open", task)
        self.assertIn("volume < 0 || volume > 100", task)


if __name__ == "__main__":
    unittest.main()
