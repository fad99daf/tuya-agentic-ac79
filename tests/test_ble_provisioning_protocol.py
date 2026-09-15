"""Deterministic protocol vectors for AC79 Tuya BLE Wi-Fi provisioning."""

from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BLE_PROV = ROOT / (
    "overlay/apps/common/LLM/tuya_agentic/agentic-kit/modules/tuya-ble/"
    "src/tuya_ble_prov.c"
)

BLE_FRAME_HEADER_LEN = 12
BLE_FRAME_CRC_LEN = 2


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def make_wifi_frame(json_payload: bytes) -> bytes:
    """Build a downlink-transparent frame exactly as the BLE parser sees it."""
    data = b"\x00\x00\x00\x01" + json_payload
    header = b"\x00\x00\x00\x01\x00\x00\x00\x00\x80\x1b" + len(data).to_bytes(2, "big")
    crc = crc16_modbus(header + data)
    return header + data + crc.to_bytes(2, "big")


def protocol_frame_is_accepted(raw: bytes, *, encrypted: bool) -> bool:
    """Reference the required data_len/padding/CRC acceptance contract."""
    if len(raw) < BLE_FRAME_HEADER_LEN + BLE_FRAME_CRC_LEN:
        return False

    data_len = int.from_bytes(raw[10:12], "big")
    expected_len = BLE_FRAME_HEADER_LEN + data_len + BLE_FRAME_CRC_LEN
    if expected_len > len(raw):
        return False

    padding = raw[expected_len:]
    if padding:
        if not encrypted or len(padding) > 16:
            return False
        if padding != bytes(len(padding)) and padding != bytes([len(padding)]) * len(padding):
            return False

    expected_crc = crc16_modbus(raw[: expected_len - BLE_FRAME_CRC_LEN])
    return raw[expected_len - 2 : expected_len] == expected_crc.to_bytes(2, "big")


class BleProvisioningProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        # Spaces, punctuation and Chinese force a Unicode-character versus UTF-8-byte distinction.
        payload = {
            "ssid": "咖啡 Wi-Fi, #1!",
            "pwd": "pass word!",
            "token": "0123456789abcdef",
            "note": "ASCII filler",
        }
        while True:
            encoded = json.dumps(payload, ensure_ascii=False, separators=(", ", ": ")).encode("utf-8")
            if (BLE_FRAME_HEADER_LEN + 4 + len(encoded) + BLE_FRAME_CRC_LEN) % 16 == 0:
                break
            payload["note"] += "."
        self.json_payload = encoded
        self.frame = make_wifi_frame(encoded)
        self.assertEqual(len(self.frame) % 16, 0)

    def test_utf8_ssid_uses_encoded_byte_count(self) -> None:
        ssid = json.loads(self.json_payload.decode("utf-8"))["ssid"]
        self.assertGreater(len(ssid.encode("utf-8")), len(ssid))
        self.assertLessEqual(len(ssid.encode("utf-8")), 64)

    def test_unpadded_pkcs7_and_zero_padded_frames_are_accepted(self) -> None:
        self.assertTrue(protocol_frame_is_accepted(self.frame, encrypted=True))
        self.assertTrue(protocol_frame_is_accepted(self.frame + bytes([16]) * 16, encrypted=True))
        self.assertTrue(protocol_frame_is_accepted(self.frame + bytes(16), encrypted=True))

    def test_invalid_tail_and_crc_are_rejected(self) -> None:
        self.assertFalse(protocol_frame_is_accepted(self.frame + bytes([16]) * 15 + b"\x01", encrypted=True))
        bad_crc = self.frame[:-1] + bytes([self.frame[-1] ^ 0x01])
        self.assertFalse(protocol_frame_is_accepted(bad_crc, encrypted=True))
        self.assertFalse(protocol_frame_is_accepted(self.frame + bytes(16), encrypted=False))

    def test_firmware_keeps_plaintext_until_data_len_boundary_is_checked(self) -> None:
        source = BLE_PROV.read_text(encoding="utf-8")
        decrypt = source[source.index("static int aes_cbc_decrypt") : source.index("static bool tuya_ble_valid_aes_padding")]
        receive = source[source.index("static void tuya_ble_recv") : source.index("static void build_adv_data")]
        config = source[source.index("static void handle_wifi_config") : source.index("static void tuya_ble_recv")]

        self.assertIn("*out_len = in_len;", decrypt)
        self.assertNotIn("*out_len = in_len -", decrypt)
        self.assertIn("frame_expected_len", receive)
        self.assertIn("tuya_ble_valid_aes_padding(frame, frame_len, frame_expected_len)", receive)
        self.assertLess(receive.index("if (recv_crc != calc_crc)"), receive.index("state->last_rx_sn = sn;"))
        self.assertLess(receive.index("state->last_rx_sn = sn;"), receive.index("switch (cmd)"))
        self.assertIn("strlen(ssid->valuestring)", config)
        self.assertIn("ssid_len > TUYA_BLE_SSID_MAX_LEN", config)
        self.assertNotIn('WiFi JSON: %s', config)
        self.assertNotIn('Password: %s', config)
        self.assertNotIn('Token: %s', config)
        self.assertNotIn("TUYA_BLE_HAL_HEXDUMP(data, data_len)", config)
        self.assertNotIn("TUYA_BLE_HAL_HEXDUMP(packet", receive)
        self.assertNotIn("TUYA_BLE_HAL_HEXDUMP(frame", receive)


if __name__ == "__main__":
    unittest.main()
