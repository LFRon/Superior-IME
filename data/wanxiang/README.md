# 万象拼音方案（随仓库分发）

`rime-wanxiang-pure.zip` 是**万象拼音 Pure** 的上游发布包，原样收录，未做任何修改。

| 项 | 值 |
|---|---|
| 方案 | 万象拼音 Pure（Rime 方案 id：`wanxiang_pure`） |
| 版本 | `18.1.7` |
| 上游 | <https://github.com/amzxyz/rime-wanxiang> |
| 作者 | amzxyz 及贡献者 |
| 许可 | **CC-BY-4.0** <https://creativecommons.org/licenses/by/4.0/> |
| 归档 SHA-256 | `0025b1ca084bfd7f4d766662050a89b62a9d21b1b8b19c623edc931cb25cf986` |
| 归档大小 | 26 MB（解压后 77 MB） |

上游的 `README.md`（含许可声明）就在归档内部，`scripts/fetch-wanxiang-schema.sh`
部署时会一并解压到 `$XDG_DATA_HOME/superior-ime/pinyin/shared/README.md`，
所以终端用户那份数据目录里也带有完整的署名信息。

## 为什么是归档而不是解压好的文件

- **一个 blob 而不是 35 个文件**：解压后是 77 MB 的 YAML 文本，进 git 会让每次
  clone 都多背 77 MB，且文件树 diff 噪音极大。归档 26 MB，且以后永不变化。
- **校验和仍然有意义**：`fetch-wanxiang-schema.sh` 里的 SHA-256 描述的正是这个文件。
  原样收录意味着「钉住的校验和」与「仓库里的字节」是同一件事，
  不会出现「仓库里的是 A，脚本期望的是 B」这种漂移。
- **解压是部署步骤，不是分发步骤**：librime 需要磁盘上的明文 YAML，所以
  `fetch-wanxiang-schema.sh` 在部署时解压，并顺手做两件必要的事：去掉未启用的
  `grammar:` 段（否则 octagram 每次建会话都要找 399 MB 的模型），
  以及写入本产品的 `default.custom.yaml`（关闭 librime 自己的 ascii 切换）。

## 它怎么被使用

- `scripts/install-user.sh` 装完后会自动跑一次
  `scripts/fetch-wanxiang-schema.sh --offline`：**装完即可打字**，不需要联网，
  也没有「装好了但要再执行一条命令」这一步。
- 手动重跑是幂等的：已部署且版本一致时只打印一行说明就退出。
- `--force` 才是「忽略本归档、从上游下载」，留给归档过期的场合；
  下载分支同样校验 SHA-256，不匹配拒绝部署。
- `ctest -R bundled-schema` 校验本文件的字节与脚本里的 `sha256` 一致，
  防止「归档换了、钉子没换」。

## 更新步骤

1. 从上游取新版本，**先看 diff**：词库决定每个候选长什么样，换版本等于换产品行为。
2. 换掉 `data/wanxiang/rime-wanxiang-pure.zip`，同步更新
   `scripts/fetch-wanxiang-schema.sh` 里的 `version` 与 `sha256`（以及本文件的表格）。
3. 更新 `tests/golden/pinyin.txt`（候选顺序可能变），跑
   `ctest -R 'golden-pinyin'`。
4. 说明为什么升：版本号本身不是理由。

## 许可义务

CC-BY-4.0 允许再分发，条件是**署名**、**附许可链接**、**标明是否修改**。本项目：

- 保持归档原样（未修改），因此不需要标注改动；
- 署名与许可见本节、仓库根 `README.md`、`docs/pinyin-engine.md`；
- 不把词库内容并入本项目的 GPL-3.0-or-later 代码部分，两者许可见
  [`LICENSE`](../../LICENSE)。

商业使用前请自行复核上游许可；本文件不是法律意见。
