from openai import OpenAI
import json
import os
import configparser
from typing import Dict, List, Optional

from tkinter import *


CONFIG_FILE = "config.ini"


# ====================== 配置加载 ======================
def load_config(path: str = CONFIG_FILE):
    """从 config.ini 读取 base_url / api_key / 默认模型。不存在则生成模板。"""
    cfg = configparser.ConfigParser()

    if not os.path.exists(path):
        cfg["api"] = {
            "base_url": "",
            "api_key": "",
        }
        cfg["model"] = {
            "default": "claude-sonnet-5",
        }
        with open(path, "w", encoding="utf-8") as f:
            cfg.write(f)
        print(f"[Config] 未找到 '{path}'，已生成模板，请填写后重新运行。")
        raise SystemExit(0)

    cfg.read(path, encoding="utf-8")

    base_url = cfg.get("api", "base_url", fallback="").strip()
    api_key = cfg.get("api", "api_key", fallback="").strip()
    default_model = cfg.get("model", "default", fallback="").strip()

    if not base_url or not api_key or api_key == "sk-REPLACE_ME":
        print(f"[Config] 请在 '{path}' 中填写有效的 base_url / api_key。")
        raise SystemExit(1)

    return base_url, api_key, default_model


# ====================== 弹窗工具 ======================
def inputBox(title, content):
    root = Tk()
    root.withdraw()
    win = Toplevel(root)
    win.title(title)
    win.geometry("500x350")
    Label(win, text=content).pack(pady=5)
    ta = Text(win, width=60, height=12)
    ta.pack(padx=10)
    ret = [""]

    def ok():
        ret[0] = ta.get("1.0", END).rstrip("\n")
        win.destroy()
        root.destroy()

    Button(win, text="OK", command=ok).pack(pady=5)
    win.wait_window()
    return ret[0]


def infoBox(content):
    root = Tk()
    root.withdraw()
    win = Toplevel(root)
    win.title("Reply")
    win.geometry("550x400")
    ta = Text(win, width=65, height=18)
    ta.pack(padx=10, pady=10)
    ta.insert("1.0", content)
    ta.config(state=DISABLED)

    def close():
        win.destroy()
        root.destroy()

    Button(win, text="Close", command=close).pack()
    win.wait_window()


# ====================== 主体 ======================
class MultiSessionChatter:
    def __init__(
        self,
        base_url: str,
        api_key: str,
        default_model: Optional[str] = None,
        cache_dir: str = "./.bot-history",
        max_completion_tokens: int = 1024,
        temperature: float = 1.0,
        top_p: float = 0.95,
    ):
        # 初始化 OpenAI 兼容客户端
        self.client = OpenAI(api_key=api_key, base_url=base_url)

        # 从服务端拉取可用模型列表（就是你那份 JSON）
        self.available_models: List[str] = []
        self._fetch_models()

        # 决定当前使用的模型
        if default_model and default_model in self.available_models:
            self.model = default_model
        elif default_model:
            # config 里指定的模型不在服务端列表中也允许（比如别名），但会提示
            print(f"[Models] Warning: '{default_model}' not in server model list, still using it.")
            self.model = default_model
        elif self.available_models:
            self.model = self.available_models[0]
        else:
            self.model = default_model or "gpt-4o"

        self.max_tokens = max_completion_tokens
        self.temperature = temperature
        self.top_p = top_p

        # 缓存
        self.cache_dir = cache_dir
        os.makedirs(cache_dir, exist_ok=True)

        # 会话
        self.sessions: Dict[str, List[dict]] = {}
        self.system_prompt = {
            "role": "system",
            "content": "You are an AI assistant."
        }

    # ==================== 模型管理 ====================
    def _fetch_models(self):
        """通过 /v1/models 拉取服务端支持的模型列表。"""
        try:
            resp = self.client.models.list()
            # resp.data 里每一项有 .id
            self.available_models = sorted({m.id for m in resp.data})
            print(f"[Models] Fetched {len(self.available_models)} models from server.")
        except Exception as e:
            print(f"[Models] Failed to fetch model list: {e}")
            self.available_models = []

    def list_models(self, keyword: str = "") -> List[str]:
        if not keyword:
            return list(self.available_models)
        kw = keyword.lower()
        return [m for m in self.available_models if kw in m.lower()]

    def current_model(self) -> str:
        return self.model

    def switch_model(self, model_name: str) -> str:
        """切换当前使用的模型（全局生效，之后所有会话都用它）。"""
        if not model_name:
            return (f"Current model: {self.model}\n"
                    f"Available models: {len(self.available_models)} "
                    f"(use 'model list [keyword]' to list them)")

        # 严格匹配（如果用户没写错）
        if model_name in self.available_models:
            self.model = model_name
            return f"Model switched to '{model_name}'."

        # 大小写不敏感匹配
        lower_map = {m.lower(): m for m in self.available_models}
        if model_name.lower() in lower_map:
            self.model = lower_map[model_name.lower()]
            return f"Model switched to '{self.model}'."

        # 允许强行使用（例如服务端未列出但确实存在的别名）
        self.model = model_name
        return (f"Warning: '{model_name}' not in server model list, "
                f"but set as current model anyway.")

    # ==================== 会话管理 ====================
    def create_session(self, session_id: str, inherit_from: Optional[str] = None):
        if session_id in self.sessions:
            return f"Session {session_id} Exist!"

        if inherit_from and inherit_from in self.sessions:
            self.sessions[session_id] = [self.system_prompt] + self.sessions[inherit_from][1:]
        else:
            self.sessions[session_id] = [self.system_prompt]

        self._save_session(session_id)
        return f"Session {session_id} Created."

    def list_sessions(self) -> list:
        return list(self.sessions.keys())

    def clear_session(self, session_id: str):
        if session_id not in self.sessions:
            return f"Session {session_id} Doesn't Exist!"
        self.sessions[session_id] = [self.system_prompt]
        self._save_session(session_id)
        return f"Session {session_id} Cleaned."

    def delete_session(self, session_id: str):
        if session_id in self.sessions:
            del self.sessions[session_id]
        path = self._get_session_path(session_id)
        if os.path.exists(path):
            os.remove(path)
        return f"Session {session_id} Deleted."

    def load_all_sessions(self):
        for fn in os.listdir(self.cache_dir):
            if fn.endswith(".json"):
                session_id = fn[:-5]
                try:
                    with open(self._get_session_path(session_id), "r", encoding="utf-8") as f:
                        self.sessions[session_id] = json.load(f)
                except Exception:
                    continue
        return f"Include {len(self.sessions)} Session(s)."

    # ==================== 核心聊天 ====================
    def chat(self, session_id: str, user_input: str) -> str:
        if session_id not in self.sessions:
            return f"Please create session {session_id} first."

        self.sessions[session_id].append({"role": "user", "content": user_input})

        try:
            completion = self.client.chat.completions.create(
                model=self.model,
                messages=self.sessions[session_id],
                max_completion_tokens=self.max_tokens,
                temperature=self.temperature,
                top_p=self.top_p,
            )

            res = completion.choices[0].message.content.strip()
            self.sessions[session_id].append({"role": "assistant", "content": res})
            self._save_session(session_id)
            return res

        except Exception as e:
            self.sessions[session_id].pop()
            return f"Error: {str(e)}"

    # ==================== 内部工具 ====================
    def _get_session_path(self, session_id: str):
        return os.path.join(self.cache_dir, f"{session_id}.json")

    def _save_session(self, session_id: str):
        path = self._get_session_path(session_id)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.sessions[session_id], f, ensure_ascii=False, indent=2)

    def dump_models_to_file(self, path: str = "models.txt"):
        with open(path, "w", encoding="utf-8") as f:
            for m in self.available_models:
                f.write(m + "\n")
        return f"Wrote {len(self.available_models)} models to {path}."


