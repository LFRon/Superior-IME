# backend/speech-recognition

语音识别能力：麦克风采集 + 一次外部引擎调用。

- 目录：`$XDG_DATA_HOME/superior-ime/speech-recognition/{engine,models}`
- 引擎：外部可执行文件（默认 `crispasr`），可用 `SUPERIOR_ASR_ENGINE` 覆盖
- 协议：stdin/stdout 一行一个 JSON（见 `Protocol.hpp`）
- 不做决策：何时开始、要不要上屏由 agent 的 `core::SpeechState` 决定

它和 `backend/pinyin` 平级：一个能力一个模块，删除即卸载。
