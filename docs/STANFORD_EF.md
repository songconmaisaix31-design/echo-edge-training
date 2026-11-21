# Stanford EchoNet-Pediatric 射血分数回归与异常收缩功能

`train_stanford_ef.py` 用 EchoNet-Pediatric 发布视频的 EF 标签训练视频级射血分数回归模型，并把预测 EF 低于阈值的情况作为异常收缩功能的筛查候选。切面与结构模型见 [EchoNet-Pediatric 训练准备](ECHONET_PEDIATRIC.md)，左室分割见 [Stanford 左室分割模型的接口与独立测试](STANFORD_LV_MODEL.md)。

## 数据与协议

- 数据：EchoNet-Pediatric 原始归档中的 A4C 视频，共 3,284 段，使用发布方第 0 次轮转（训练 Split 2–9、验证 1、独立测试 0，患者级隔离）。
- 标签：`FileList.csv` 的 `EF` 列，每个片段一个 EF（百分比）。
- 输入：每段视频按呈现时间在整段上等分抽 8 帧，等比缩放补边到 224 × 224 RGB，ImageNet 均值方差归一化。
- 模型：MobileNetV3-Small（ImageNet 初始化）提取逐帧特征，帧特征取平均后接线性回归头，输出 EF 百分比；约 94 万参数、3.83 MB。
- 损失与优化：SmoothL1、AdamW（学习率 3e-4，余弦退火）、AMP 混合精度、batch 8 段、12 轮、seed 42。
- 选择：按验证集患儿平均绝对误差（MAE）选择最佳轮次，本轮最佳为第 5 轮。

## 独立测试结果

测试折 0：271 段 A4C 视频、151 名患儿，阈值与轮次在训练期锁定后只跑一次。

| 口径 | MAE | RMSE | Pearson | R² |
| --- | ---: | ---: | ---: | ---: |
| 片段级 | 4.69 | 6.55 | 0.664 | 0.428 |
| 患儿级（先按患儿平均） | 3.80 | 5.08 | 0.689 | 0.470 |

异常收缩功能（正类 = 真实 EF 低于阈值，分数 = 负的预测 EF，阈值用真值定义）：

| 阈值 | 阳性患儿 | AUROC | 解释 |
| --- | ---: | ---: | --- |
| EF < 45% | 4 例阳性 | 0.991 | 探索性结果 |
| EF < 50% | 5 例阳性 | 0.996 | 方向性结果 |
| EF < 55% | 12 例阳性 | 0.810 | 探索性结果 |

[第 5 轮独立测试报告](https://github.com/songconmaisaix31-design/echo-edge-training/releases/download/stanford-ef-epoch5/Stanford-EF-test.json)

以预测 EF < 50% 判为异常时：敏感度 0.60、特异度 1.00（阳性仅 5 例，区间很宽）。阳性样本少，AUROC 只能作为方向性证据，不能当作临床筛查性能。

## 命令

```powershell
$python = '.\.venv\Scripts\python.exe'
$archive = 'C:\path\to\EchoNet - Pediatric_files.zip'
$cache = '.\work\cache'
$run = '.\runs\stanford-ef-v1'
& $python -X utf8 .\train_stanford_ef.py train --archive $archive --cache-dir $cache --run-dir $run
& $python -X utf8 .\train_stanford_ef.py test --archive $archive --cache-dir $cache `
  --checkpoint (Join-Path $run 'best.pt') --report (Join-Path $run 'test.json')
& $python -X utf8 .\train_stanford_ef.py predict --checkpoint (Join-Path $run 'best.pt') `
  --video '.\data\sample.avi'
```

第一次运行会在缓存目录生成 `a4c-ef-frames.npy`（约 3.95 GB，3,284 段 × 8 帧 × 224 × 224 × 3 通道）和同名 JSON 清单；之后复用缓存。`predict` 输出视频级 EF 与抽样帧数。

## 限制

- 只用了 A4C 片段；EF 标签来自发布方，未与超声报告独立核对。
- 异常阈值 45/50/55% 只用于研究报告，儿童 EF 正常范围随年龄和体表面积变化，临床阈值需医生确认。
- 患儿级结果只说明数据集内留出患者的泛化，不代表外部院点或部署设备。
- 结果为研究用途，不构成临床诊断。
