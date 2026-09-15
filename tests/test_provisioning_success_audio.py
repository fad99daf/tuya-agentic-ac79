"""Event-sequence regression tests for Tuya first-provisioning prompts.

The target firmware is AC79-only, so the event contract is exercised here with a
small deterministic harness and tied back to the C call order below.  This keeps
the acceptance cases executable without pretending a host Python process can
run the board's Wi-Fi, MQTT, or audio drivers.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
MUSIC = ROOT / "overlay/apps/wifi_story_machine/app_music.c"


class ProvisioningAudioHarness:
    """Minimal observable contract of the provisioning/audio event boundary."""

    def __init__(self) -> None:
        self.provisioning_active = False
        self.activation_ok = False
        self.credentials_persisted = False
        self.mqtt_window_complete = False
        self.prompts: list[str] = []

    def begin_provisioning(self) -> None:
        self.provisioning_active = True

    def net_connected(self) -> None:
        if not self.provisioning_active:
            self.prompts.append("NetCfgSucc.mp3")

    def net_disconnected(self, was_connected: bool = True) -> None:
        if was_connected and not self.provisioning_active:
            self.prompts.append("NetDisc.mp3")

    def activation_succeeded(self) -> None:
        self.activation_ok = True

    def credentials_saved(self) -> None:
        self.credentials_persisted = True

    def mqtt_sync_completed(self) -> None:
        self.mqtt_window_complete = True

    def finish_success(self) -> None:
        if not (
            self.activation_ok
            and self.credentials_persisted
            and self.mqtt_window_complete
        ):
            return
        self.provisioning_active = False
        self.prompts.append("NetCfgSucc.mp3")


class ProvisioningSuccessAudioEventTests(unittest.TestCase):
    def test_dhcp_then_activation_failure_never_announces_success(self) -> None:
        flow = ProvisioningAudioHarness()
        flow.begin_provisioning()
        flow.net_connected()  # DHCP / NET_EVENT_CONNECTED
        # Activation fails, so persistence, MQTT completion, and finish do not occur.
        flow.finish_success()

        self.assertTrue(flow.provisioning_active)
        self.assertEqual(flow.prompts, [])

    def test_success_is_emitted_once_only_after_full_activation_sequence(self) -> None:
        flow = ProvisioningAudioHarness()
        flow.begin_provisioning()
        flow.net_connected()
        flow.activation_succeeded()
        flow.finish_success()
        self.assertEqual(flow.prompts, [])

        flow.credentials_saved()
        flow.finish_success()
        self.assertEqual(flow.prompts, [])

        flow.mqtt_sync_completed()
        flow.finish_success()
        self.assertFalse(flow.provisioning_active)
        self.assertEqual(flow.prompts, ["NetCfgSucc.mp3"])

    def test_expected_sta_handoff_is_silent_but_normal_disconnect_is_unchanged(self) -> None:
        flow = ProvisioningAudioHarness()
        flow.begin_provisioning()
        flow.net_disconnected()  # old AP disconnect while moving to BLE-provided SSID
        self.assertEqual(flow.prompts, [])

        flow.provisioning_active = False
        flow.net_disconnected()
        self.assertEqual(flow.prompts, ["NetDisc.mp3"])


class ProvisioningSuccessAudioSourceGuards(unittest.TestCase):
    def test_firmware_call_order_matches_event_contract(self) -> None:
        source = DEMO.read_text(encoding="utf-8")
        first_time = source.index("/* ---- 首次:BLE 配网 ---- */")
        activated = source.index("iot_client_init_on_boarding_with_token", first_time)
        persisted = source.index("tuya_save_required_provision_data(iot)", activated)
        mqtt_hold = source.index("msleep(3000)", persisted)
        clear_active = source.index("s_tuya_provisioning_active = 0", mqtt_hold)
        announce = source.index("app_music_play_netcfg_success()", clear_active)

        self.assertLess(activated, persisted)
        self.assertLess(persisted, mqtt_hold)
        self.assertLess(mqtt_hold, clear_active)
        self.assertLess(clear_active, announce)

    def test_dhcp_and_disconnect_prompts_are_both_guarded_while_active(self) -> None:
        source = MUSIC.read_text(encoding="utf-8")
        connected = source.index("case NET_EVENT_CONNECTED:")
        disconnected = source.index("case NET_EVENT_DISCONNECTED:", connected)
        timeout = source.index("case NET_EVENT_SMP_CFG_TIMEOUT:", disconnected)

        connected_block = source[connected:disconnected]
        disconnected_block = source[disconnected:timeout]
        self.assertIn("!tuya_agentic_provisioning_active()", connected_block)
        self.assertIn("!tuya_agentic_provisioning_active()", disconnected_block)

    def test_required_credentials_are_written_and_read_back_without_logging_values(self) -> None:
        source = DEMO.read_text(encoding="utf-8")
        helper_start = source.index("static int tuya_save_required_provision_data")
        helper_end = source.index("static void tuya_prov_prompt_task", helper_start)
        helper = source[helper_start:helper_end]

        for key in (
            "VM_TUYA_DEVID_IDX",
            "VM_TUYA_SECRET_IDX",
            "VM_TUYA_LOCALKEY_IDX",
            "VM_TUYA_SSID_IDX",
            "VM_TUYA_PWD_IDX",
            "VM_TUYA_REGION_IDX",
        ):
            self.assertIn(key, helper)
        self.assertIn("syscfg_read(index, readback, len)", source)
        self.assertIn("memcmp(readback, data, len)", source)
        self.assertNotIn("BLE done: ssid=%s token=%s", source)


if __name__ == "__main__":
    unittest.main()
