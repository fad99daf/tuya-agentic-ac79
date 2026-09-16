"""Static regression checks for the AC79 Tuya BLE connection lifecycle."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BLE = ROOT / "overlay/apps/common/LLM/tuya_agentic/le_net_cfg_tuya.c"


class BleLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = BLE.read_text(encoding="utf-8")
        self.callback = self.source[
            self.source.index("static void tuya_ble_state_cb"):
            self.source.index("static void tuya_pkt_handler")
        ]
        self.profile = self.source[
            self.source.index("static int tuya_ble_profile_init"):
            self.source.index("int tuya_ble_netcfg_start")
        ]
        worker_start = self.source.index("static void tuya_ble_prov_worker_task")
        self.worker = self.source[
            worker_start:self.source.index("\nvoid tuya_ble_prov_worker(", worker_start)
        ]

    def test_official_server_state_callback_owns_connection_lifecycle(self) -> None:
        self.assertIn('#include "btstack/le/le_user.h"', self.source)
        self.assertIn('#include "third_party/common/ble_user.h"', self.source)
        self.assertIn("ble_get_server_operation_table(&ble_ops)", self.profile)
        self.assertIn("regist_state_cbk(NULL, tuya_ble_state_cb)", self.profile)
        self.assertIn("case BLE_ST_CONNECT:", self.callback)
        self.assertIn("case BLE_ST_DISCONN:", self.callback)
        self.assertIn("case BLE_ST_CONNECT_FAIL:", self.callback)
        self.assertIn("s_ble_connected = 0", self.callback)

    def test_disconnect_only_schedules_advertising_recovery(self) -> None:
        self.assertIn("!s_stop_requested && (!s_prov_done || s_bound_session_active)", self.callback)
        self.assertIn("s_adv_restart_pending = 1", self.callback)
        self.assertIn('os_taskq_post("tuya_prov_w", 0)', self.callback)
        self.assertNotIn("msleep(", self.callback)
        self.assertNotIn("tuya_make_adv();", self.callback)

    def test_worker_recovers_tuya_advertising_after_callback_returns(self) -> None:
        self.assertIn("if (s_adv_restart_pending)", self.worker)
        self.assertIn("msleep(TUYA_BLE_RESTART_DELAY_MS)", self.worker)
        self.assertIn("!s_stop_requested && (!s_prov_done || s_bound_session_active) && !s_ble_connected", self.worker)
        self.assertIn("tuya_make_adv();", self.worker)
        self.assertIn("tuya_ble_adv_enable_with_retry(1)", self.worker)

    def test_explicit_stop_cannot_reopen_advertising(self) -> None:
        stop = self.source[self.source.index("void tuya_ble_netcfg_stop(void)"):]
        self.assertIn("s_stop_requested = 1", stop)
        self.assertIn("s_adv_restart_pending = 0", stop)

    def test_stop_preserves_the_att_link_until_bound_session_is_ready(self) -> None:
        stop = self.source[self.source.index("void tuya_ble_netcfg_stop(void)"):]
        self.assertIn("ATT retained", stop)
        self.assertNotIn("->disconnect(", stop)

    def test_bound_session_is_started_with_activated_credentials(self) -> None:
        self.assertIn("int tuya_ble_bound_session_start", self.source)
        self.assertIn("iot->local_key", self.source)
        self.assertIn("iot->secret_key", self.source)
        self.assertIn("tuya_ble_prov_enable_bound_session", self.source)
        self.assertIn("tuya_ble_dp_to_app", self.source)


if __name__ == "__main__":
    unittest.main()
