import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
WIFI = ROOT / "overlay/apps/wifi_story_machine/wifi_app_task.c"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ReconnectFlowTests(unittest.TestCase):
    def test_dhcp_marks_network_ready_before_notifying_apps(self) -> None:
        source = read(WIFI)
        dhcp = source.index("case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:")
        ready = source.index("s_tuya_network_ready = 1;", dhcp)
        generation = source.index("s_tuya_network_ready_generation++;", dhcp)
        event = source.index("net_event_notify(NET_EVENT_FROM_USER, &net);", dhcp)
        self.assertLess(ready, generation)
        self.assertLess(generation, event)

    def test_link_failures_revoke_network_ready(self) -> None:
        source = read(WIFI)
        for event in (
            "WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID:",
            "WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL:",
            "WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT:",
            "WIFI_EVENT_STA_DISCONNECT:",
        ):
            start = source.index(event)
            self.assertIn("s_tuya_network_ready = 0;", source[start : start + 460])

    def test_provisioned_boot_waits_before_cloud_initialization(self) -> None:
        source = read(DEMO)
        provisioned = source.index("if (have) {")
        reconnect = source.index("wifi_enter_sta_mode(ssid, pwd);", provisioned)
        wait = source.index("tuya_wait_for_network_available(network_generation);", reconnect)
        sync = source.index("tuya_sync_wifi_to_jl(ssid, pwd);", wait)
        init = source.index("iot_client_t *iot = iot_client_init(&cfg);", sync)
        self.assertLess(reconnect, wait)
        self.assertLess(wait, sync)
        self.assertLess(sync, init)

    def test_only_invalid_persisted_data_enters_clear_provision_path(self) -> None:
        source = read(DEMO)
        invalid = source.index("invalid stored provisioning data")
        clear = source.index("tuya_clear_provision_and_reset();", invalid)
        wait = source.index("tuya_wait_for_network_available(network_generation);", clear)
        self.assertLess(invalid, clear)
        self.assertLess(clear, wait)
        self.assertIn("Association failures and DHCP timeouts never enter this path.", source)

    def test_fresh_provisioning_waits_for_a_new_dhcp_generation(self) -> None:
        source = read(DEMO)
        fresh_wifi = source.index("wifi_enter_sta_mode(s_main_creds.ssid, s_main_creds.password);")
        wait = source.index("tuya_wait_for_network_ready(network_generation, 0)", fresh_wifi)
        activation = source.index("iot_client_init_on_boarding_with_token", wait)
        self.assertLess(fresh_wifi, wait)
        self.assertLess(wait, activation)

    def test_mqtt_and_ai_retries_are_gated_by_network_readiness(self) -> None:
        source = read(DEMO)
        mqtt = source.index("static void tuya_mqtt_keepalive_task")
        mqtt_offline = source.index("if (!wifi_tuya_network_is_ready())", mqtt)
        mqtt_process = source.index("iot_client_process(iot, 0);", mqtt)
        self.assertLess(mqtt_offline, mqtt_process)
        self.assertIn(
            "g_mqtt_ka_run && wifi_tuya_network_is_ready() &&\n"
            "                   iot_client_message_connect(iot)",
            source[mqtt : mqtt_process + 2600],
        )

        ai = source.index("static void tuya_ai_run")
        ai_offline = source.index("if (!wifi_tuya_network_is_ready())", ai)
        session = source.index("tuya_ai_session(pal, iot, local_key);", ai)
        self.assertLess(ai_offline, session)


if __name__ == "__main__":
    unittest.main()
