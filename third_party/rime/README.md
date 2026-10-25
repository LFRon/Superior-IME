# rime_api.h

Vendored copy of the public C API header from
[rime/librime](https://github.com/rime/librime).

| 项目 | 值 |
|---|---|
| 文件 | `src/rime_api.h` |
| 上游版本 | 1.17.0（与本机 `librime.so.1` 一致） |
| 许可 | BSD-3-Clause（`Copyright RIME Developers`，保留在原文件头部） |
| 大小 | ~20 KB，单文件，无依赖 |

## 为什么只 vendor 这个头

librime 对外**只导出两个符号**：

```
$ nm -D /lib/x86_64-linux-gnu/librime.so.1 | grep ' T rime_get_api'
00000000000e5090 T rime_get_api
00000000000f27a0 T rime_get_api_stdbool
```

其余全部通过 `RimeApi` 函数表访问。所以编译只需要这一个头文件；链接期连
`-lrime` 都不需要（运行期 `dlopen("librime.so.1")`），也不需要 `librime-dev`，
更不必把 17 MB 源码树拖进仓库。

`RimeTraits` / `RimeContext` / `RimeMenu` 等结构体都带 `data_size` 版本位
（`RIME_STRUCT_INIT`），所以运行期能校验 ABI，升级 librime 不会静默读错字段。

## 升级方式

```bash
curl -o third_party/rime/rime_api.h \
  https://raw.githubusercontent.com/rime/librime/1.17.0/src/rime_api.h
```

升级前先确认 `nm -D` 仍只导出那两个符号；若上游新增了导出符号，说明 ABI 策略变了，
需要重读这一节的假设。
