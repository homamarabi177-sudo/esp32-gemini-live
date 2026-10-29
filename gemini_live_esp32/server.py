import asyncio
import os
import logging
import websockets
from google import genai
from google.genai import types

logging.basicConfig(level=logging.INFO)
LOG = logging.getLogger("gemini-live-bridge")

API_KEY = os.environ.get("GEMINI_API_KEY")
PORT = int(os.environ.get("PORT", "8080"))
MODEL = os.environ.get("GEMINI_LIVE_MODEL", "gemini-3.8-live")

if not API_KEY:
    raise RuntimeError("GEMINI_API_KEY environment variable is required")

client = genai.Client(api_key=API_KEY)
CONFIG = {
    "response_modalities": ["AUDIO"],
    "system_instruction": (
        "Sen Türkçe konuşan, yardımsever ve doğal bir sesli asistansın. "
        "Kullanıcıyla karşılıklı konuş. Cevaplarını kısa ve anlaşılır tut. "
        "Kullanıcı başka bir dil istemedikçe Türkçe konuş."
    ),
    "speech_config": {
        "voice_config": {
            "prebuilt_voice_config": {"voice_name": "Aoede"}
        }
    },
}

async def handle_device(device_ws):
    peer = getattr(device_ws, "remote_address", None)
    LOG.info("ESP32 connected: %s", peer)
    try:
        async with client.aio.live.connect(model=MODEL, config=CONFIG) as session:
            async def device_to_gemini():
                async for message in device_ws:
                    if isinstance(message, bytes):
                        # ESP32 sends raw mono signed PCM, 16-bit little-endian, 16 kHz.
                        await session.send_realtime_input(
                            audio=types.Blob(data=message, mime_type="audio/pcm;rate=16000")
                        )
                    else:
                        # Optional diagnostic text messages from the device are ignored.
                        LOG.debug("Text from device: %s", message[:200])

            async def gemini_to_device():
                async for response in session.receive():
                    content = getattr(response, "server_content", None)
                    if not content:
                        continue
                    model_turn = getattr(content, "model_turn", None)
                    if not model_turn:
                        continue
                    for part in (getattr(model_turn, "parts", None) or []):
                        inline = getattr(part, "inline_data", None)
                        if inline and inline.data:
                            # SDK returns raw audio bytes. The ESP32 plays PCM at 24 kHz.
                            data = inline.data
                            if isinstance(data, str):
                                import base64
                                data = base64.b64decode(data)
                            await device_ws.send(data)
                        text = getattr(part, "text", None)
                        if text:
                            LOG.info("Gemini text part: %s", text[:200])

            tasks = [
                asyncio.create_task(device_to_gemini()),
                asyncio.create_task(gemini_to_device()),
            ]
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            for task in done:
                exception = task.exception()
                if exception:
                    raise exception
    except websockets.ConnectionClosed:
        LOG.info("ESP32 disconnected")
    except Exception:
        LOG.exception("Live session ended with an error")
    finally:
        LOG.info("Connection closed: %s", peer)

async def main():
    LOG.info("Starting Gemini Live bridge on port %s; model=%s", PORT, MODEL)
    async with websockets.serve(
        handle_device,
        "0.0.0.0",
        PORT,
        max_size=None,
        ping_interval=20,
        ping_timeout=60,
    ):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
