# CardiacUDC 四腔分割

CardiacUDC（上游论文称 CardiacUDA）是公开的 2D 心尖四腔心超视频分割数据集，来自两家医院（Site G / Site R），共 293 段带标注的 A4C 视频、2,257 张手工标注帧，掩膜覆盖 LV / LA / RA / RV 四个腔室。本仓库用它把结构能力从 CAMUS 的「LV 心内膜 + LV 心肌 + 左房」扩展到四腔。数据源与许可口径见 [公开心超数据源调研](PUBLIC_DATASETS.md)。

## 许可口径

该再发布仓库存在许可标注不一致：HF 数据卡 front-matter 写 `cc-by-nc-4.0`，README 正文与上游 Kaggle 记录声明 Apache License 2.0。本仓库按更严格口径处理：只做非商业科学研究，遵守 CC BY-NC 4.0 的署名与非商业限制；不再分发原始数据，只提供本机训练权重与汇总指标；权重同样限定非商业研究用途。商业或再分发用途需回到上游作者与再发布方确认 Apache-2.0 适用范围并取得书面许可。

引用：J. Yang, X. Ding, Z. Zheng, X. Xu, X. Li. "GraphEcho: Graph-Driven Unsupervised Domain Adaptation for Echocardiogram Video Segmentation", ICCV 2023, pp. 11878-11887.

## 数据与划分

- 来源：Hugging Face `MedOtter/CardiacUDC` 的 `annotated` 配置，178 MB 单文件，SHA-256 `c286113c…499d7`；293 段视频、2,257 帧、原始分辨率 800 × 600（另有少量 1024 × 768 和 640 × 480）。
- 类别：掩膜取值 0 背景、1 LV、2 LA、3 RA、4 RV（注意 2 是左房，RV 是 4）。
- 划分：数据只发布一个 `train` split，本仓库按 `group_id` 自行划分，固定 seed 42、按站点与标注密度分层：训练 201 组 / 1,593 帧、验证 42 组 / 297 帧、测试 43 组 / 367 帧。`label_all_frame` 的 10 段密集标注视频中有 8 段与 `Site_R_126` 视频是近重复录制，`group_id` 已把它们绑定到同一 split，避免测试泄漏。
- 预处理：灰度缩放到 256 × 256（双线性），掩膜最近邻缩放，像素 /255。原始 parquet 带去除头部标识的遮罩带（发布方处理），不影响标注区域。

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\prepare_cardiacudc.py `
  --parquet '.\data\cardiacudc\annotated\train-00000-of-00001.parquet' --output-dir '.\work\cardiacudc'
```

## 训练

单帧 5 类 U-Net（编码 16/32/64/96 通道），交叉熵 + 逐类 Dice，AdamW 1e-3 余弦退火、batch 8、AMP、20 轮；按验证集前景组级平均 Dice 选最佳轮次（第 18 轮，1.57 MB）。

```powershell
& $python -X utf8 .\train_cardiacudc.py train --data-dir '.\work\cardiacudc' --run-dir '.\runs\cardiacudc-fourchamber-v1'
& $python -X utf8 .\train_cardiacudc.py test --data-dir '.\work\cardiacudc' `
  --checkpoint '.\runs\cardiacudc-fourchamber-v1\best.pt' --report '.\runs\cardiacudc-fourchamber-v1\test.json'
```

## 独立测试结果

测试折 43 组 / 367 帧，帧级 Dice/IoU 与组级宏平均：

| 腔室 | 帧级 Dice | 帧级 IoU | 组级宏平均 Dice | 组级宏平均 IoU |
| --- | ---: | ---: | ---: | ---: |
| LV | 0.8243 | 0.7238 | 0.7192 | 0.6388 |
| LA | 0.8279 | 0.7363 | 0.7318 | 0.6594 |
| RA | 0.8841 | 0.8124 | 0.7576 | 0.6979 |
| RV | 0.8420 | 0.7425 | 0.7114 | 0.6253 |
| 前景平均 | 0.8446 | 0.7538 | 0.7300 | 0.6553 |

按站点拆分（帧级前景 Dice / 组级宏平均）：

| 站点 | 帧数 | 组数 | 前景帧级 Dice | 前景组级宏平均 Dice |
| --- | ---: | ---: | ---: | ---: |
| Site G | 135 | 22 | 0.8330 | 0.7152 |
| Site R | 172 | 20 | 0.8587 | 0.7413 |
| 站点未标注 | 60 | 1 | 0.8304 | 0.8304 |

组级指标低于帧级，是因为密集标注的 `label_all_frame` 组帧数多（每段 47–69 帧）而稀疏组只有 5–8 帧，组级宏平均对小样本组更敏感。RA 最好、RV 最弱，与右心边界在四腔切面内对比度低一致。

## 限制

- 这是成人/混合队列（两家医院），迁移儿童需单独验证；只有 A4C 一个切面。
- 部分视频每段只标注 5–8 帧，帧级指标受标注帧分布影响；组级宏平均与帧级平均需一起看。
- 数据只发布单一 train split，本仓库的划分是按 `group_id` 的分层划分，不是发布方官方测试集。
- 许可按更严格口径（CC BY-NC 4.0）处理，仅限非商业研究；权重不随原始数据再分发。
- 结果为研究用途，不构成临床诊断。
