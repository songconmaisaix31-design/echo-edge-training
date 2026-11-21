# 两阶段流程：冻结编码 + 轻量注意力头

把建模拆成两段：先让 PanEcho 把每个视频冻成一组向量，再在向量上训练一个很小的注意力头。这样做的直接好处是**换标签不用重新编码**——EF 复核协议里那 120 例一旦回来，重训只需要秒级，不用再等编码。

## 为什么这样分

| | 阶段一：编码 | 阶段二：训练头 |
| --- | --- | --- |
| 输入 | 原始视频 | `(n_clips, 768)` 数组 |
| 参数更新 | 无（完全冻结） | 只更新头，约 0.9 M |
| 成本 | 编码一次，后续训练复用缓存 | CAMUS 缓存训练一轮少于 1 秒 |
| 产物 | `work/emb/*.npy` | `runs/emb-head-*/best.pt` |
| 失败影响 | 编码器不合格 → 后面全白做 | 只影响当前标签与切分 |

编码器是外部资产（Yale CarDS 的 PanEcho），本仓库不训练也不分发它；阶段二才是我们自己的东西。这个边界让"换了基线"和"换了标签"都不会互相污染。

## 与 EchoFocus 的关系

结构同源：不做切面分类、把整段检查的所有片段当序列、Transformer 编码后池化、线性头回归。差别在三点：

1. **池化方式**：EchoFocus 用均值池化；这里用显式注意力池化，权重可以打印出来看它更关注哪些片段。
2. **训练数据**：EchoFocus 在本院数据上训练；这里用本仓库自己的标签重训，权重不共享。
3. **定位**：EchoFocus 作为外部方法对照出现在评估协议里，不作为参考标准，也不生成伪标签。

权重许可证为 CC BY-NC-SA 4.0，代码 AGPLv3；派生权重需同协议发布。详见 [ECHOFOCUS.md](ECHOFOCUS.md)。

## 成本实测

在本机（Windows x64、RTX 5060 Laptop 8G、cu128 torch 2.9）上，PanEcho 编码一个视频：

| 每视频 clip 数 | CPU（16 线程） | GPU | 说明 |
| --- | ---: | ---: | --- |
| 8 | 4.3 秒 | 0.35 秒（纯计算） | CAMUS 这类 15–30 帧的短环已够用 |
| 16 | 8.6 秒 | 约 1.3 秒（含 HDF5 解码与预处理） | EchoFocus 的原设置 |

494 段 CAMUS 实测：8 clip 用 CPU 35 分钟；16 clip 用 GPU 11 分钟，其中解码与预处理占比不小。

## 数据边界

- 视频与嵌入都留在授权环境：`work/emb/` 与 `work/echo-index-private.json` 已被 `.gitignore` 排除。
- `index.json` 含 `subject`（患儿号），只能留在 `work/`；对外只发布聚合后的报告。
- PanEcho 权重从 Releases 之外的授权位置读取，通过 `--weights` 指向，不进仓库。

## 命令

### EchoNet-Pediatric 原始归档

阶段一直接读取官方 ZIP 的 A4C 和 PSAX 视频。每条嵌入的 `subject` 来自发布方文件名中的匿名患儿编号，`ef` 来自该视频所在切面的 `FileList.csv`，单位为百分比。按发布方第 0 次轮转划分，原始折 2–9 用于训练，折 1 用于验证，折 0 用于测试。输出索引和数组存放在本机忽略目录。

```powershell
$python = '.\.venv\Scripts\python.exe'
$archive = 'C:\path\to\EchoNet - Pediatric_files.zip'
$emb = '.\work\pediatric-emb'
$run = '.\runs\pediatric-emb-head'

& $python -X utf8 .\embed_panecho.py `
  --pediatric-archive $archive --weights '.\work\models\panecho.pt' `
  --panecho-repo '.\work\PanEcho' --out $emb --clips 16 --device cuda --fp16 `
  --report (Join-Path $run 'encode.json')

& $python -X utf8 .\train_emb_head.py --emb $emb --run-dir $run `
  --device cuda --epochs 200 --report (Join-Path $run 'test.json')
```

中断后用同一归档、权重、精度、片段数和输出目录重试。FP16 用于编码计算，保存的嵌入为 float32。阶段二保留归档指定的验证集，发现无效的 `subject` 或 `ef`、索引与数组不一致、同一患儿跨集合时终止。

### 本地视频与标签表

自备视频时，按视频目录下的相对路径提供标签。文件名在目录内无重复时也可用作标签键。

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\embed_panecho.py `
  --weights '.\work\models\panecho.pt' --panecho-repo '.\work\PanEcho' `
  --video-dir '.\data\videos' --index '.\work\echo-index-private.json' `
  --out '.\work\emb' --clips 8 --report '.\runs\embed\summary.json'
```

`--index` 的格式为 `{"相对路径.avi": {"subject": "P0001", "split": "train", "ef": 55.0}}`。给已缓存的视频补齐或更新标签时，用同样的编码参数和输出目录重跑，阶段一更新对应记录并复用现有数组。只提取嵌入时可以省略 `--index`。

阶段二（训练头 + 报告）：

```powershell
& $python -X utf8 .\train_emb_head.py `
  --emb '.\work\emb' --run-dir '.\runs\emb-head-v1' --epochs 200 --device cuda `
  --report '.\runs\emb-head-v1\test.json'
```

阶段二需要训练集与独立测试集。没有指定 `val` 时，从训练集按 `subject` 划出验证集。报告记录各集合的视频数与患儿数、MAE（EF 百分点）、常数基线，以及 EF<50 的 AUROC 与阳性例数。验证集 MAE 最低的模型保存为 `best.pt`。按 [AGENTS.md](../AGENTS.md)，只有报告里的数字可以对外引用。

## 验证记录

### EchoNet-Pediatric 两阶段运行

2026-11-21 使用 RTX 5060 Laptop GPU 和 PyTorch `2.9.0+cu128` 处理原始归档。A4C 3,284 段，PSAX 4,526 段，共 1,939 名患儿。FP16 编码用时 1,997.7 秒，每段保存为 `16 × 768` 的 float32 数组。逐条核对 `subject`、`ef` 和原始折，缺失或不匹配 0 条，训练、验证和测试集的患儿交集为 0。

| 原始折 | 视频数 | 患儿数 |
| --- | ---: | ---: |
| 训练 2–9 | 6,354 | 1,549 |
| 验证 1 | 798 | 204 |
| 测试 0 | 658 | 186 |

阶段二按验证误差选择权重，并在验证集拟合仿射校准。EF 参考值来自归档 `FileList.csv`，测试使用原始留出折。本次嵌入实验的测量结果保存在本地评估报告中。

### CAMUS 两阶段运行

已用公开的成人 CAMUS 影像验证编码与嵌入头的运行流程。编码产物可供不同的标签、分组和头结构复用。阶段二可在验证集拟合仿射校准，在测试集报告误差及不确定性。`--dump-attention` 保存模型片段权重，权重与临床证据的对应关系仍需独立评价。相关实验结果保存在本地报告中。

复现：

```powershell
python -X utf8 train_emb_head.py --emb '.\work\emb' --run-dir '.\runs\emb-head-v2' `
  --epochs 150 --folds 4 --bootstrap 1000 `
  --report '.\runs\emb-head-v2\test.json' `
  --predictions '.\runs\emb-head-v2\predictions.json' `
  --dump-attention '.\runs\emb-head-v2\attention.json'
python -m pytest test_emb_head.py -q
```

