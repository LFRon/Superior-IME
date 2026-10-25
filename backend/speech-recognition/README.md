# backend/speech-recognition

语音识别能力：麦克风采集 + 一次外部引擎调用。

- 目录：`$XDG_DATA_HOME/superior-ime/speech-recognition/{engine,models}`
- 引擎：外部可执行文件（默认 `crispasr`），可用 `SUPERIOR_ASR_ENGINE` 覆盖
- 协议：stdin/stdout 一行一个 JSON（见 `Protocol.hpp`）
- 不做决策：何时开始、要不要上屏由 agent 的 `core::SpeechState` 决定

它和 `backend/pinyin` 平级：一个能力一个模块，删除即卸载。

## 语音门控（VAD）

引擎按 2 s 步长把整个滚动窗口重新解码，窗口里只有静音或底噪时，Paraformer 依然会"流畅地"编出文字（`没有没有没有…`、`eeee零零零…`）。这些文本会被当作识别结果上屏，也就是"静音时疯狂打字"。所以启动引擎时带上 `--vad --vad-model <权重>`：静音只上报 `silence`，每段 utterance 按语音边界裁剪。

两个条件同时成立才会加这两个参数（见 `Engine.cpp` 的 `appendVadArgs`）：

- **引擎认识 `--vad`**：启动前用 `--help` 探测一次并缓存结果。旧引擎收到未知参数会直接退出，那等于所有会话都不出声。
- **权重在本地**：优先 `models/vad/ggml-silero-v6.2.0.bin`，其次引擎自己的缓存 `~/.cache/crispasr/ggml-silero-v6.2.0.bin`，可用 `SUPERIOR_ASR_VAD_MODEL` 覆盖。

只给 `--vad` 而不给 `--vad-model` 时引擎会自行从 huggingface.co 下载权重，而下载失败**不会**退回"不做门控"——它把每次录音都判成静音，语音输入彻底不出字。所以权重由 `scripts/install-speech-engine.sh` 显式安装（0.9 MB，sha256 固定），和模型、标点模型一样不由运行时偷偷拉取。
