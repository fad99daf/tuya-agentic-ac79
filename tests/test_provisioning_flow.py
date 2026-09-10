"""Static integration guards for the AC79 Tuya provisioning state machine."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BLE = ROOT / "overlay/apps/common/LLM/tuya_agentic/le_net_cfg_tuya.c"
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
WIFI = ROOT / "overlay/apps/wifi_story_machine/wifi_app_task.c"
MUSIC = ROOT / "overlay/apps/wifi_story_machine/app_music.c"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ProvisioningFlowTests(unittest.TestCase):
    def test_network_ready_is_published_before_blocking_app_event(self) -> None:
        source = read(WIFI)
        dhcp = source.index("case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:")
        event = source.index("net_event_notify(NET_EVENT_FROM_USER, &net);", dhcp)
        generation = source.index("s_tuya_network_ready_generation++", dhcp)
        self.assertLess(generation, event)

    def test_activation_waits_for_this_connection_network_ready_generation(self) -> None:
        source = read(DEMO)
        snapshot = source.index("wifi_get_tuya_network_ready_generation()")
        connect = source.index("wifi_enter_sta_mode(s_main_creds.ssid", snapshot)
        wait = source.index("tuya_wait_for_network_ready(network_generation)", connect)
        activate = source.index("iot_client_init_on_boarding_with_token", wait)
        self.assertLess(snapshot, connect)
        self.assertLess(connect, wait)
        self.assertLess(wait, activate)

    def test_provisioning_success_prompt_waits_for_cloud_activation(self) -> None:
        source = read(MUSIC)
        connected = source.index("case NET_EVENT_CONNECTED:")
        disconnected = source.index("case NET_EVENT_DISCONNECTED:", connected)
        block = source[connected:disconnected]
        self.assertIn("tuya_agentic_provisioning_active()", block)
        self.assertIn("app_music_play_voice_prompt(\"NetCfgSucc.mp3\"", block)

    def test_ble_stop_never_calls_unpaired_official_module_exit(self) -> None:
        source = read(BLE)
        stop = source[source.index("int tuya_ble_netcfg_stop(void)"):]
        self.assertNotIn("bt_ble_exit();", stop)
        self.assertIn("tuya_ble_disconnect_with_retry", stop)
        self.assertIn("TUYA_BLE_STOP_TIMEOUT_MS", stop)

    def test_disconnect_during_stop_cannot_restart_advertising(self) -> None:
        source = read(BLE)
        handler = source[source.index("static void tuya_pkt_handler"):source.index("/* ---------------- 初始化 + 启动")]
        disconnected = handler[handler.index("case HCI_EVENT_DISCONNECTION_COMPLETE:"):]
        self.assertIn("!s_stop_requested && !s_prov_done", disconnected)

    def test_provisioning_wait_is_bounded_and_failure_resets_to_advertising(self) -> None:
        ble = read(BLE)
        demo = read(DEMO)
        self.assertIn("TUYA_BLE_PROVISION_TIMEOUT_TICKS", ble)
        self.assertNotIn("os_sem_pend(&s_prov_sem, 0)", ble)
        self.assertIn("tuya_provisioning_fail_and_reset", demo)


if __name__ == "__main__":
    unittest.main()
