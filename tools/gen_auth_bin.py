# -*- coding: utf-8 -*-
"""gen_auth_bin.py — 生成涂鸦三元组量产授权区 96B bin(2026-09-28 三方契约定稿版)

结构与 apps/common/LLM/tuya_agentic/tuya_auth_region.h 的 tuya_auth_region_t 逐字节一致,
烧录挂载点见 cpu/wl82/tools/isd_config_rule.c 的 [RESERVED_EXPAND_CONFIG] USER_FILE:
    cpu/wl82/tools/tuya_auth/auth_test.bin   (app_config.h 开 TUYA_AUTH_EMBED_TEST_BIN 才挂载)

布局(共 96B,裸放区偏移 +0,无任何文件头;多字节整数小端):
    [0..7]   magic      "TUYAAUTH"
    [8..9]   version    u16 = 1
    [10..11] head_size  u16 = 96
    [12..28] product_key char[17](NUL 结尾,内容<=16)
    [29..49] uuid        char[21](NUL 结尾,内容<=20)
    [50..82] auth_key    char[33](NUL 结尾,内容<=32)
    [83..93] reserved    11B 全 0
    [94..95] crc16       u16,CRC16-CCITT-FALSE 覆盖字节 [0..93]
                          (poly=0x1021, init=0xFFFF, 无反射, 无异或输出;
                           校验向量 crc16("123456789",9)=0x29B1)

用法:
    python gen_auth_bin.py <product_key> <uuid> <auth_key> <out.bin>

⚠ 生成的是真实授权凭据:输出 bin 一律不进 git 仓库(overlay/.gitignore 已挡 *.bin),
  不上传、不截图、不贴日志;auth_key 只允许出现在烧写器工装与授权区里。
"""
import struct
import sys


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def make_triplet_bin(pk: str, uuid: str, auth_key: str) -> bytes:
    body = b"TUYAAUTH" + struct.pack("<HH", 1, 96)
    body += pk.encode("ascii")[:16].ljust(17, b"\0")       # product_key[17]
    body += uuid.encode("ascii")[:20].ljust(21, b"\0")     # uuid[21]
    body += auth_key.encode("ascii")[:32].ljust(33, b"\0") # auth_key[33]
    body += b"\0" * 11                                     # reserved[11]
    assert len(body) == 94, len(body)
    return body + struct.pack("<H", crc16_ccitt_false(body))


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    pk, uuid, auth_key, out = sys.argv[1:5]
    blob = make_triplet_bin(pk, uuid, auth_key)
    with open(out, "wb") as f:
        f.write(blob)
    # 只打印非敏感信息:auth_key 永不回显
    print("OK: 96 bytes -> %s (pk=%s uuid=%s... crc=0x%04x)"
          % (out, pk, uuid[:8], crc16_ccitt_false(blob[:94])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
