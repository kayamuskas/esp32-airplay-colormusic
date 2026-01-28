# ESP32 AirPlay VU Visualizer

ESP-IDF project for ESP32-WROOM: AirPlay 1 (RAOP) receiver with a WS2812B LED VU visualizer. It reuses the RAOP implementation from `squeezelite-esp32` (`components/raop`). Audio output is disabled; decoded PCM is used only for LED visualization.

## Requirements
- ESP-IDF 5.1+ (tested on 5.5.x).
- WS2812B LED strip.

The project includes a minimal FastLED-compatible shim in `components/fastled` built on top of `led_strip`. Replace it with a full FastLED port if you prefer.

## Structure
- `components/raop` — RAOP port from `squeezelite-esp32`.
- `components/alac` — ALAC decoder (Apple) + `alac_wrapper`.
- `main/app_main.cpp` — WiFi, RAOP, PCM sink, LED VU logic.
- `main/Kconfig.projbuild` — WiFi/LED/AirPlay name settings.

## PCM hook (where to tap audio)
ALAC is decoded in `components/raop/rtp.c`:
- `alac_decode()` does the decode.
- `buffer_push_packet()` calls `ctx->data_cb(...)` — this is the decoded PCM hook (int16 stereo).

In this project, `raop_data_cb()` in `main/app_main.cpp` accumulates PCM levels for the VU. No I2S/DAC/Bluetooth output is used.

## Logging
Key logs include:
- mDNS/Bonjour start (`main/app_main.cpp`, `raop_start_after_wifi()` + `components/raop/raop.c`).
- RAOP session start/stop (`raop_cmd_cb()`).
- Sample rate/channels from ALAC init (`components/raop/rtp.c`, `alac_init()`).
- PCM fps and VU averages/levels (`led_task()`).

## Build and flash
1) Set up ESP-IDF environment.
2) Make sure FastLED shim is present in `components/fastled`.
3) Configure:
   - `idf.py menuconfig` → **AirPlay VU Visualizer**
4) Build and flash:
```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Notes
- The visualizer is VU-style (level bars), not FFT.
- Low-level background can be a dim hue or a gentle rainbow flow.
- PCM handling is light-weight (accumulated levels, no ring buffer).

## Initial prompt (project creation)
This was the initial prompt used to create the project:

```
You are a senior embedded engineer. Build an ESP-IDF project for ESP32-WROOM that acts as an AirPlay (RAOP / AirPlay 1) receiver and drives a WS2812B LED strip as a real-time spectrum visualizer.

Constraints:
- Do NOT use shairport-sync (Linux daemon). Instead, reuse the RAOP implementation from squeezelite-esp32 (sle118/squeezelite-esp32), specifically the components/raop code which is reasonably independent.
- We only need PCM samples for FFT; no audio output (no I2S/DAC/Bluetooth).
- Implement a PCM sink that receives decoded PCM frames (int16 stereo, typically 44.1kHz) from the RAOP pipeline and pushes them into a ring buffer.
- Use FreeRTOS tasks:
  1) RAOP/network task(s) (as required by the RAOP component)
  2) FFT task: pull N=1024 frames (mono mix), compute FFT, 16 bands with smoothing and peak hold
  3) LED task (or combined with FFT task): update FastLED WS2812B at ~60 FPS

Dependencies:
- ESP-IDF (specify required version if needed by squeezelite-esp32)
- FastLED (component or idf-compatible port)
- A lightweight FFT (kissfft or esp-dsp; avoid heavy allocations; static buffers)

Deliverables:
- Project structure (CMakeLists.txt, main component)
- Steps to build and flash using idf.py
- Where exactly to hook into the RAOP decoded PCM path (function names/files), and how to disable any audio output paths.
- Add extensive logging:
  - mDNS/Bonjour advertisement started
  - RAOP session start/stop
  - sample rate/channels confirmation
  - PCM frames received per second
  - ring buffer fill level and overrun counts
  - FFT RMS levels
```
