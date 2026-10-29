# ESP32-S3 + Gemini Live voice assistant

This is a starter prototype for the supplied ESP32-S3 / INMP441 / MAX98357A / SH1106 wiring. It streams mono PCM from the ESP32 to a small Python WebSocket bridge, which connects to Google's Gemini Live API, then streams generated PCM audio back to the MAX98357A.

## Important wiring assumptions

- INMP441: BCLK/SCK GPIO6, WS GPIO5, SD GPIO4.
- MAX98357A: BCLK GPIO16, LRC/WS GPIO15, DIN GPIO7. Connect amplifier SD/EN to 3V3 to keep it enabled unless your particular module is wired to a GPIO.
- OLED SH1106 I2C: SDA GPIO41, SCL GPIO42, VCC 3V3, GND GND.
- Confirm every pin against the labels on your physical board/module before powering it. The provided diagram is visually ambiguous in a few wire crossings.
- The speaker must be connected to the MAX98357A speaker outputs, not directly to ESP32 pins.

## 1. Create a Gemini API key

Create an API key in Google AI Studio. Keep it secret; do not paste it into the Arduino sketch or commit it to a repository.

## 2. Deploy the bridge

Deploy this folder as a Python web service on a host that supports WebSocket upgrades (the included `render.yaml` is a starting point).

- Build command: `pip install -r requirements.txt`
- Start command: `python server.py`
- Add environment variable `GEMINI_API_KEY` with your key in the hosting dashboard.
- Optional environment variable `GEMINI_LIVE_MODEL` defaults to `gemini-3.8-live`. If that model is not enabled for your API project, set it to a currently available Live API model from Google's documentation.
- The service needs outbound WebSocket access to Google's Gemini API.

## 3. Arduino IDE

Install these libraries using Library Manager:

- U8g2 by oliver
- WebSockets by Markus Sattler (Links2004)

The Wi-Fi and TLS libraries come with the ESP32 Arduino core. Select your actual ESP32-S3 board and a recent ESP32 Arduino core. This sketch uses the legacy `driver/i2s.h` API; if your installed core version removes/changes that API, it must be adapted to that core's I2S driver.

Edit these fields near the top of `ESP32_Gemini_Live.ino`:

- `WIFI_ADIN`
- `WIFI_SIFREN`
- `YOUR-SERVICE.onrender.com` (host only, no `https://`)

The sketch assumes the deployed service accepts secure WebSockets on port 443 at `/`. It calls `setInsecure()` as a prototype shortcut, which does not verify the server certificate. Do not use that setting in a production/security-sensitive device; configure root CA verification instead.

## Notes and limitations

- This is a starter integration, not a claim of hardware-tested or compile-verified firmware. I cannot compile against your exact board/core from here.
- Gemini Live API availability, model names, quotas, pricing, and account access can change; verify the model ID in the official docs.
- Continuous microphone streaming may create echo because the microphone can hear the speaker. For a better hands-free device, add acoustic echo cancellation or mute/duck the mic while audio is playing.
- The code is intentionally simple and sends I2S chunks from `loop()`. For more robust low-latency full-duplex audio, use FreeRTOS tasks and ring buffers so websocket callbacks and I2S never block each other.
- Audio must be 16-bit little-endian PCM at 16 kHz on input; Gemini Live returns 24 kHz PCM output. The bridge uses binary WebSocket frames for raw audio in both directions.
