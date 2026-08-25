# tuya-agentic-ac79

End-side port of **Tuya agentic-kit** (AI Agent cloud voice chat) onto the **JieLi AC791N (wl82) AIoT SDK**. Communicates with the cloud AI over Tuya's tRTC realtime channel.

> This repo contains **only the integration code** (the new `tuya_agentic/` plus minor edits to the official SDK). The JieLi SDK itself is **not** included and must be obtained separately. README structure inspired by [xiaozhi-esp32 (Tuya edition)](https://github.com/fad99daf/xiaozhi-esp32).

---

## Features

- **Voice chat** — realtime interaction (ASR + LLM + TTS)
- **Uplink ASR** — PCM 16k / 16bit / mono (JieLi's opus is a Baidu headerless format that Tuya cannot decode, so uplink stays PCM)
- **Downlink TTS** — opus (default, ~2KB/s fights stutter on congested networks) / PCM (optional, stable). Opus now works: CBR + `sample_rate=0` auto-resampling
- **Barge-in (interrupt)** — AEC + VAD + multi-frame energy confirmation (experimental, depends heavily on AEC)
- **Cloud VAD end-of-speech** — wakeup is always local VAD; end-of-speech is decided by the cloud (TAI 2.1 signals it via ChatBreak; ServerVad is also handled for compatibility), with a local 2s silence timeout fallback
- **Persistent MQTT + DP downlink** — MQTT and the AI TLS connection are independent TCP links that coexist; a `tuya_mqtt_ka` thread keeps the heartbeat and receives DP downlink. The device stays online in the App, and cloud DP/MCP commands arrive in real time (`on_dp_downlink` / `on_event`)
- **TTS first-byte prebuffering** — buffers ~160ms of audio at the start of each TTS turn before feeding the decoder, fixing first-frame underrun stutter
- **Tuya cloud OTA** — checks for upgrade before connecting to AI on boot; downloads, flashes, and auto-reboots if a new firmware exists (dual-bank)
- **Tuya BLE one-click provisioning** — via the "Tuya Smart" App
- **Persistent credentials** — device triple (devid/secret/localkey) written to VM after activation; direct-connect on later boots
- **K6 long-press resets provisioning** — clears credentials and re-enters provisioning

> Note: this port does **not** include image understanding/generation or device MCP (not implemented on the device side). Cloud AI capabilities depend on the Tuya platform configuration.

---

## Hardware

- **JieLi AC791N (wl82)** board (based on the SDK's `wifi_story_machine` app)
- Microphone (uplink ASR), speaker (downlink TTS)
- Keys **K1–K8** (AD ladder on PB1); **K6 = KEY_PHOTO, long-press = clear provisioning & reset**
- UART (flashing + logs, default 115200)

---

## 0. Prerequisites

### SDK version (important)

Built against **JieLi AC79 AIoT SDK `AC79NN_SDK_V1.2.0`**. You **must check out exactly this tag** — other versions have different source line numbers, so the patch / overlay won't align. Official repo: <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>

### Tuya IoT platform preparation

Before use, complete the following on the [Tuya IoT Development Platform](https://iot.tuya.com):

| Requirement | Description | How to get |
| --- | --- | --- |
| **Product PID** | Identifies a class of devices and its bound AI Agent | Create a product on the platform |
| **Device auth code** (uuid / authkey) | Unique per device, used to activate and obtain cloud credentials | Claim free test auth codes on the platform |

Rough flow:
1. **Create a product** → get the **Product PID**
2. Configure the **AI Agent** for the product (system prompt, TTS voice, language, etc.)
3. **Claim test auth codes** (uuid + authkey)
4. Fill PID / uuid / authkey into `tuya_agentic_demo.c` (search for `YOUR_PID_HERE`; see "Quick start" step 4)

> A small number of auth codes can be claimed for free during testing; for mass production contact Tuya sales.

---

## 1. Quick start

```bash
# 1) Get the official SDK and pin the version (must be V1.2.0)
git clone https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git
cd fw-AC79_AIoT_SDK
git checkout AC79NN_SDK_V1.2.0

# 2) Clone this integration repo (anywhere)
git clone <this-repo-url> ../tuya-agentic-ac79

# 3) Merge (copy the integration code into the SDK)
#    Linux / macOS / Git Bash:
bash ../tuya-agentic-ac79/apply.sh .
#    Windows CMD:
#    ..\tuya-agentic-ac79\apply.bat .

# 4) Fill in your own Tuya credentials: edit
#    apps/common/LLM/tuya_agentic/tuya_agentic_demo.c search for `YOUR_PID_HERE`
#    (repo ships placeholders, see "Tuya credentials" below)

# 5) Build
make ac791n_wifi_story_machine
```

After merging, `apps/common/LLM/tuya_agentic/` holds the integration code, and `apps/wifi_story_machine/` auto-starts the Tuya flow on boot.

---

## 2. Two merge methods

| Method | Command | Use when |
|---|---|---|
| **overlay overwrite** (default, recommended) | Linux/Mac/Git Bash: `bash apply.sh <SDK_root>` · Windows: `apply.bat <SDK_root>` | Clean official SDK; full-file overwrite |
| **patch** (edits only) | `cd <SDK_root> && git apply patches/tuya-agentic-v1.2.0.patch` | You want to review the diff line-by-line, or the SDK already has your own changes |

- overlay **overwrites 10 SDK files** and **adds the whole `tuya_agentic/` directory**.
- patch only modifies those 10 files (the new directory must still be copied in via overlay).

> ⚠ **Re-running apply overwrites `demo.c` and resets your credentials back to placeholders.** If you've already filled them in, edit the target file directly for later changes, or re-fill after re-running.

---

## 3. Tuya credentials

After merging, fill in **your own** Tuya product triple in `apps/common/LLM/tuya_agentic/tuya_agentic_demo.c` (**search for `YOUR_PID_HERE`**). The repo ships placeholders (**no real credentials included**) — replace them:

```c
#define TUYA_PRODUCT_KEY    "YOUR_PID_HERE"      /* PID:     Tuya IoT platform -> your product -> Product ID */
#define TUYA_UUID           "YOUR_UUID_HERE"     /* UUID:    Tuya IoT platform -> Device -> UUID            */
#define TUYA_AUTH_KEY       "YOUR_AUTHKEY_HERE"  /* AuthKey: Tuya IoT platform -> Device -> AuthKey         */
```

Line 51 `TUYA_ACTIVATION_TOKEN` (default placeholder `xxxxxxxx`) is the provisioning activation token: only fill it once from the platform/App when manually testing activation; the normal App provisioning flow does **not** need it.

The device triple (devid / secret / localkey) is activated by the cloud and written to VM (indices 176–180) on the first BLE provisioning — survives power loss, **no need to fill it**.

---

## 4. Configuration switches

Edit `apps/wifi_story_machine/include/app_config.h`:

| Macro | Description | Default |
|---|---|---|
| `CONFIG_TUYA_AGENTIC_ENABLE` | Tuya integration master switch (controls Makefile sources + K6 reset branch + auto-start) | on |
| `TUYA_BARGE_IN_ENABLE` | Interrupt TTS (depends on AEC; experimental) | on |
| `TUYA_DOWNLINK_OPUS_ENABLE` | Use opus for downlink TTS (fights stutter); commented = PCM | off (commented) |

---

## 5. Provisioning & reset

### BLE provisioning (first time)

1. On boot, with no triple stored, the device auto-enters BLE advertising and periodically announces "please configure the network"
2. Open the **Tuya Smart App** (or an OEM app) → add device
3. The App finds the device → sends WiFi credentials + provisioning token over BLE
4. The device connects to WiFi → cloud activation → triple written to VM → connects to AI
5. Later boots read the VM and connect directly, no re-provisioning needed

### Reset provisioning

- **Long-press K6** → clears the VM triple → soft reset → re-enters provisioning on reboot
- (BT advertising is stopped before the reset to avoid a dirty BT-controller state after soft reset causing provisioning failure — see FAQ)

---

## 6. FAQ

| Symptom | Cause / Fix |
|---|---|
| **Tuya App can't find the Bluetooth device** | You flashed the placeholder build; `demo.c` still says `YOUR_PID_HERE`. Fill in real PID/uuid/authkey and rebuild |
| **Provisioning fails after long-press K6** (but works after the reset key) | Soft reset (P33) doesn't fully reset the BT controller like a power cycle. Fixed by stopping BT + delay before reset; if it still happens occasionally, use the reset key (cold boot) or retry |
| **Downlink TTS has squeal/noise** | If opus behaves abnormally, fall back to PCM (comment out `TUYA_DOWNLINK_OPUS_ENABLE`). Opus is now working (CBR + sample_rate=0 auto-resampling) |
| **Connects then drops (conn nack → timeout) during provisioning** | Usually 2.4G RF interference. Turn off phone WiFi, move closer, retry a few times |
| **Patch won't apply / line numbers off** | Wrong SDK version. Must be `AC79NN_SDK_V1.2.0` |

---

## 7. Known issues / notes

- **Downlink opus is now working** (CBR + `sample_rate=0` lets the decoder auto-output 48k and resample to DAC); enabled by default. Earlier `sample_rate=16000` forced alignment caused slow/low-pitched audio, and removing CBR hung the decoder — both fixed.
- **OTA version is a manual scheme**: `TUYA_FIRMWARE_VERSION` must be updated before each release to match what's filled in on the Tuya platform. Auto-persisting the version across OTA (VM/USER/BTIF/RTC) was verified unreliable, so it's not used.
- **Barge-in depends heavily on AEC**; experimental with a single mic. Tuning details in [`docs/CHANGES.md`](docs/CHANGES.md).
- **Version-bound**: this repo only fits `AC79NN_SDK_V1.2.0`. To track a newer official SDK, regenerate the patch.

---

## 8. What changed

Full list in [`docs/CHANGES.md`](docs/CHANGES.md). Summary:

- **New** `apps/common/LLM/tuya_agentic/`: `tuya_agentic_demo.c` (main loop), `pal_ac791n.c` (PAL), `le_net_cfg_tuya.c/.h` (BLE provisioning), bool-compat shims, and the pulled-in `agentic-kit/`
- **Edited SDK files** (8): `audio_input.c/.h`, `user_cfg.c` (AEC), `app_music.c` (K6), `Makefile`, `app_config.h`, `wifi_app_task.c`, `app_main.c` (btstack stack 768→2048)
- **Optional debug change**: `board_7916A.c` (UART baudrate, just for logs)

---

## 9. Repository layout

```
tuya-agentic-ac79/
├── README.md            ← Chinese readme
├── README.en.md         ← this file
├── apply.sh             ← merge script (Linux/Mac/Git Bash)
├── apply.bat            ← merge script (Windows)
├── overlay/             ← integration code in SDK-relative paths
├── patches/
│   └── tuya-agentic-v1.2.0.patch   ← unified diff of the 10 changed files
└── docs/
    ├── INTEGRATION.md   ← integration architecture
    └── CHANGES.md       ← full change list
```

---

## License

Integration code: Apache-2.0 (same as the JieLi SDK). Pulled-in modules under `agentic-kit/` (Tuya open-source, AWS coreHTTP / coreMQTT) keep their original licenses.

## Acknowledgements

- [Tuya agentic-kit](https://github.com/tuya) — AI Agent device SDK
- [JieLi AC79 AIoT SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK) — chip SDK
- [xiaozhi-esp32 (Tuya edition)](https://github.com/fad99daf/xiaozhi-esp32) — README structure reference
