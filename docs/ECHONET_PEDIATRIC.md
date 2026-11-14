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

预检检查每个 AVI 与对应 `FileList.csv`、原始 `Split` 值、视频解码和按呈现时间抽帧。用发布方的匿名患儿编号计算原始折号，并按第 0 次轮转验证训练、验证、测试分别为 6,354、798、658 段。报告按切面汇总原始 `Split`，保留患儿数和片段数。通过只读检查时退出码为 `0`，报告中的 `training_readiness` 列出开训缺项。 [分组算法](https://github.com/echonet/rv-internal/blob/master/scripts/process_pediatric.py#L152-L155) · [轮转划分](https://github.com/echonet/rv-internal/blob/master/scripts/cross_validate_pediatric.py#L20-L28)

斯坦福授权范围和患儿级隔离确认后，训练使用原始 `Split 2–9`，验证使用 `1`，独立测试保留 `0`。程序会再跑同一套预检，随后在 CUDA 上微调单分类头模型。测试命令见 [独立评估与 ONNX 核对](MODEL_EVALUATION.md)。

```powershell
& $python -X utf8 .\train_echonet_views.py --archive $archive `
  --report '.\work\echonet_views_preflight.json' --run-dir '.\runs\echonet-views-01' `
  --cache-dir '.\work\cache' `
  --stanford-authorization-confirmed --patient-grouping-confirmed
```

每轮结束后，程序保存 `last.pt`，其中包含最近一轮的模型、优化器、AMP scaler 和抽样状态。训练中断后，使用原命令和相同的 `--run-dir`、原始归档、抽样参数，再加 `--resume` 从最近完成的一轮继续。`best.pt` 始终按验证集 Macro-F1 选出，用于冻结和独立测试。

## 左室内膜轮廓分割

`train_stanford_lv.py` 使用 A4C 和 PSAX 发布视频中有有效左室描迹的帧，训练单帧 U-Net。按发布方加载器的规则，整数 `Frame` 直接作为从 0 开始的解码帧索引，按 CSV 原始顶点顺序在 112 × 112 图像上填充多边形。缺少描迹的标记和超出可解码范围的帧单独计数。 [发布方的数据加载器](https://github.com/echonet/rv-internal/blob/74e45522fc616a2591116b30cd3521ca0cfa2133/echonet/datasets/echo.py#L279-L339)

```powershell
& $python -m pip install scikit-image==0.26.0
$lvRun = '.\runs\stanford-lv-v1'
& $python -X utf8 .\train_stanford_lv.py train --archive $archive --run-dir $lvRun
& $python -X utf8 .\train_stanford_lv.py test --archive $archive `
  --checkpoint (Join-Path $lvRun 'best.pt') --report (Join-Path $lvRun 'test.json')
& $python -X utf8 .\train_stanford_lv.py predict --checkpoint (Join-Path $lvRun 'best.pt') `
  --video $video --frame $frameIndex --mask '.\work\left-ventricle-mask.png'
```

第 0 次轮转使用原始折 `2–9` 训练、`1` 验证、`0` 独立测试。使用验证集患儿平均 Dice 选择最佳 checkpoint，独立测试固定二值概率阈值 0.5。报告包含逐帧平均及患儿平均 Dice、IoU，并按切面列出结果和描迹覆盖。预测入口对 112 × 112 源视频的指定帧输出二值 PNG mask，轮廓对应左室内膜所包围的区域。

### 独立测试结果

2026-11-14 在 RTX 5060 Laptop 上完成 12 轮训练，最佳为第 8 轮（验证集患儿平均 Dice 0.9108）。锁定原始折 0 的独立测试折包含 186 名患儿、658 段视频和 1,298 个有描迹帧（A4C 530、PSAX 768）。帧级 Dice 0.9047、IoU 0.8323；患儿平均 Dice 0.9074、IoU 0.8362（A4C 0.8938 / 0.8147，PSAX 0.9151 / 0.8482）。按患儿有放回重抽样 1,000 次的 95% 区间：Dice 0.9012–0.9132、IoU 0.8271–0.8449。测试患儿与训练、验证患儿集合交集为 0。

权重、验证曲线、独立测试报告和接口测试记录通过 [stanford-lv-epoch8](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-lv-epoch8) 提供；接口约定与接口测试细节见 [Stanford 左室分割模型的接口与独立测试](STANFORD_LV_MODEL.md)。

完整心脏结构轮廓需要其他结构的空间标注。影像质量和异常征象各需医生确认的标签与适用范围，射血分数和描迹缺失保持其原有含义。

## 儿童心超专家需要确认

准备首批人工复核时，使用 [20 段人工复核流程](DOCTOR_REVIEW.md)。

- **切面名称与覆盖范围。** 确认 A4C 和 PSAX 在团队字典中的含义，尤其是 PSAX 涵盖哪些胸骨旁短轴层面。定义无法确定、非目标和扫查过渡如何处理。
- **质量标注。** 按切面和成像模式列出必须可见的结构，以及可评估、需补扫或无法评估的判定条件。标注区间内无法由单帧确定的现象保留原因，供后续时序任务使用。
- **本地验证病例。** 先由两位标注者独立复核同一小批去标识病例，再处理分歧。覆盖拟部署设备的灰阶和彩色多普勒，包含正常但采集不足、异常但采集充分、低质量与过渡片段。 [ASE 儿童经胸超声指南](https://www.asecho.org/guideline/comprehensive-pediatric-tte/)
