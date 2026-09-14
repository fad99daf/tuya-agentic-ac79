"""Static integration guards for the AC79 Tuya provisioning state machine."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BLE = ROOT / "overlay/apps/common/LLM/tuya_agentic/le_net_cfg_tuya.c"
BLE_PROV = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/tuya-ble/src/tuya_ble_prov.c"
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"
WIFI = ROOT / "overlay/apps/wifi_story_machine/wifi_app_task.c"
MUSIC = ROOT / "overlay/apps/wifi_story_machine/app_music.c"
IOT_HEADER = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/include/iot_client.h"
IOT_CLIENT = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client.c"
IOT_MESSAGE = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client_message.c"


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
        self.assertIn("tuya_ble_request_disconnect", stop)
        self.assertIn("while (s_ble_connected", stop)
        self.assertNotIn("ble_op_disconnect", stop)
        self.assertIn("TUYA_BLE_STOP_TIMEOUT_MS", stop)

    def test_official_ble_state_callback_is_the_connection_source_of_truth(self) -> None:
        source = read(BLE)
        callback = source[source.index("static void tuya_ble_state_cb"):source.index("static void tuya_pkt_handler")]
        profile = source[source.index("static int tuya_ble_profile_init"):source.index("int tuya_ble_netcfg_start")]
        self.assertIn("case BLE_ST_CONNECT:", callback)
        self.assertIn("case BLE_ST_DISCONN:", callback)
        self.assertIn("s_ble_connected = 0", callback)
        self.assertIn("regist_state_cbk(NULL, tuya_ble_state_cb)", profile)

    def test_disconnect_is_official_and_idempotent(self) -> None:
        source = read(BLE)
        request = source[source.index("static int tuya_ble_request_disconnect"):source.index("/* ---------------- notify")]
        self.assertIn("if (!s_ble_connected || s_disconnect_pending)", request)
        self.assertIn("s_disconnect_pending = 1", request)
        self.assertIn("s_ble_ops->disconnect(NULL)", request)
        self.assertNotIn("ble_op_disconnect", source)

    def test_unexpected_disconnect_restores_tuya_advertising_outside_callback(self) -> None:
        source = read(BLE)
        callback = source[source.index("static void tuya_ble_state_cb"):source.index("static void tuya_pkt_handler")]
        worker_start = source.index("static void tuya_ble_prov_worker_task")
        worker = source[worker_start:source.index("\nvoid tuya_ble_prov_worker(", worker_start)]
        self.assertIn("!s_stop_requested && !s_prov_done", callback)
        self.assertIn("s_adv_restart_pending = 1", callback)
        self.assertNotIn("msleep(", callback)
        self.assertIn("tuya_make_adv();", worker)
        self.assertIn("tuya_ble_adv_enable_with_retry(1)", worker)

    def test_provisioning_wait_is_bounded_and_failure_resets_to_advertising(self) -> None:
        ble = read(BLE)
        demo = read(DEMO)
        self.assertIn("TUYA_BLE_PROVISION_TIMEOUT_TICKS", ble)
        self.assertNotIn("os_sem_pend(&s_prov_sem, 0)", ble)
        self.assertIn("tuya_provisioning_fail_and_reset", demo)

    def test_bt_ready_event_replaces_fixed_boot_delay_with_timeout_fallback(self) -> None:
        source = read(DEMO)
        handler = source.index("static void tuya_agentic_bt_event_handler")
        main = source.index("void tuya_agentic_main(void *arg)")
        flow_start = source.index('printf("===== tuya_agentic_main start', main)
        init = source.index("static int tuya_agentic_main_init(void)")
        self.assertIn("BT_STATUS_INIT_OK", source[handler:main])
        self.assertIn("os_sem_post(&s_bt_ready_sem)", source[handler:main])
        self.assertIn("os_sem_pend(&s_bt_ready_sem, TUYA_BT_READY_TIMEOUT_TICKS)", source[main:flow_start])
        self.assertNotIn("os_time_dly(400)", source[main:flow_start])
        self.assertIn("BT init %s; starting agentic flow", source[main:flow_start])
        self.assertIn("register_sys_event_handler(SYS_BT_EVENT, BT_EVENT_FROM_CON", source[init:])
        self.assertLess(source.index("os_sem_create(&s_bt_ready_sem, 0)", init),
                        source.index("register_sys_event_handler(SYS_BT_EVENT", init))

    def test_cloud_reset_clears_credentials_then_reenters_ble_without_reboot(self) -> None:
        source = read(DEMO)
        reset = source[source.index("static int tuya_clear_provision_credentials(void)"):]
        cloud_reset = source[source.index("if (s_cloud_reset_pending) {", source.index("static void tuya_ai_run")):]
        self.assertIn("syscfg_write(VM_TUYA_DEVID_IDX", reset)
        self.assertIn("syscfg_write(VM_TUYA_SECRET_IDX", reset)
        self.assertIn("syscfg_write(VM_TUYA_LOCALKEY_IDX", reset)
        self.assertIn("wifi_store_mode_info(SMP_CFG_MODE", reset)
        self.assertIn("syscfg_read(VM_TUYA_DEVID_IDX", reset)
        self.assertIn("syscfg_read(VM_TUYA_SECRET_IDX", reset)
        self.assertIn("syscfg_read(VM_TUYA_LOCALKEY_IDX", reset)
        self.assertIn("wifi_and_network_off()", cloud_reset)
        self.assertIn("s_cloud_reset_ready = 1", cloud_reset)
        self.assertIn("goto start_ble_provisioning", source)
        self.assertNotIn("tuya_clear_provision_and_reset();", cloud_reset)

    def test_ble_receive_accepts_consistent_mobile_padding_only(self) -> None:
        source = read(BLE_PROV)
        decrypt = source[source.index("static int aes_cbc_decrypt"):
                         source.index("static void ble_id_compress")]
        receive = source[source.index("uint16_t data_len ="):
                         source.index("uint16_t recv_crc =", source.index("uint16_t data_len ="))]
        self.assertIn("*out_len = in_len", decrypt)
        self.assertNotIn("*out_len = in_len - pad", decrypt)
        self.assertIn("frame_expected_len", receive)
        self.assertIn("frame_expected_len > frame_len", receive)
        self.assertIn("frame_len > frame_expected_len", receive)
        self.assertIn("is_pkcs7_padding", receive)
        self.assertIn("is_zero_padding", receive)
        self.assertIn("frame[frame_expected_len + i] != 0", receive)
        self.assertIn("frame[frame_expected_len + i] != pad_len", receive)
        self.assertIn("!is_pkcs7_padding && !is_zero_padding", receive)


class CloudRemovalTests(unittest.TestCase):
    def test_reset_callback_is_carried_through_every_client_creation_path(self) -> None:
        header = read(IOT_HEADER)
        client = read(IOT_CLIENT)
        self.assertIn("iot_reset_callback_t reset_callback", header)
        self.assertIn("void *reset_user_data", header)
        self.assertEqual(client.count("reset_callback = config->reset_callback"), 3)
        self.assertEqual(client.count("reset_user_data = config->reset_user_data"), 3)

    def test_protocol_11_removal_commands_are_consumed_before_dp_dispatch(self) -> None:
        source = read(IOT_MESSAGE)
        reset = source.index("iot_client_message_handle_reset(client, decrypted, decrypted_len)")
        dp_dispatch = source.index("iot_dp_dispatch_downlink", reset)
        self.assertLess(reset, dp_dispatch)
        self.assertIn("cJSON_ParseWithLength", source)
        self.assertIn("jproto->valueint != IOT_PROTO_GW_RESET", source)
        self.assertIn("strcmp(jgw->valuestring, client->devid)", source)
        self.assertIn('"reset_factory"', source)
        self.assertIn("IOT_RESET_REMOTE_FACTORY", source)
        header = read(IOT_HEADER)
        self.assertIn("IOT_RESET_REMOTE_UNBIND = 0", header)
        self.assertIn("IOT_RESET_REMOTE_FACTORY", header)
        self.assertIn("iot_reset_type_t type = IOT_RESET_REMOTE_UNBIND", source)

    def test_application_defers_erase_until_mqtt_process_owner_exits(self) -> None:
        source = read(DEMO)
        callback = source[source.index("static void on_cloud_reset"):
                          source.index("static void tuya_mqtt_keepalive_task")]
        reset = source[source.index("if (s_cloud_reset_pending) {", source.index("static void tuya_ai_run")):]
        self.assertIn("s_cloud_reset_pending = 1", callback)
        self.assertIn("g_exit = 1", callback)
        self.assertIn("IOT_RESET_REMOTE_UNBIND", callback)
        self.assertIn("g_mqtt_ka_run = 0", reset)
        self.assertLess(reset.index("g_mqtt_ka_run = 0"), reset.index("iot_client_deinit(iot)"))
        self.assertLess(reset.index("s_mqtt_ka_exited"), reset.index("iot_client_deinit(iot)"))
        self.assertIn("syscfg_write(VM_TUYA_SCHEMAID_IDX", source)
        self.assertIn("syscfg_write(VM_TUYA_SCHEMA_IDX", source)


if __name__ == "__main__":
    unittest.main()
