# EchoFocus 基线评估

EchoFocus 是波士顿儿童医院 Cava Lab 与 CHAI Lab 发布的儿童心超方法，把 PanEcho 抽好的视频嵌入再过一个 Transformer 头，用注意力代替切面分类，做整次检查级的预测。本仓库把它作为**外部基线**引入：它代表"先抽嵌入、再做检查级聚合"这一路线的公开参考实现，用来和本地 MobileNetV3-Small 端到端模型区分开。

- 论文：Lukyanenko P, Ghelani S, … La Cava WG, Mayourian J. *Automated Echocardiographic Detection of Congenital Heart Disease Using Artificial Intelligence.* medRxiv 2026. doi:10.64898/2026.01.24.26344771（预印本，未经同行评审）。只作为方法来源引用，不作为性能依据。
- 上游：`https://github.com/cavalab/echofocus`（代码 AGPLv3，权重 CC BY-NC-SA 4.0），依赖 `https://github.com/CarDS-Yale/PanEcho`（代码 AGPLv3，权重 CC BY-NC-SA 4.0）。
- 许可影响：**仅限非商业研究**。派生权重必须同样按 CC BY-NC-SA 4.0 发布，署名两家实验室；本仓库不重新分发其权重，推理时从 Releases 之外的授权位置读取。

## 与本仓库模型的关系

| | EchoFocus | 本仓库主模型 |
| --- | --- | --- |
| 输入 | PanEcho 视频嵌入（每段 16 clip × 16 帧 = 256 次 ConvNeXt-T 前向） | 原始帧，端到端 |
| 参数量 | Measure 头 17.75 M + PanEcho 约 30 M | MobileNetV3-Small 量级 |
| 切面处理 | 不做切面分类，注意力自行加权 | 切面识别后分流 |
| 推理形态 | 仅 CUDA | ONNX / TensorRT FP16 |
| 可比性 | 只在同一切分、同一标签口径下可比 | — |

两者的差距主要在**上游嵌入**，不在头部：EchoFocus 的 3.56–17.75 M 参数 Transformer 头本身很轻，成本全在 PanEcho 的 256 次前向。因此把它作为基线，同时也在量化"冻结嵌入 + 轻头"这条路线值不值。

## 实测：三个必须知道的坑

以下均在 Windows x64、Python 3.13、CPU 上复现。

1. **代码硬性要求 CUDA**：`echofocus.py` 构造时若无 CUDA 直接 `raise ValueError('No CUDA. Exiting.')`，且 `save_nn` 无条件调用 `torch.cuda.get_rng_state()`。CPU 复现需要给这两处加保护。
2. **权重以 CUDA tensor 保存**：`torch.load` 不带 `map_location` 会在 CPU 上抛 `RuntimeError: Attempting to deserialize object on a CUDA device`。转存一份 CPU 版即可，数值不变。
3. **`saveout_*.csv` 的列名和内容相反**：`_evaluate` 把 `run_model_on_dataloader` 的返回值解包成 `y_true, y_pred`，而该函数返回顺序是「模型输出, 真值」。结果 `*_Correct` 列是预测值、`*_Predict` 列是真值。用这个 CSV 算 MAE 不受影响，做阈值或 AUC 分析会出错。另有两处：`evaluate()` 缺 `self.split` 会直接报错，默认 `encoder_depth=0` 的训练路径因 `MultiheadAttention` 调用缺参而不可用。

## 实测：跨域零样本（不是同条件对比）

用 CAMUS 测试集 30 段视频喂 `EchoFocus_Measure` 的 EF05 头，标签为该数据集的报告 EF：

| n = 30 段 | MAE | 偏差 | r | EF<50 AUROC |
| --- | ---: | ---: | ---: | ---: |
| EchoFocus_Measure（零样本） | 40.99 | −40.99 | −0.346 | 0.654 |

偏差 −41 表示它对几乎所有输入都输出约 5 % 的 EF（真实均值 46 %）。这是**分布外失效**，不能读作"EchoFocus 不准"：它训练于本院的儿童数据，从未见过成人、另一套机器的图像。此行的意义是划清边界——外部基线不能直接拿来当对照，必须重训或至少重新校准。

**数据口径注意**：本仓库 CAMUS 文档使用的是 Hugging Face `MedOtter/CAMUS`（500 名病人 × 2 切面 × ED/ES 帧）；上述实测使用 `zeahub/camus` 的 HDF5 镜像，该镜像只有 **247 名病人、494 个文件**（每名 2CH + 4CH 各一个）。两者患者集合不同，数字不可直接相加或对比。

## 若要作为正式基线

按 [AGENTS.md](../AGENTS.md) 的评估纪律，需要：

1. 固定同一批 EchoNet-Pediatric 或 CAMUS 视频、同一患者级切分、同一标签来源。
2. 在同一批样本上跑本仓库模型和 EchoFocus，报告 MAE、偏差、r 与 EF<50 的 AUROC，并注明样本量与统计单位（视频/患儿）。
3. 记录权重版本与 PanEcho 版本，以及预处理差异（EchoFocus 用 16 帧 clip、ImageNet 归一化、224 px 中心裁剪）。
4. 性能数字进入 `runs/` 的评估记录并在 [MODEL_EVALUATION.md](MODEL_EVALUATION.md) 登记来源，不写进代码注释。

在完成上述步骤前，本文件不声明任何"EchoFocus 优于/劣于本仓库模型"的结论。

## 复现入口

EchoFocus 侧的准备（需自行放置权重，不进 Git）：

```powershell
$python = '.\.venv\Scripts\python.exe'
git clone https://github.com/cavalab/echofocus.git .\work\echofocus
git clone https://github.com/CarDS-Yale/PanEcho.git .\work\PanEcho
# panecho.pt 与 EchoFocus_Measure/best_checkpoint.pt 放到 work\models\ 下，均不入库
```

CPU 复现需先按上面第 1、2 条打补丁并把权重转存为 CPU 版；GPU 机器上可直接按上游 README 运行。
