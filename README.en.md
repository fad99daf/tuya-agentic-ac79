# tuya-agentic-ac79

End-side port of **Tuya agentic-kit** (AI Agent cloud voice chat) onto the **JieLi AC791N (wl82) AIoT SDK**. Communicates with the cloud AI over Tuya's tRTC realtime channel.

> This repo contains **only the integration code** (the new `tuya_agentic/` plus minor edits to the official SDK). The JieLi SDK itself is **not** included and must be obtained separately. README structure inspired by [xiaozhi-esp32 (Tuya edition)](https://github.com/fad99daf/xiaozhi-esp32).

---

## Features

- **Voice chat** — realtime interaction (ASR + LLM + TTS)
- **Wake word "ni hao tu ya" (你好涂鸦) + "hei tu ya" (嘿涂鸦)** — always-on recognition via Tuya's closed-source KWS engine (`kws/audio_subsys.a` v2 package, default model fsmn_v8_0515_avg). Both words registered with official CTC tokens — 你好涂鸦={23,4,27,9,22,5,38,1} (primary), 嘿涂鸦={27,8,22,5,38,1} — threshold 0.7 each. On a hit it plays the "I'm here" alert and opens a 15s awake window in which the local VAD may start a turn; same-utterance double-hit suppression and cross-utterance gluing protection included. Falls back to always-listening automatically if the engine fails to init. Parameters and tuning guide in [`docs/WAKEWORD.md`](docs/WAKEWORD.md) (Chinese)
- **Uplink ASR** — opus (default; local libopus 1.4 fixed-point software encoder, 16k/mono/CBR 16kbps/40ms, ~2KB/s — 1/16 of PCM) / PCM (optional, 32KB/s). The mic pipeline stays PCM (VAD/AEC/energy gate/barge-in unaffected); encoding happens per frame only at send time, with automatic PCM fallback if the encoder fails to init. Verified over both TCP and UDP transports
- **Downlink TTS** — opus (default, ~2KB/s fights stutter on congested networks) / PCM (optional, stable). Opus now works: CBR + `sample_rate=0` auto-resampling
- **Selectable transport (TCP / UDP)** — TCP by default (source-level `rtc-tcp-client`, behavior unchanged); optionally switches to Tuya's prebuilt STM OPEN SDK (`stm/libstm_tuya.a`: races UDP/DTLS vs TCP, UDP-first with automatic fallback) via the single `TUYA_TRANSPORT_STM_ENABLE` switch — both backends compile side by side. See `stm/README.md`
- **DNS noise-suppression boost** — noisy-environment ASR: DNS forcibly enabled (immune to stale flash config) + over_drive=3; measured noise floor 9k–64k → 3k–6k. If quiet speech gets eaten, dial back to 2.0–2.5 in the `user_cfg.c` override block
- **Barge-in (interrupt)** — AEC + VAD + multi-frame energy confirmation (depends heavily on AEC). Idle turn-start and during-playback interrupt use two independent energy gates (`BARGE_MIN_ENERGY` 100k / `BARGE_CONFIRM_ENERGY` 600k); the latter specifically rejects AEC residue of TTS echo (measured: residue confirm frames <400k, real speech >1.1M)
- **Music playback** — "play X's songs": the cloud music SKILL returns a trial mp3 URL; the device parses it and plays through JieLi's network decode chain (https with automatic TLS). The DAC is handed over after the TTS announcement and restored when done; speaking during playback stops it (VAD + 3-frame energy gate). ⚠️ Trial clips are ~30s; full songs require the paid music capability on the Tuya platform
- **Cloud VAD end-of-speech** — wakeup is always local VAD; end-of-speech is decided by the cloud (TAI 2.1 signals it via ChatBreak; ServerVad is also handled for compatibility), with a local 2s silence timeout fallback (TCP transport). On the STM/UDP transport this event is not yet distinguishable and the code degrades to the local silence fallback — see `stm/README.md`
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

Built against the **JieLi AC79 AIoT SDK V1.2.0 release archive** (gitee Releases → `fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0.zip`). If you clone via git, check out **`AC79NN_SDK_V1.2.12_2026-03-07`** — that tag's content matches the V1.2.0 release archive byte-for-byte (the patch baseline was verified against it; note there is no tag literally named `AC79NN_SDK_V1.2.0` upstream). Other versions differ and the patch / overlay won't align. Official repo: <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>

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
# 1) Get the official SDK and pin the version (tag identical to the V1.2.0 release archive)
git clone https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git
cd fw-AC79_AIoT_SDK
git checkout AC79NN_SDK_V1.2.12_2026-03-07

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

> ⚠ **Command-line make caveat**: JieLi's bundled old GNU make (`C:/JL/mc/bin/make.exe`) does not understand the Makefile's `$(file >...)` and **silently skips the objs.txt rewrite** — the link step then consumes a stale obj list from a previous build (e.g. CodeBlocks'), producing firmware mixed with old code without any error. Command-line builds must pass full overrides: `make -j8 MKDIR="mkdir -p" RM="rm -rf" LINK_AT=0` (MKDIR/RM overridden because `mkdir_win` is not on this machine; delete `sdk.elf` first if in doubt). Building from the JieLi CodeBlocks IDE is unaffected.

After merging, `apps/common/LLM/tuya_agentic/` holds the integration code, and `apps/wifi_story_machine/` auto-starts the Tuya flow on boot.

---

## 2. Two merge methods

| Method | Command | Use when |
|---|---|---|
| **overlay overwrite** (default, recommended) | Linux/Mac/Git Bash: `bash apply.sh <SDK_root>` · Windows: `apply.bat <SDK_root>` | Clean official SDK; full-file overwrite |
| **patch** (edits only) | `cd <SDK_root> && git apply patches/tuya-agentic-v1.2.0.patch` | You want to review the diff line-by-line, or the SDK already has your own changes |

- overlay **overwrites 10 SDK files** and **adds the whole `tuya_agentic/` directory**.
- patch only modifies those 10 files (the new directory must still be copied in via overlay).
- The new directory ships a bundled libopus fixed-point encoder with a **prebuilt `libopus_tuya.a`** — no local rebuild needed for normal use; only if you modify sources under `libopus/`, re-run its `build_tuya_libopus.sh` (needs the JieLi LLVM toolchain at `/c/JL/pi32/bin`) and rebuild the project. The `stm/` libs `libstm.a` (Tuya factory build) / `libstm_tuya.a` (platform-patched) are prebuilt too — only re-run `patch_libstm.sh` when Tuya ships a new factory lib.

> ⚠ **Re-running apply overwrites `demo.c` and resets your credentials back to placeholders.** If you've already filled them in, edit the target file directly for later changes, or re-fill after re-running.

---

## 3. Tuya credentials

After merging, fill in **your own** Tuya product triple in `apps/common/LLM/tuya_agentic/tuya_agentic_demo.c` (**search for `YOUR_PID_HERE`**). The repo ships placeholders (**no real credentials included**) — replace them:

```c
#define TUYA_PRODUCT_KEY    "YOUR_PID_HERE"      /* PID:     Tuya IoT platform -> your product -> Product ID */
#define TUYA_UUID           "YOUR_UUID_HERE"     /* UUID:    Tuya IoT platform -> Device -> UUID            */
#define TUYA_AUTH_KEY       "YOUR_AUTHKEY_HERE"  /* AuthKey: Tuya IoT platform -> Device -> AuthKey         */
```

`TUYA_ACTIVATION_TOKEN` (default placeholder `xxxxxxxx`) is the provisioning activation token: only fill it once from the platform/App when manually testing activation; the normal App provisioning flow does **not** need it.

The device triple (devid / secret / localkey) is activated by the cloud and written to VM (indices 176–180) on the first BLE provisioning — survives power loss, **no need to fill it**.

---

## 4. Configuration switches

Edit `apps/wifi_story_machine/include/app_config.h`:

| Macro | Description | Default |
|---|---|---|
| `CONFIG_TUYA_AGENTIC_ENABLE` | Tuya integration master switch (controls Makefile sources + K6 reset branch + auto-start) | on |
| `TUYA_TRANSPORT_STM_ENABLE` | Voice transport: 0 = TCP (rtc-tcp-client sources); 1 = Tuya STM lib (UDP-first, races UDP/TCP with fallback) — see `stm/README.md` | 0 |
| `TUYA_BARGE_IN_ENABLE` | Interrupt TTS (depends on AEC; experimental) | on |
| `TUYA_KWS_ENABLE` | Wake word "你好涂鸦" (primary) + "嘿涂鸦" gating (closed-source engine, always-on; falls back to always-listening on engine failure) — see `docs/WAKEWORD.md` | on |
| `TUYA_DOWNLINK_OPUS_ENABLE` | Use opus for downlink TTS (fights stutter); commented = PCM | on |
| `TUYA_UPLINK_OPUS_ENABLE` | Encode uplink ASR with the local libopus fixed-point encoder (~2KB/s); commented = PCM (32KB/s) | on |
| `TUYA_SERVER_VAD_ENABLE` | Cloud VAD end-of-speech (wake-up stays local VAD; local 2s silence fallback) | on |
| `TUYA_MUSIC_ENABLE` | Music skill: parse the music SKILL and play via the network decode chain; speech can stop playback | on |
| `TUYA_OTA_ENABLE` / `TUYA_FIRMWARE_VERSION` | Tuya cloud OTA; version is a manual scheme (update the macro to match the platform before each release) | 1 / "1.0.11" |

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
| **Music stops after ~30 seconds** | Platform trial-clip limit; full songs require the paid music capability on the Tuya platform |
| **Music stops by itself mid-play** | False barge-in trigger: music pickup got through the energy gate. Check the `[MUSIC-DBG]` baseline sum in the logs and raise `BARGE_CONFIRM_ENERGY` (`tuya_agentic_demo.c`) |
| **ASR misrecognizes in noisy environments** | Check `idle drain avg_sum` (noise floor) vs speech-frame `act%` in the serial log. DNS is forcibly enabled with over_drive=3 (`user_cfg.c` override block); if **quiet speech gets eaten / recognition worsens** (over-suppression), dial `DNS_over_drive` back to 2.0–2.5. Note DNS only suppresses stationary noise (fans/hum); non-stationary noise (nearby voices, TV) can't be filtered — speak closer to the mic |
| **Patch won't apply / line numbers off** | Wrong SDK version. Use `AC79NN_SDK_V1.2.12_2026-03-07` (content = the official V1.2.0 release archive; the patch baseline) |

---

## 7. Known issues / notes

- **Downlink opus is now working** (CBR + `sample_rate=0` lets the decoder auto-output 48k and resample to DAC); enabled by default. Earlier `sample_rate=16000` forced alignment caused slow/low-pitched audio, and removing CBR hung the decoder — both fixed.
- **Uplink opus is a local libopus fixed-point software encoder** (all symbols `topus_`-prefixed for isolation — zero conflict with JieLi's closed-source opus libs; a prebuilt `libopus_tuya.a` is bundled). A clang+LTO crash with a local libopus **floating-point decoder** was recorded and that decoder path was dropped — only the fixed-point **encoder** remains; if boot/speech crashes reappear, comment out `TUYA_UPLINK_OPUS_ENABLE` to fall back to PCM uplink while diagnosing. Modifying libopus sources requires re-running `build_tuya_libopus.sh`, then rebuilding.
- **Music interrupt is "stop then listen"**: the interrupting utterance overlaps the music and is discarded (not sent to ASR) — say the next command after the music stops. If the music stops by itself, raise `BARGE_CONFIRM_ENERGY` based on the `[MUSIC-DBG]` logs (AEC against continuous music is unverified).
- **OTA version is a manual scheme**: `TUYA_FIRMWARE_VERSION` must be updated before each release to match what's filled in on the Tuya platform. Auto-persisting the version across OTA (VM/USER/BTIF/RTC) was verified unreliable, so it's not used.
- **Barge-in depends heavily on AEC**; experimental with a single mic. Tuning details in [`docs/CHANGES.md`](docs/CHANGES.md). If TTS answers get chaotic / cut off mid-sentence (echo self-interrupt), raise `BARGE_CONFIRM_ENERGY` (default 600k; measured residue confirm frames <400k vs real speech >1.1M on 2026-09-04).
- **Transport defaults to TCP.** For UDP (`TUYA_TRANSPORT_STM_ENABLE 1`), usage/principles/known gaps (cloud VAD event indistinguishable, MCP responses over the TEXT channel) are in `stm/README.md`; the STM lib is a Tuya-team prebuild (LLVM IR archive, 725KB).
- **Version-bound**: the patch baseline is the official V1.2.0 release archive content (git tag `AC79NN_SDK_V1.2.12_2026-03-07`, verified). To track a newer official SDK, regenerate the patch.

---

## 8. What changed

Full list in [`docs/CHANGES.md`](docs/CHANGES.md). Summary:

- **New** `apps/common/LLM/tuya_agentic/`: `tuya_agentic_demo.c` (main loop), `pal_ac791n.c` (PAL), `le_net_cfg_tuya.c/.h` (BLE provisioning), `tuya_music.c/.h` (music skill parsing), `tuya_ota.c/.h` (OTA), `tuya_opus_enc.c/.h` + `libopus/` (uplink opus encoding, prebuilt archive included), `stm/` (Tuya STM OPEN SDK adapter for the optional UDP transport: `tuya_stm_ai.c` + `stm_port_ac79_shim.c` + prebuilt libs + repatch script), `kws/` (wake word "你好涂鸦"+"嘿涂鸦": closed-source engine `audio_subsys.a` — acoustics team v2 package — + reverse-engineered API header + C++ shim + thin wrapper — see `docs/WAKEWORD.md`), bool-compat shims, and the pulled-in `agentic-kit/`
- **Edited SDK files** (10): `audio_input.c/.h`, `user_cfg.c` (AEC), `app_music.c` (K6 + music playback exports + wake alert prompt), `Makefile`, `AC791N_WIFI_STORY_MACHINE.cbp`, `app_config.h`, `wifi_app_task.c`, `app_main.c` (btstack stack 768→2048)
- **New resource** `cpu/wl82/tools/audlogo/WakeHeyTuya.mp3` (wake alert prompt, copied verbatim by overlay; other stock audlogo files are not touched)
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
    ├── WAKEWORD.md      ← wake-word subsystem: v2 engine & official tokens / params / tuning / logs (Chinese)
    └── CHANGES.md       ← full change list
```

---

## License

Integration code: Apache-2.0 (same as the JieLi SDK). Pulled-in modules under `agentic-kit/` (Tuya open-source, AWS coreHTTP / coreMQTT) keep their original licenses; `tuya_agentic/libopus/` (libopus 1.4 subset, incl. the prebuilt `libopus_tuya.a`) is BSD-3-Clause (see its `COPYING`); `libstm.a` / `libstm_tuya.a` under `tuya_agentic/stm/` are prebuilt libraries provided by the Tuya team — copyright Tuya, redistributed here solely for use with that SDK.

## Acknowledgements

- [Tuya agentic-kit](https://github.com/tuya) — AI Agent device SDK
- [JieLi AC79 AIoT SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK) — chip SDK
- [xiaozhi-esp32 (Tuya edition)](https://github.com/fad99daf/xiaozhi-esp32) — README structure reference
