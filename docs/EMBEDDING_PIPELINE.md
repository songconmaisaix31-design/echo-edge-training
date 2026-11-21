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

自备视频时，按视频目录下的相对路径提供标签。文件名在目录内唯一时也可用作标签键。

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

在本机用公开 CAMUS（494 段视频 / 247 名病人，成人）跑通两阶段，非本仓库授权的儿童数据。目的不是给出性能结论，而是确认管线可用、量出成本结构、并把阶段二做成可复核的工具。

**阶段一**：494 段。CPU 8 clip：35 分钟；GPU 16 clip：11 分钟。

**阶段二**：头参数 0.9 M，150 轮约 80 秒。官方测试集 100 段 / 50 名病人，训练与选型均未见过。

8 clip，三个随机种子：

| | MAE（EF 百分点） | 偏差 | Pearson r | EF<50 AUROC |
| --- | ---: | ---: | ---: | ---: |
| 常数基线（训练均值） | 8.36 | −3.78 | — | 0.50 |
| 冻结 PanEcho + 注意力头（未校准） | 7.13 ± 0.16 | −2.51 | 0.574 | 0.719 |
| **同上，验证集仿射校准后** | **6.86 ± 0.18** | −1.93 | 0.574 | 0.719 |

- **校准值钱**：偏差从 −2.5 收到 −1.9，MAE 再降 0.27。校准斜率只在验证集上拟合，测试集从未参与。
- **患儿级 4 折交叉验证**（训练池内）：MAE 7.83 ± 0.41，说明这个量级不是单次划分的偶然。
- **超参网格**（3 折，80 轮）：dim 128/1 8.41、256/1 8.24、**256/2 8.19**、384/2 8.47。默认即最优，无需调。
- **clip 数量不是越多越好**：16 clip 在 CAMUS 上反而更差（校准后 7.06 ± 0.43 vs 8 clip 的 6.86 ± 0.18），因为 15–30 帧的环里 16 个 clip 高度重叠，只增加了冗余 token。EchoNet 那种长视频才需要 16 个。

同一批数据、同一切分下，端到端从头训练的小骨干（MobileNetV4-conv-small，2.66 M，8 帧 @160px）为 MAE 11.87、r 0.048、AUROC 0.48——**不如常数基线**。两者差距不是调参问题，是"有没有可用的预训练表示"。

由此可以定下这条流程的定位：

- **它解决的是小数据下的表示问题，不是精度天花板。** r 0.57 说明信号真实存在，但 MAE 6.9 仍明显弱于本仓库在 Stanford 数据上的患儿级 MAE 3.80（见 [STANFORD_EF.md](STANFORD_EF.md)）。两者数据与口径不同，不可直接比较，仅说明"这条路不是精度升级"。
- **真正的收益是迭代速度。** 编码只做一次；此后换标签、换切分、换头结构都是分钟级，这正好接上 [EF 复测协议](EF_REVIEW_PROTOCOL.md)——120 例复核结果回来时可以立刻重训，不必重新编码。
- **区间比点估计重要**：患儿级 bootstrap 的 95% 区间在 6.2–9.2 之间，任何"MAE 6.9" 的对外表述都必须带上这个区间与样本量。

### 注意力的实测表现（未达预期）

`--dump-attention` 能导出每个视频的片段权重，但在 CAMUS 上这些权重**几乎均匀**：100 段测试视频里，最大权重平均只有 0.135（均匀分布是 0.125），绝大多数视频的最大值与最小值差不到 0.01。

也就是说，这个头退化成接近均值池化，**注意力目前不提供可解释性**。原因有三：CAMUS 每个视频只有 15–30 帧，8 个 clip 高度重叠，本来就没有可区分的片段；任务信号弱；也没有加熵正则。

机制是通的，结论还没拿到。只有在真正有多片段、多切面的长视频（例如 EchoNet-Peds 的完整检查）上重跑，才能判断注意力是否会对齐到有诊断价值的片段。在那之前，不要把"可解释注意力"写进任何对外说明。

复现：

```powershell
python -X utf8 train_emb_head.py --emb '.\work\emb' --run-dir '.\runs\emb-head-v2' `
  --epochs 150 --folds 4 --bootstrap 1000 `
  --report '.\runs\emb-head-v2\test.json' `
  --predictions '.\runs\emb-head-v2\predictions.json' `
  --dump-attention '.\runs\emb-head-v2\attention.json'
python -m pytest test_emb_head.py -q
```

