"""Regression checks for BLE-provisioned Wi-Fi authority on AC79."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
MUSIC = ROOT / "overlay/apps/wifi_story_machine/app_music.c"


class ProvisioningSsidAuthorityHarness:
    """Observable contract; host tests cannot execute AC79 Wi-Fi drivers."""

    def __init__(self, configured_ssid: str) -> None:
        self.configured_ssid = configured_ssid
        self.provisioning_active = True
        self.used_stored_wifi = False
        self.activation_started = False
        self.credentials_persisted = False
        self.reboot_to_ble = False

    def disconnected_and_request_connect(self) -> None:
        if not self.provisioning_active:
            self.used_stored_wifi = True

    def dhcp_ready(self, associated_ssid: str) -> None:
        if associated_ssid.encode("utf-8") != self.configured_ssid.encode("utf-8"):
            self.reboot_to_ble = True
            return
        self.activation_started = True
        self.credentials_persisted = True


class ProvisioningSsidAuthorityTests(unittest.TestCase):
    def test_22_byte_utf8_ssid_never_falls_back_to_a_remembered_ap(self) -> None:
        configured = "a b 唐 @# <《？ %++"
        self.assertEqual(len(configured.encode("utf-8")), 22)
        flow = ProvisioningSsidAuthorityHarness(configured)

        flow.disconnected_and_request_connect()
        flow.dhcp_ready("Tuya-Test")

        self.assertFalse(flow.used_stored_wifi)
        self.assertTrue(flow.reboot_to_ble)
        self.assertFalse(flow.activation_started)
        self.assertFalse(flow.credentials_persisted)

    def test_exact_utf8_byte_match_allows_activation(self) -> None:
        configured = "a b 唐 @# <《？ %++"
        flow = ProvisioningSsidAuthorityHarness(configured)
        flow.dhcp_ready(configured)

        self.assertFalse(flow.reboot_to_ble)
        self.assertTrue(flow.activation_started)
        self.assertTrue(flow.credentials_persisted)

    def test_source_blocks_stored_wifi_and_checks_ssid_before_persisting(self) -> None:
        demo = DEMO.read_text(encoding="utf-8")
        music = MUSIC.read_text(encoding="utf-8")
        fallback = music[
            music.index("case NET_EVENT_DISCONNECTED_AND_REQ_CONNECT:"):
            music.index("case NET_NTP_GET_TIME_SUCC:")
        ]
        provisioning = demo[demo.index("/* ---- 首次:BLE 配网 ---- */"):]

        self.assertIn("if (tuya_agentic_provisioning_active())", fallback)
        self.assertLess(
            fallback.index("tuya_agentic_provisioning_active()"),
            fallback.index("wifi_return_sta_mode()"),
        )
        self.assertIn("tuya_provisioning_ssid_matches_current_sta()", provisioning)
        self.assertLess(
            provisioning.index("tuya_provisioning_ssid_matches_current_sta()"),
            provisioning.index("tuya_sync_wifi_to_jl(s_main_creds.ssid"),
        )
        self.assertIn("tuya_restart_ble_provisioning_after_wifi_failure", provisioning)


if __name__ == "__main__":
    unittest.main()
