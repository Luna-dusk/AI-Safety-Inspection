"""aiohttp 入口：在 server/ 目录下 `python -m aichat.main`（先 cp .env.example .env 填 key）。"""
import logging
import os

import aiohttp
from aiohttp import web

from . import pipeline, ws
from .config import Config
from .llm_deepseek import DeepSeekLLM
from .session_store import SessionStore


def load_dotenv():
    """极简 .env 读取（不引第三方依赖）：KEY=VALUE，# 开头跳过，已存在的环境变量优先。"""
    for p in (os.path.join(os.getcwd(), ".env"),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".env")):
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    os.environ.setdefault(k.strip(), v.strip())
            logging.getLogger("aichat").info("loaded env from %s", p)
            return


async def chat_handler(request: web.Request) -> web.StreamResponse:
    return await pipeline.handle_chat(request.app["ctx"], request)


async def delete_handler(request: web.Request) -> web.Response:
    return await pipeline.handle_delete(request.app["ctx"], request)


async def health_handler(request: web.Request) -> web.Response:
    return await pipeline.handle_health(request.app["ctx"], request)


async def _on_startup(app: web.Application):
    app["http"] = aiohttp.ClientSession()          # 全局共享：连接池 + WS 客户端
    app["store"]._http = app["http"]               # store/ctx 的 http 引用在 loop 内补齐
    app["ctx"].http = app["http"]
    await app["store"].start()                     # 启动空闲回收清扫任务


async def _on_cleanup(app: web.Application):
    await app["store"].stop()
    await app["http"].close()


def make_app(cfg: Config = None) -> web.Application:
    cfg = cfg or Config()
    app = web.Application(client_max_size=16 * 1024 * 1024)   # 上行上限 480KB（15s），留足余量
    app["cfg"] = cfg
    app["store"] = SessionStore(cfg, None)
    app["ctx"] = pipeline.Ctx(cfg, None, app["store"], DeepSeekLLM(cfg))
    app.on_startup.append(_on_startup)
    app.on_cleanup.append(_on_cleanup)
    app.router.add_post("/api/v1/chat", chat_handler)
    app.router.add_delete("/api/v1/chat/{session_id}", delete_handler)
    app.router.add_get("/healthz", health_handler)
    app.router.add_get("/ws/v1", ws.ws_handler)   # v2 长连接（附录 B）；v1 HTTP 保留为降级路径
    return app


def main():
    load_dotenv()
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s %(message)s")
    app = make_app()
    cfg = app["cfg"]
    if not (cfg.asr_ready() and cfg.llm_ready() and cfg.tts_ready()):
        logging.getLogger("aichat").warning(
            "stages not fully configured: asr=%s llm=%s tts=%s "
            "（对应轮会回 err 事件，设备走本地兜底）；/healthz 可查",
            cfg.asr_ready(), cfg.llm_ready(), cfg.tts_ready())
    web.run_app(app, host=cfg.host, port=cfg.port)


if __name__ == "__main__":
    main()
