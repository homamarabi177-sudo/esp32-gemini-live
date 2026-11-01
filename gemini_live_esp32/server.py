
import asyncio
import os
import logging
import websockets

from google import genai
from google.genai import types


# LOG AYARLARI
logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s:%(name)s:%(message)s"
)

LOG = logging.getLogger("gemini-live-bridge")


# SUNUCU AYARLARI
API_KEY = os.environ.get("GEMINI_API_KEY")
PORT = int(os.environ.get("PORT", "8080"))
MODEL = os.environ.get(
    "GEMINI_LIVE_MODEL",
    "gemini-3.8-live"
)


if not API_KEY:
    raise RuntimeError(
        "GEMINI_API_KEY environment variable is required"
    )


# GEMINI İSTEMCİSİ
client = genai.Client(api_key=API_KEY)


# GEMINI SES AYARLARI
CONFIG = {
    "response_modalities": ["AUDIO"],

    "system_instruction": (
        "Sen Türkçe konuşan, yardımsever ve doğal bir "
        "sesli asistansın. Kullanıcıyla karşılıklı konuş. "
        "Cevaplarını kısa ve anlaşılır tut. "
        "Kullanıcı başka bir dil istemedikçe Türkçe konuş."
    ),

    "speech_config": {
        "voice_config": {
            "prebuilt_voice_config": {
                "voice_name": "Aoede"
            }
        }
    }
}


# ESP32 BAĞLANTISI
async def handle_device(device_ws):

    peer = getattr(device_ws, "remote_address", None)

    LOG.info("ESP32 baglandi: %s", peer)

    try:

        LOG.info(
            "Gemini baglantisi deneniyor. Model: %s",
            MODEL
        )

        async with client.aio.live.connect(
            model=MODEL,
            config=CONFIG
        ) as session:

            LOG.info("Gemini Live oturumu acildi!")

            # ESP32 -> GEMINI
            async def device_to_gemini():

                async for message in device_ws:

                    if isinstance(message, bytes):

                        LOG.info(
                            "ESP32 ses gonderdi: %d byte",
                            len(message)
                        )

                        await session.send_realtime_input(
                            audio=types.Blob(
                                data=message,
                                mime_type="audio/pcm;rate=16000"
                            )
                        )


            # GEMINI -> ESP32
            async def gemini_to_device():

                async for response in session.receive():

                    content = getattr(
                        response,
                        "server_content",
                        None
                    )

                    if not content:
                        continue

                    model_turn = getattr(
                        content,
                        "model_turn",
                        None
                    )

                    if not model_turn:
                        continue

                    for part in model_turn.parts or []:

                        inline = getattr(
                            part,
                            "inline_data",
                            None
                        )

                        if inline and inline.data:

                            data = inline.data

                            LOG.info(
                                "Gemini ses gonderdi: %d byte",
                                len(data)
                            )

                            await device_ws.send(data)

                        text = getattr(
                            part,
                            "text",
                            None
                        )

                        if text:

                            LOG.info(
                                "Gemini text part: %s",
                                text[:200]
                            )


            # İKİ YÖNÜ AYNI ANDA ÇALIŞTIR
            tasks = [
                asyncio.create_task(device_to_gemini()),
                asyncio.create_task(gemini_to_device())
            ]

            done, pending = await asyncio.wait(
                tasks,
                return_when=asyncio.FIRST_COMPLETED
            )

            # TAMAMLANMAYAN GÖREVLERİ DURDUR
            for task in pending:
                task.cancel()

            await asyncio.gather(
                *pending,
                return_exceptions=True
            )

            # GÖREV HATALARINI GÖSTER
            for task in done:

                exception = task.exception()

                if exception:
                    raise exception


    except websockets.ConnectionClosed as exc:

        LOG.info(
            "ESP32 baglantisi kapandi: code=%s reason=%s",
            exc.code,
            exc.reason
        )

    except Exception:

        LOG.exception(
            "Gemini Live oturumunda hata olustu"
        )

    finally:

        LOG.info(
            "Baglanti kapandi: %s",
            peer
        )


# ANA SUNUCU
async def main():

    LOG.info(
        "Sunucu baslatiliyor. Port: %s Model: %s",
        PORT,
        MODEL
    )

    async with websockets.serve(
        handle_device,
        "0.0.0.0",
        PORT,
        max_size=None,
        ping_interval=20,
        ping_timeout=60
    ):

        LOG.info("WebSocket sunucusu hazir!")

        await asyncio.Future()


# PROGRAMI BASLAT
if __name__ == "__main__":

    asyncio.run(main())