# ====================== 使用示例 ======================
if __name__ == "__main__":
    base_url, api_key, default_model = load_config()

    bot = MultiSessionChatter(
        base_url=base_url,
        api_key=api_key,
        default_model=default_model,
    )
    print(bot.load_all_sessions())
    print(f"Current model: {bot.current_model()}")

    print("\nUsage: ")
    print("  create [SessionID]                 新建会话")
    print("  create [NewID] [FromID]            继承历史新建会话")
    print("  chat [SessionID] [Message]         发消息（不带消息则弹窗输入）")
    print("  list                               列出所有会话")
    print("  clear [SessionID]                  清空会话（保留 system）")
    print("  delete [SessionID]                 删除会话")
    print("  model                              查看当前模型")
    print("  model list [keyword]               列出可用模型（可带关键词过滤）")
    print("  model <Name>                       切换模型（全局生效）")
    print("  models-dump                        把模型清单写入 models.txt")
    print("  quit / exit                        退出")

    while True:
        cmd = input("\n> ").strip()
        if not cmd:
            continue

        low = cmd.lower()

        if low in ("quit", "exit"):
            print("Exited.")
            break

        # ---- 会话列表 ----
        if cmd == "list":
            print("Session List: ", bot.list_sessions())
            continue

        # ---- 模型 ----
        if cmd == "model":
            print(f"Current model: {bot.current_model()}")
            print(f"Available models: {len(bot.list_models())} "
                  f"(use 'model list [keyword]' to view)")
            continue

        if cmd == "model list":
            for i, m in enumerate(bot.list_models(), 1):
                mark = " *" if m == bot.current_model() else ""
                print(f"  {i:>4}. {m}{mark}")
            continue

        if cmd.startswith("model list "):
            kw = cmd[len("model list "):].strip()
            results = bot.list_models(kw)
            if not results:
                print(f"No model matches '{kw}'.")
            else:
                for m in results:
                    mark = " *" if m == bot.current_model() else ""
                    print(f"  {m}{mark}")
            continue

        if cmd.startswith("model "):
            name = cmd[len("model "):].strip()
            print(bot.switch_model(name))
            continue

        if cmd == "models-dump":
            print(bot.dump_models_to_file())
            continue

        # ---- 会话操作 ----
        if cmd.startswith("create "):
            parts = cmd.split()
            if len(parts) == 2:
                print(bot.create_session(parts[1]))
            elif len(parts) == 3:
                print(bot.create_session(parts[1], inherit_from=parts[2]))
            else:
                print("Usage: create [SessionID] [InheritFrom?]")
            continue

        if cmd.startswith("clear "):
            print(bot.clear_session(cmd.split()[1]))
            continue

        if cmd.startswith("delete "):
            print(bot.delete_session(cmd.split()[1]))
            continue

        # ---- 聊天 ----
        if cmd.startswith("chat "):
            parts = cmd.split(maxsplit=2)
            if len(parts) < 2:
                print("Usage: chat [SessionID] {Message}")
                continue
            session = parts[1]
            if len(parts) > 2:
                print(bot.chat(session, parts[2]))
            else:
                text = inputBox("AI Chatter", "What do you want to say?")
                if text:
                    infoBox(bot.chat(session, text))
            continue

        print("Unknown Command.")