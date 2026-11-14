# CAMUS 多结构分割与影像质量分级

CAMUS（Cardiac Acquisitions for Multi-structure Ultrasound Segmentation）是公开的 2D 经胸心超数据集，500 名病人、每名 2CH 与 4CH 各一次检查，提供手工标注的 ED/ES 帧、四类结构掩膜、专家影像质量分级和射血分数。本仓库接入它补齐两项能力：多结构分割（LV 心内膜、LV 心肌、左房）和影像质量三分类。数据源与许可见 [公开心超数据源调研](PUBLIC_DATASETS.md)，EchoNet-Pediatric 上的左室分割与 EF 见 [左室分割](STANFORD_LV_MODEL.md) 与 [射血分数](STANFORD_EF.md)。

## 数据与预处理

- 来源：Hugging Face `MedOtter/CAMUS`（CC BY-NC-SA 4.0，直链，无需审批），`ed_es` 配置共 2,000 行 = 500 名病人 × 2 切面 × ED/ES 帧。
- 划分：发布方划分，训练 400 名病人 / 1,600 帧、验证 50 名 / 200 帧、测试 50 名 / 200 帧，患者两两不交叉。
- 标签：掩膜取值 0 背景、1 LV 心内膜、2 LV 心肌、3 左房；`image_quality` 为 Good/Medium/Poor 的视位级专家评级；`ef` 为参考射血分数。
- 预处理：灰度图缩放到 256 × 256（双线性），掩膜用最近邻缩放；像素 /255。

先把 parquet 转成 npy 数组：

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\prepare_camus.py --parquet-dir '.\data\camus\parquet' --output-dir '.\work\camus'
```

输出 `images.npy`（2,000 × 256 × 256 uint8）、`masks.npy`、`meta.json`。

## 多结构分割

单帧 4 类 U-Net（编码 16/32/64/96 通道），BCE 换为交叉熵加逐类 Dice，AdamW 1e-3 余弦退火、batch 8、AMP、20 轮。按验证集前景患儿平均 Dice 选取最佳轮次（第 19 轮）。

```powershell
& $python -X utf8 .\train_camus.py train --task seg --data-dir '.\work\camus' --run-dir '.\runs\camus-seg-v1'
& $python -X utf8 .\train_camus.py test --task seg --data-dir '.\work\camus' `
  --checkpoint '.\runs\camus-seg-v1\best.pt' --report '.\runs\camus-seg-v1\test.json'
```

锁定测试集（50 名留出病人、200 帧）结果：

| 结构 | 帧级 Dice | 帧级 IoU | 患儿平均 Dice | 患儿平均 IoU |
| --- | ---: | ---: | ---: | ---: |
| LV 心内膜 | 0.9275 | 0.8673 | 0.9275 | 0.8673 |
| LV 心肌 | 0.8553 | 0.7515 | 0.8553 | 0.7515 |
| 左房 | 0.8835 | 0.8072 | 0.8835 | 0.8072 |
| 前景平均 | 0.8888 | 0.8086 | 0.8888 | 0.8086 |

测试集每名病人正好 4 帧（2 切面 × ED/ES），所以患儿平均与帧级平均相同作为聚合口径的核对。

## 影像质量三分类

MobileNetV3-Small（ImageNet 初始化）输入 224 × 224 灰度复制三通道，输出 Good/Medium/Poor。训练使用类别加权交叉熵、标签平滑、随机水平翻转与亮度/对比度扰动；15 轮（v1）与 25 轮（v2）两版，按验证集患者-视位 macro-F1 选取最佳轮次。

```powershell
& $python -X utf8 .\train_camus.py train --task quality --data-dir '.\work\camus' --run-dir '.\runs\camus-quality-v1'
& $python -X utf8 .\train_camus.py test --task quality --data-dir '.\work\camus' `
  --checkpoint '.\runs\camus-quality-v1\best.pt' --report '.\runs\camus-quality-v1\test.json'
```

锁定测试集（50 名病人 × 2 切面 = 100 个患者-视位，200 帧）：

| 版本 | 患者-视位准确率 | 患者-视位 macro-F1 | 95% 区间 | 帧级准确率 | 帧级 macro-F1 |
| --- | ---: | ---: | --- | ---: | ---: |
| v1（无增强，15 轮） | 0.50 | 0.504 | — | 0.52 | 0.526 |
| v2（增强 + 标签平滑，25 轮） | 0.63 | 0.591 | 0.466–0.692 | 0.615 | 0.570 |

v2 逐类召回：Good 0.511、Medium 0.895、Poor 0.333。混淆矩阵（行=真实，列=预测，Good/Medium/Poor）：`[[24, 23, 0], [3, 34, 1], [0, 10, 5]]`。

影像质量是可分级主观标签，Poor 类样本少（测试 15 个患者-视位），模型对 Good 与 Poor 的区分仍是弱项；这一结果只能说明公开数据上可训练出有效信号，不能当作临床质量判定。

## 限制

- CAMUS 是成人队列（18–93 岁，中位 67），迁移到儿童心超必须单独验证；本轮训练结果只代表成人数据。
- 质量标签是视位级序数评级，不是逐帧或诊断级质量判断；测试阳性类少。
- 结构掩膜覆盖 LV 心内膜、LV 心肌、左房；右心与其他结构需额外标注。
- 结果为研究用途，不构成临床诊断或临床性能结论。
