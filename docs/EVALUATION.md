# 心超模型独立评估

从仓库根目录运行 `evaluate_echo.py`。使用训练生成的 `best.pt`，并保持类别顺序、抽帧参数及患儿划分与训练一致。`$python` 指向 [README 中配置的 Python](../README.md#环境要求)。评估复用现有的 DICOM／视频解码、时间分箱、等比缩放和 RGB 归一化，以 FP32 推理。CPU 为默认设备，RTX 5060 可用 `--device cuda`。报告写入新的 JSON 文件。

## 团队双头模型

使用训练时的完整标注工作簿。视频清单按患儿明确指定 `train`、`val`、`test`，同一患儿的其他视频可以继承该分组。命令只读取测试组中带已确认区间的影像。验证集确定阈值，`$viewThreshold`、`$qualityThreshold` 填锁定后的数值。将 `$acceptableCode` 和 `$lowQualityCodes` 填为临床确认的质量类别代码。

```powershell
& $python -X utf8 .\evaluate_echo.py team --media $media --labels $labels `
  --checkpoint $checkpoint --device cuda --report '.\work\team-test.json' `
  --view-threshold $viewThreshold --quality-threshold $qualityThreshold `
  --acceptable-quality $acceptableCode --low-quality $lowQualityCodes
```

训练使用自定义 `--sample-fps` 或 `--max-frames-per-segment` 时，评估传入相同值。当前入口使用带抽帧配置和实际患儿划分的 checkpoint，由本仓库更新后的训练入口写入。

## EchoNet-Pediatric 检查与评估

对本机 ZIP 复算发布方的患儿分折。用原始 `Split` 2–9 作为训练、1 作为验证、0 作为测试的只读核对命令如下。它只检查清单、分组、解码和选帧数量。

```powershell
& $python -X utf8 .\evaluate_echo.py stanford --archive $archive `
  --train-splits 2 3 4 5 6 7 8 9 --val-splits 1 --test-splits 0 `
  --audit-only --report '.\work\stanford-audit.json'
```

正式评分使用相同的分组参数，将 `--audit-only` 换成 `--stanford-authorization-confirmed --checkpoint $checkpoint --view-threshold $viewThreshold`，另指定新的报告路径。训练与验证折必须和 checkpoint 一致。

## 报告读法

- 每个已确认区间或 Stanford 视频先对选中帧的 logits 求平均，再做 softmax。低于该任务固定阈值时拒判。混淆矩阵列末尾的 `null` 表示拒判，召回的分母包含拒判。
- 各任务只统计自身有确认标签的片段。覆盖率是给出类别的片段比例。低质量误放行率的分母为标为指定低质量类别的片段，分子为其中被预测为可用的片段。
- 按患儿有放回抽样 1,000 次计算百分位 95% 置信区间，保留同一患儿的全部片段。分母为零时指标为 `null`。少于两个可计算该指标的患儿时区间为 `null`。置信区间是该批数据的抽样估计。
- 团队报告按设备、区间成像模式和院点分组，缺少信息的组记为 `null`。另列在训练和验证清单中未出现的测试院点代码。

## 本机只读验证

原始 Stanford 测试折核对通过，包含 186 个去标识患儿编号、658 段视频，解码 71,299 帧、选中 5,200 帧。7,810 条清单记录的分折核对通过，跨组患儿为 0。

公开彩色 DICOM 通过真实时间抽帧、RGB 归一化和官方 ImageNet 权重前向检查。公开样本的工程工作簿被正式评估入口判定为缺少有效标签。训练预检也识别出另一个工程工作簿里的患儿分组冲突。

团队正式评分等待对应 checkpoint 和确认后的测试标注。Stanford 正式评分等待对应 checkpoint 和使用确认。
