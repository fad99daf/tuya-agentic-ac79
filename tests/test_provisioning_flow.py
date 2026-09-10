#!/usr/bin/env python3
"""Static integration guards for the embedded Tuya provisioning flow.

The AC79 target build is Windows-toolchain-only. These checks run on any host and
lock the ordering/scope invariants that caused MT-88, while the target build and
device tests remain the authoritative runtime verification.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
WIFI = ROOT / "overlay/apps/wifi_story_machine/wifi_app_task.c"
MUSIC = ROOT / "overlay/apps/wifi_story_machine/app_music.c"
BLE_PORT = ROOT / "overlay/apps/common/LLM/tuya_agentic/le_net_cfg_tuya.c"
BLE_PROTO = (
    ROOT
    / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/tuya-ble/src/tuya_ble_prov.c"
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ProvisioningFlowGuards(unittest.TestCase):
    def test_activation_waits_for_this_network_generation(self) -> None:
        source = read(DEMO)
        first_time = source.index("/* ---- 首次:BLE 配网 ---- */")
        snapshot = source.index(
            "network_generation = wifi_get_tuya_network_ready_generation()", first_time
        )
        connect = source.index("wifi_enter_sta_mode(s_main_creds.ssid", snapshot)
        wait = source.index("tuya_wait_for_network_ready(network_generation)", connect)
        activate = source.index("iot_client_init_on_boarding_with_token", wait)

        self.assertLess(snapshot, connect)
        self.assertLess(connect, wait)
        self.assertLess(wait, activate)
        self.assertNotIn("msleep(1500)", source[connect:activate])

    def test_generation_advances_after_network_prerequisites(self) -> None:
        source = read(WIFI)
        dhcp = source.index("case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:")
        disconnect = source.index("case WIFI_EVENT_STA_DISCONNECT:", dhcp)
        block = source[dhcp:disconnect]

        mac_assignment = block.index("server_assign_macaddr(wifi_return_sta_mode)")
        ready_event = block.index("net_event_notify(NET_EVENT_FROM_USER, &net)")
        generation = block.index("s_tuya_network_ready_generation++")
        self.assertLess(mac_assignment, ready_event)
        self.assertLess(ready_event, generation)

    def test_only_success_prompt_is_suppressed_until_activation(self) -> None:
        source = read(MUSIC)
        connected = source.index("case NET_EVENT_CONNECTED:")
        disconnected = source.index("case NET_EVENT_DISCONNECTED:", connected)
        block = source[connected:disconnected]

        self.assertIn("tuya_agentic_provisioning_active()", block)
        self.assertIn("ble_cfg_net_result_notify(event->event)", block)
        self.assertIn("dev_profile_init()", block)
        self.assertIn("app_music_event_net_connected()", block)
        self.assertIn("__this->net_connected = 1", block)

    def test_all_new_failure_paths_reset_to_ble_provisioning(self) -> None:
        source = read(DEMO)
        for stage in (
            'tuya_provisioning_fail_and_reset("ble", NULL)',
            'tuya_provisioning_fail_and_reset("network-ready", NULL)',
            'tuya_provisioning_fail_and_reset("activation", NULL)',
            'tuya_provisioning_fail_and_reset("activation-result", iot)',
            'tuya_provisioning_fail_and_reset("credential-persist", iot)',
        ):
            self.assertIn(stage, source)

        helper = source[source.index("static void tuya_provisioning_fail_and_reset") :]
        self.assertLess(
            helper.index("app_music_play_tuya_netcfg_result(0)"),
            helper.index("tuya_clear_provision_and_reset()"),
        )

    def test_success_prompt_follows_activation_persistence_and_app_hold(self) -> None:
        source = read(DEMO)
        activate = source.index("iot_client_init_on_boarding_with_token")
        persist = source.index("tuya_save_required_provision_data(iot)", activate)
        hold = source.index("msleep(3000)", persist)
        prompt = source.index("app_music_play_tuya_netcfg_result(1)", hold)
        clear_guard = source.index("s_tuya_provisioning_active = 0", prompt)

        self.assertLess(activate, persist)
        self.assertLess(persist, hold)
        self.assertLess(hold, prompt)
        self.assertLess(prompt, clear_guard)

    def test_provisioning_secrets_are_not_logged_in_plaintext(self) -> None:
        combined = "\n".join((read(DEMO), read(BLE_PORT), read(BLE_PROTO)))
        for forbidden in (
            "WiFi JSON: %s",
            "Password: %s",
            "Token: %s",
            "prov done: ssid=%s token=%s",
            "BLE done: ssid=%s token=%s",
        ):
            self.assertNotIn(forbidden, combined)

        proto = read(BLE_PROTO)
        wifi_handler = proto[
            proto.index("static void handle_wifi_config") : proto.index(
                "static void tuya_ble_recv"
            )
        ]
        self.assertNotIn("TUYA_BLE_HAL_HEXDUMP", wifi_handler)


if __name__ == "__main__":
    unittest.main()
