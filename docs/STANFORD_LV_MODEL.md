# Stanford 左室分割模型的接口与独立测试

`train_stanford_lv.py` 在 Stanford EchoNet-Pediatric 发布数据上训练单帧左室心内膜腔分割模型，并提供训练、独立测试和单帧预测三个入口。本页记录模型接口、接口测试记录、本轮训练配置和锁定独立测试折的评分结果。数据来源与使用条件见 [EchoNet-Pediatric 训练准备](ECHONET_PEDIATRIC.md)。[Stanford EchoNet-Pediatric 项目页](https://echonet.github.io/pediatric/)

## 模型与接口

模型输入单帧 112 × 112 灰度图像，像素值除以 255 后送入单帧 U-Net（编码通道 16/32/64/96，4 级下采样与跳连，1 × 1 输出）。输出为 112 × 112 前景 logits，logits ≥ 0 判为左室心内膜腔，对应发布方 `VolumeTracings.csv` 描迹所包围的区域。

| 项目 | 约定 |
| --- | --- |
| 输入 | 视频解码单帧，灰阶或可转灰阶，尺寸必须为 112 × 112 |
| 预处理 | 单通道，像素值 / 255 |
| 输出 | 新建 PNG，112 × 112 二值掩膜（0 或 255） |
| 判定 | 固定 logits ≥ 0（sigmoid 概率 0.5），训练验证、独立测试和预测入口一致 |
| 设备 | `--device cuda` 或 `--device cpu`；`cuda` 需要本机可用的 CUDA PyTorch |
| 依赖 | `torch`、`av`、`Pillow`、`numpy`、`scikit-image` |

三个入口均从仓库根目录运行，`$python` 指向 README 环境要求中配置的 Python，`$archive` 指向原始 ZIP：

```powershell
& $python -X utf8 .\train_stanford_lv.py train --archive $archive --run-dir '.\runs\stanford-lv-v1'
& $python -X utf8 .\train_stanford_lv.py test --archive $archive `
  --checkpoint '.\runs\stanford-lv-v1\best.pt' --report '.\runs\stanford-lv-v1\test.json'
& $python -X utf8 .\train_stanford_lv.py predict --checkpoint '.\runs\stanford-lv-v1\best.pt' `
  --video $video --frame $frameIndex --mask '.\work\left-ventricle-mask.png'
```

`predict` 成功时不输出 stdout 文本，只写入新的 PNG mask；失败时以退出码 1 结束并给出中文错误信息。`test` 会核对 checkpoint 中的训练划分、输入尺寸、目标结构和阈值，与当前协议不一致时拒绝加载。

## 接口测试记录

2026-11-14 在本机 Windows 11 x64、Python 3.13.13、torch 2.12.1+cu130、RTX 5060 Laptop 8 GB 上，用本轮 checkpoint（第 8 轮，SHA-256 `883cf3e3…7894ea`）对独立测试折 0 的一段 A4C 视频（`CR32a7559-CR32a7586-000045.avi`，第 16 帧有发布方描迹）实测：

| 用例 | 预期 | 实测 | 结果 |
| --- | --- | --- | --- |
| 正常预测 | 退出码 0，生成 112 × 112 二值 PNG | 退出码 0，掩膜 112 × 112，取值 {0, 255} | 通过 |
| 与发布方描迹帧对比 | 掩膜与描迹帧一致 | 单帧 Dice 0.8733、IoU 0.7751 | 通过 |
| 目标 mask 路径已存在 | 拒写并报错 | 退出码 1，`ValueError: 模型输出需要一个新的 PNG 路径` | 通过 |
| 输入不是 112 × 112 | 报错 | 退出码 1，`ValueError: 模型输入需要 112×112 像素` | 通过 |
| 指定帧不存在 | 报错 | 退出码 1，`ValueError: 指定的源视频帧不存在` | 通过 |

单帧 Dice 与独立测试折整体（0.9047）存在单帧采样波动，模型精度以锁定测试折的逐帧统计为准。

## 训练配置

| 项目 | 本轮实际值 |
| --- | --- |
| 数据 | EchoNet-Pediatric 原始归档 7,810 段 AVI（A4C 3,284 段、PSAX 4,526 段） |
| 划分 | 第 0 次轮转：训练原始折 2–9，验证 1，独立测试 0 |
| 样本 | 训练 6,354 段 / 12,503 个描迹帧；验证 204 名患儿 / 1,570 帧；测试 186 名患儿 / 1,298 帧 |
| 输入 | 112 × 112 灰度，像素 / 255 |
| 模型 | 单帧 U-Net，编码通道 16/32/64/96，参数约 0.4M，checkpoint 1.57 MB |
| 损失 | BCEWithLogits + Soft Dice |
| 优化 | AdamW，学习率 1e-3 余弦退火到 1e-4，batch 32，AMP 混合精度，12 轮，seed 42 |
| 选择 | 按验证集患儿平均 Dice 选最佳轮次，最佳为第 8 轮（验证患儿平均 Dice 0.9108） |
| 环境 | Windows 11 x64、Python 3.13.13、torch 2.12.1+cu130、CUDA 13.0、PyAV 19.0.2、RTX 5060 Laptop 8 GB |
| 耗时 | 数据解码加 12 轮训练约 8 分钟（GPU 训练 6–7 分钟） |

## 独立测试结果

锁定原始折 0，测试患儿与训练、验证患儿集合两两交集为 0。阈值固定 0.5，逐帧计算 Dice = |P∩T| × 2 / (|P|+|T|)、IoU = |P∩T|/|P∪T|。

| 分组 | 帧数 | 患儿数 | 帧级 Dice | 帧级 IoU | 患儿平均 Dice | 患儿平均 IoU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 全部 | 1,298 | 186 | 0.9047 | 0.8323 | 0.9074 | 0.8362 |
| A4C | 530 | 151 | 0.8918 | 0.8125 | 0.8938 | 0.8147 |
| PSAX | 768 | 184 | 0.9137 | 0.8460 | 0.9151 | 0.8482 |

按患儿有放回重抽样 1,000 次的 95% 区间：

| 分组 | Dice 95% 区间 | IoU 95% 区间 |
| --- | --- | --- |
| 全部（186 名患儿） | 0.9012 – 0.9132 | 0.8271 – 0.8449 |
| A4C（151 名患儿） | 0.8851 – 0.9013 | 0.8033 – 0.8250 |
| PSAX（184 名患儿） | 0.9082 – 0.9211 | 0.8372 – 0.8574 |

描迹覆盖：A4C 271/271 段有描迹（No Systolic 7、No Diastolic 3、帧号超出可解码范围 2）；PSAX 387/387 段有描迹（No Systolic 1、No Diastolic 2、帧号超出可解码范围 3）。

同一 checkpoint 经独立实现（自写数据解码、掩膜栅格化、模型前向和逐帧/聚合指标，未复用训练入口的评估函数）重算：12 项指标最大绝对差 1.76e-5，属 CUDA float32 批大小与内存布局造成的数值差异；帧数、患儿数、覆盖计数逐项一致，患儿级隔离通过。

## 限制

- 发布方描迹只覆盖左室心内膜腔。完整心脏结构轮廓需要其他结构的空间标注。
- 独立测试折是数据集内患儿级留出（原始折 0），不是合作医院或部署设备的外部测试，跨院点泛化尚未验证。
- 影像质量和异常征象没有发布方金标准标签，本模型不输出这两类结果。
- 结果为研究用途，不构成临床诊断或临床性能结论。

## Release 与附件

本轮的权重、验证曲线、独立测试报告和接口测试记录通过 [stanford-lv-epoch8](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-lv-epoch8) 提供，供有私有仓库访问权限的成员用于非商业研究。
