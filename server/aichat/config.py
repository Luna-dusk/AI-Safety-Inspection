"""配置：全部来自环境变量（A.12 #2：key 只放服务器环境变量，绝不进仓库）。"""
import os

DEFAULT_SYSTEM_PROMPT = (
    "你是陪老人聊天的助手。规则：\n"
    "1. 第一句不超过 10 个字，先把话说出来，别让人等。\n"
    "2. 每次回答不超过 3 句、60 字，说完就停，不要追问太多。\n"
    "3. 语气慢、用短句、用口语，不用书面语、不用英文、不用表情符号。\n"
    "4. 老人重复问同一件事时，不要纠正，平静地再答一遍。\n"
    "5. 涉及打钱、转账、密码、身份证号，一律拒绝并提示\"这个我做不了，让家里人帮你\"。"
)


def _bool(v: str) -> bool:
    return str(v).strip().lower() in ("1", "true", "yes", "on")


class Config:
    def __init__(self, env=None):
        env = env or os.environ
        self.host = env.get("HOST", "0.0.0.0")
        self.port = int(env.get("PORT", "80"))

        # 设备鉴权
        self.device_ids = [x.strip() for x in env.get("DEVICE_IDS", "DC1").split(",") if x.strip()]
        self.device_tokens = {}
        for pair in env.get("DEVICE_TOKENS", "").split(","):
            if ":" in pair:
                d, t = pair.split(":", 1)
                self.device_tokens[d.strip()] = t.strip()

        # 火山 TTS（双向流式 V3）
        self.tts_api_key = env.get("VOLC_TTS_API_KEY", "")           # 新版控制台
        self.tts_app_id = env.get("VOLC_TTS_APP_ID", "")             # 旧版控制台
        self.tts_access_token = env.get("VOLC_TTS_ACCESS_TOKEN", "")
        self.tts_resource_id = env.get("VOLC_TTS_RESOURCE_ID", "seed-tts-1.0")
        self.tts_speaker = env.get("VOLC_TTS_SPEAKER", "zh_female_cancan_mars_bigtts")
        self.tts_speech_rate = int(env.get("VOLC_TTS_SPEECH_RATE", "-15"))  # [-50,100]，-15≈0.85 倍速
        self.tts_pitch = int(env.get("VOLC_TTS_PITCH", "0"))

        # 火山 ASR（大模型流式识别）
        self.asr_api_key = env.get("VOLC_ASR_API_KEY", "")
        self.asr_app_id = env.get("VOLC_ASR_APP_ID", "")
        self.asr_access_token = env.get("VOLC_ASR_ACCESS_TOKEN", "")
        self.asr_resource_id = env.get("VOLC_ASR_RESOURCE_ID", "volc.bigasr.sauc.duration")

        # LLM（deepseek 官方直连，A.6）
        self.deepseek_api_key = env.get("DEEPSEEK_API_KEY", "")
        self.deepseek_base_url = env.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com")
        # 需求 A.6 写 deepseek-flash；以实际开通的模型名为准，走环境变量
        self.deepseek_model = env.get("DEEPSEEK_MODEL", "deepseek-chat")
        # 思考程度（2026-11-08 实测：flash 默认开思考，正文 TTFT 2.1s；关掉 0.59s）：
        #   off       → 请求体 thinking.type=disabled（推荐，陪聊场景不需要思考）
        #   minimal/low/medium/high → reasoning_effort=<值>（OpenAI 风格，API 接受）
        #   ""/default → 不传参，模型默认行为（flash 即开思考，慢）
        self.llm_thinking = env.get("LLM_THINKING", "off").strip().lower()
        self.llm_max_tokens = int(env.get("LLM_MAX_TOKENS", "120"))
        self.llm_temperature = float(env.get("LLM_TEMPERATURE", "0.7"))
        self.system_prompt = env.get("SYSTEM_PROMPT", DEFAULT_SYSTEM_PROMPT).replace("\\n", "\n")

        # 行为开关
        self.split_text = _bool(env.get("SPLIT_TEXT", "0"))   # A.5 分句器，默认关（见 README 协议对照表）
        self.record_text = _bool(env.get("RECORD_TEXT", "0"))  # A.10 文字留痕，默认关
        self.text_log_path = env.get("TEXT_LOG_PATH", "")
        self.greeting_text = env.get("GREETING_TEXT", "奶奶早，今天感觉怎么样？")
        self.tts_idle_close_ms = int(env.get("TTS_IDLE_CLOSE_MS", "60000"))
        self.session_ttl_ms = int(env.get("SESSION_TTL_MS", "900000"))

    def tts_ready(self) -> bool:
        return bool(self.tts_api_key or (self.tts_app_id and self.tts_access_token))

    def asr_ready(self) -> bool:
        return bool(self.asr_api_key or (self.asr_app_id and self.asr_access_token))

    def llm_ready(self) -> bool:
        return bool(self.deepseek_api_key)
