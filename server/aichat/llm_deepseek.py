"""deepseek 流式（openai 兼容接口，官方直连，附录 A.6）。

约束（A.6）：streaming 必开；temperature 0.7；max_tokens ≤ 120；
系统提示词随固件版本一起管理（config.py 的 DEFAULT_SYSTEM_PROMPT）。
上下文最近 10 轮，由 session_store 维护，设备只传 session/turn。
"""
import logging
from typing import AsyncIterator, List, Dict

from openai import AsyncOpenAI

log = logging.getLogger("aichat.llm")


class LLMError(RuntimeError):
    pass


class DeepSeekLLM:
    def __init__(self, cfg):
        self._cfg = cfg
        self._client = None
        if cfg.llm_ready():
            self._client = AsyncOpenAI(api_key=cfg.deepseek_api_key,
                                       base_url=cfg.deepseek_base_url)

    async def stream(self, messages: List[Dict[str, str]]) -> AsyncIterator[str]:
        """逐 token 产出 delta；由 pipeline 直接喂 TTS（V3 官方推荐直喂，见 splitter.py）。

        思考程度由 LLM_THINKING 控制（config.py 有档位说明）；思考模型的
        reasoning_content delta 在正文之前到达，这里只透传 content，思考过程自然丢弃。
        """
        if self._client is None:
            raise LLMError("llm_unavailable (DEEPSEEK_API_KEY not set)")
        kwargs = {}
        if self._cfg.llm_thinking == "off":
            kwargs["extra_body"] = {"thinking": {"type": "disabled"}}
        elif self._cfg.llm_thinking not in ("", "default"):
            kwargs["reasoning_effort"] = self._cfg.llm_thinking
        resp = await self._client.chat.completions.create(
            model=self._cfg.deepseek_model,
            messages=messages,
            stream=True,
            temperature=self._cfg.llm_temperature,
            max_tokens=self._cfg.llm_max_tokens,
            **kwargs,
        )
        async for chunk in resp:
            if not chunk.choices:
                continue
            delta = chunk.choices[0].delta.content
            if delta:
                yield delta
