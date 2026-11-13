# EchoNet-Pediatric 训练准备

EchoNet-Pediatric 的发布资料包括心尖四腔（A4C）、胸骨旁短轴（PSAX）、左室射血分数和左室描迹。切面可以作为研究用切面监督的候选。采集质量头需要团队确认的质量标注。用射血分数、描迹是否存在或视频能否解码代替质量标签，会改变训练目标。 [数据集说明与研究协议](https://echonet.github.io/pediatric/)

## 数据准备

1. 确认下载时接受的条款和斯坦福书面授权覆盖计划中的本地处理和训练。若涉及团队共同使用、对外演示、发布权重或产品用途，分别确认对应许可。 [Stanford AIMI 商业许可](https://aimi.stanford.edu/datasets-commercial-use)
2. 保留原始 ZIP。记录归档版本、两类切面的数量、CSV 与视频关联、可解码帧数及呈现时间戳。将描迹缺失或索引异常记录在质检报告中。需要使用描迹时，先核对帧编号基准和缺失标记，再建立任务对应的排除规则。
3. 保留来源中的 `Split` 数值及其出处。用发布方确认的患者分组说明或允许使用的匿名分组字段核对患儿是否跨组。文件名唯一和文件内容不同只说明视频级差异。患者级隔离未核实时，评估结果只能按已知划分层级解释。 [EchoNet-Pediatric 原论文](https://doi.org/10.1016/j.echo.2023.01.015)
4. 单切面任务使用 `train_echonet_views.py` 直接读取 AVI，用 A4C 和 PSAX 两个发布类别训练一个分类头。正式双头模型的数据要求见 [训练数据交付说明](TRAINING_DATA.md)。

## 只读预检

在仓库根目录用 [README](../README.md) 配好的 Python 环境执行。将 `$archive` 设为原始 ZIP 的实际路径。

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\train_echonet_views.py --archive $archive `
  --report '.\work\echonet_views_preflight.json' --preflight-only
```

预检检查每个 AVI 与对应 `FileList.csv`、原始 `Split` 值、视频解码和按呈现时间抽帧。报告按两类切面汇总原始 `Split`，数据错误会定位到文件和 CSV 行。通过只读检查时退出码为 `0`，报告中的 `training_readiness` 仍会列出开训缺项。

斯坦福授权范围和患儿级隔离确认后，由团队确定训练与验证使用哪些原始 `Split`。把已确认的原始数字分别放入 `$trainSplits` 和 `$valSplits` 数组。两个数组不得重叠，未选中的分组保留，不参与训练和验证。程序会再跑同一套预检，随后在 CUDA 上微调单分类头模型，输出 `best.pt`、验证 Macro-F1 和混淆矩阵。

```powershell
& $python -X utf8 .\train_echonet_views.py --archive $archive `
  --report '.\work\echonet_views_preflight.json' --run-dir '.\runs\echonet-views-01' `
  --cache-dir '.\work\cache' --train-splits $trainSplits --val-splits $valSplits `
  --stanford-authorization-confirmed --patient-grouping-confirmed
```

## 儿童心超专家需要确认

准备首批人工复核时，使用 [20 段人工复核流程](DOCTOR_REVIEW.md)。

- **切面名称与覆盖范围。** 确认 A4C 和 PSAX 在团队字典中的含义，尤其是 PSAX 涵盖哪些胸骨旁短轴层面。定义无法确定、非目标和扫查过渡如何处理。
- **质量标注。** 按切面和成像模式列出必须可见的结构，以及可评估、需补扫或无法评估的判定条件。标注区间内无法由单帧确定的现象保留原因，供后续时序任务使用。
- **本地验证病例。** 先由两位标注者独立复核同一小批去标识病例，再处理分歧。覆盖拟部署设备的灰阶和彩色多普勒，包含正常但采集不足、异常但采集充分、低质量与过渡片段。 [ASE 儿童经胸超声指南](https://www.asecho.org/guideline/comprehensive-pediatric-tte/)
