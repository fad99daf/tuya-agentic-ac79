from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/include/iot_client.h"
CLIENT = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client.c"
ATOP = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop.c"
ATOP_BASE = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop_base.c"
MESSAGE = ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client_message.c"
DEMO = ROOT / "overlay/apps/common/LLM/tuya_agentic/tuya_agentic_demo.c"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class CloudResetStateMachineTests(unittest.TestCase):
    def test_protocol_11_requires_a_nonempty_exact_gateway_identity(self) -> None:
        source = read(MESSAGE)
        handler = source[source.index("static bool iot_client_message_handle_cloud_remove"):
                         source.index("static void mqtt_message_handler")]
        self.assertIn("cJSON_ParseWithLength", handler)
        self.assertIn("IOT_PROTOCOL_CLOUD_REMOVE", handler)
        self.assertIn("cJSON_IsObject(data)", handler)
        self.assertIn("cJSON_IsString(gwid)", handler)
        self.assertIn("gwid->valuestring[0] == '\\0'", handler)
        self.assertIn("client->devid[0] == '\\0'", handler)
        self.assertIn("strcmp(gwid->valuestring, client->devid) != 0", handler)
        self.assertIn("IOT_RESET_REMOTE_FACTORY", handler)
        self.assertIn('"reset_factory"', handler)
        self.assertLess(
            source.index("iot_client_message_handle_cloud_remove(client, decrypted, decrypted_len)"),
            source.index("iot_dp_dispatch_downlink"),
        )

    def test_callback_api_is_available_on_every_initialization_path(self) -> None:
        header = read(HEADER)
        client = read(CLIENT)
        demo = read(DEMO)
        self.assertIn("typedef enum {\n    IOT_RESET_REMOTE_UNBIND = 0,", header)
        self.assertIn("iot_reset_callback_t reset_callback", header)
        self.assertIn("void *reset_user_data", header)
        self.assertEqual(client.count("reset_callback = config->reset_callback"), 3)
        self.assertEqual(client.count("reset_user_data = config->reset_user_data"), 3)
        self.assertEqual(demo.count("reset_callback = on_cloud_reset"), 4)

    def test_mqtt_callback_only_queues_and_deduplicates_the_first_request(self) -> None:
        source = read(DEMO)
        callback = source[source.index("static void on_cloud_reset"):
                          source.index("static void tuya_mqtt_keepalive_task")]
        self.assertIn("s_cloud_reset_type == TUYA_CLOUD_RESET_NONE", callback)
        self.assertIn("s_cloud_reset_type = (int)type", callback)
        self.assertIn("duplicate cloud reset ignored", callback)
        self.assertIn("g_exit = 1", callback)
        self.assertNotIn("syscfg_write", callback)
        self.assertNotIn("iot_client_deinit", callback)
        self.assertNotIn("cpu_reset", callback)

    def test_supervisor_marks_then_stops_mqtt_then_deinits_and_verifies(self) -> None:
        source = read(DEMO)
        supervisor = source[source.index("static int tuya_cloud_reset_supervise"):
                            source.index("static void tuya_ai_run")]
        self.assertLess(
            supervisor.index("tuya_cloud_reset_write_marker(tuya_cloud_reset_marker(type, 0))"),
            supervisor.index("g_mqtt_ka_run = 0"),
        )
        self.assertLess(supervisor.index("g_mqtt_ka_run = 0"),
                        supervisor.index("iot_client_deinit(iot)"))
        self.assertIn("TUYA_CLOUD_RESET_STOP_TICKS", supervisor)
        self.assertNotIn("thread_kill", supervisor)
        self.assertNotIn("while (!s_mqtt_ka_exited)", supervisor)
        self.assertLess(supervisor.index("iot_client_deinit(iot)"),
                        supervisor.index("tuya_cloud_reset_clear_and_verify(type)"))
        self.assertLess(supervisor.index("tuya_cloud_reset_clear_and_verify(type)"),
                        supervisor.rindex("tuya_cloud_reset_cpu_reboot()"))

    def test_clear_is_recoverable_verifies_the_triplet_and_keeps_type_policy(self) -> None:
        source = read(DEMO)
        clear = source[source.index("static int tuya_cloud_reset_clear_and_verify"):
                       source.index("static void tuya_cloud_reset_cpu_reboot")]
        self.assertIn("VM_TUYA_RESET_STATE_IDX 184", source)
        self.assertIn("TUYA_RESET_MARK_CLEARING_UNBIND", source)
        self.assertIn("TUYA_RESET_MARK_CLEARING_FACTORY", source)
        self.assertIn("syscfg_write(VM_TUYA_DEVID_IDX", clear)
        self.assertIn("syscfg_write(VM_TUYA_SECRET_IDX", clear)
        self.assertIn("syscfg_write(VM_TUYA_LOCALKEY_IDX", clear)
        self.assertIn("tuya_cloud_reset_credentials_cleared()", clear)
        self.assertIn("memcmp(devid, zero, sizeof(devid))", source)
        self.assertIn("memcmp(secret, zero, sizeof(secret))", source)
        self.assertIn("memcmp(localkey, zero, sizeof(localkey))", source)
        self.assertIn("tuya_cloud_reset_clear_unbind_state", source)
        self.assertIn("tuya_cloud_reset_clear_factory_state", source)
        factory = source[source.index("static void tuya_cloud_reset_clear_factory_state"):
                         source.index("static int tuya_cloud_reset_clear_and_verify")]
        self.assertIn("VM_TUYA_SSID_IDX", factory)
        self.assertIn("VM_TUYA_PWD_IDX", factory)
        self.assertIn("VM_TUYA_SCHEMAID_IDX", factory)
        self.assertIn("VM_TUYA_REGION_IDX", factory)
        self.assertIn("wifi_store_mode_info(SMP_CFG_MODE", factory)

    def test_boot_recovers_before_accepting_saved_identity_or_starting_iot(self) -> None:
        source = read(DEMO)
        main = source[source.index("void tuya_agentic_main(void *arg)"):]
        self.assertLess(main.index("tuya_cloud_reset_recover_if_needed()"),
                        main.index("int have = syscfg_read"))
        self.assertIn("secret[0] != 0", main)
        self.assertIn("localkey[0] != 0", main)
        self.assertIn("no devid, start BLE provisioning", main)
        self.assertNotIn("while (1) { ; }", main[main.index("void tuya_clear_provision_and_reset"):])

    def test_local_factory_reset_is_available_without_a_cloud_response(self) -> None:
        header = read(HEADER)
        client = read(CLIENT)
        atop = read(ATOP)
        demo = read(DEMO)
        http = read(ROOT / "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/http_client_interface.c")
        k6 = demo[demo.rindex("void tuya_clear_provision_and_reset"):
                  demo.index("static int tuya_agentic_main_init")]
        supervisor = demo[demo.index("static int tuya_local_factory_reset_supervise"):
                          demo.index("static void tuya_ai_run")]
        run = demo[demo.index("static void tuya_ai_run"):
                   demo.index("/* 配网等待期循环播报")]
        send_only = http[http.index("if (request->send_only)"):
                         http.index("// Send HTTP request and receive its response.")]

        self.assertIn("iot_client_factory_reset", header)
        self.assertIn("iot_client_factory_reset_notify", header)
        self.assertIn("device_reset_request_t request", client)
        self.assertIn("return atop_device_reset(client->pal, &request)", client)
        self.assertIn("return atop_device_reset_notify(client->pal, &request)", client)
        self.assertIn('ATOP_DEVICE_RESET "tuya.device.reset"', atop)
        self.assertIn('.version = "4.0"', atop)
        self.assertIn("atop_device_reset_notify", atop)
        self.assertIn("send_only ? ATOP_DEVICE_RESET_NOTIFY_TIMEOUT_MS : 0", atop)
        self.assertIn("bool success = atop_response.success", atop)
        self.assertIn("if (!success)", atop)
        self.assertIn("s_local_factory_reset_requested = 1", k6)
        self.assertIn("g_exit = 1", k6)
        self.assertNotIn("syscfg_write", k6)
        self.assertNotIn("cpu_reset", k6)
        self.assertIn("tuya_local_factory_reset_without_client()", k6)
        self.assertIn("wifi_tuya_network_is_ready()", supervisor)
        self.assertIn("iot_client_factory_reset_notify(iot)", supervisor)
        self.assertNotIn("iot_client_factory_reset(iot)", supervisor)
        self.assertNotIn("if (ret != OPRT_OK)", supervisor)
        self.assertLess(supervisor.index("iot_client_factory_reset_notify(iot)"),
                        supervisor.index("s_cloud_reset_type = IOT_RESET_REMOTE_FACTORY"))
        self.assertLess(supervisor.index("s_cloud_reset_type = IOT_RESET_REMOTE_FACTORY"),
                        supervisor.index("tuya_cloud_reset_supervise(iot)"))
        self.assertNotIn("waiting for cloud network", run)
        self.assertIn("HTTPClient_SendHttpHeaders", send_only)
        self.assertIn("HTTPClient_SendHttpData", send_only)
        self.assertNotIn("HTTPClient_Send(&transport", send_only)
        self.assertIn("static uint32_t http_client_get_timestamp_ms(void)", http)
        self.assertIn("return (uint32_t)timer_get_ms()", http)
        self.assertNotIn("HTTPClient_SendHttpHeaders(&transport,\n                                                  NULL,", send_only)
        self.assertNotIn("HTTPClient_SendHttpData(&transport,\n                                                   NULL,", send_only)
        self.assertEqual(send_only.count("http_client_get_timestamp_ms"), 2)

    def test_send_only_atop_request_does_not_parse_an_absent_response(self) -> None:
        source = read(ATOP_BASE)
        sent = source[source.index("if (HTTP_CLIENT_SUCCESS != http_status)"):
                      source.index("size_t result_buffer_length = 0")]

        self.assertIn("if (request->send_only)", sent)
        self.assertIn("return OPRT_OK", sent)


if __name__ == "__main__":
    unittest.main()
