# 公开心超数据源调研

2026-11-15 对可用的公开心超数据集做了一轮调研，筛选标准：允许非商业科研使用、无需人工审批、有直接下载链接。结论如下。

## 已接入

| 数据 | 来源与许可 | 内容 | 用途 |
| --- | --- | --- | --- |
| EchoNet-Pediatric | Stanford，需在 Stanford AIMI 签署研究使用协议后获取；本机归档 7,810 段 | A4C / PSAX 灰阶视频；EF、左室描迹、患儿分折 | 切面、左室分割、EF 回归 |
| CAMUS | [HF MedOtter/CAMUS](https://huggingface.co/datasets/MedOtter/CAMUS)，CC BY-NC-SA 4.0，直链下载，聚合 SHA-256 `abcdaa86…60936a2` | 500 名病人、1,000 次 2CH/4CH 检查、2,000 张手工标注 ED/ES 帧；掩膜 4 类（背景/LV 心内膜/LV 心肌/左房）；`image_quality` 为 Good/Medium/Poor 专家分级；EF 5–81 | 多结构分割、影像质量分级、跨数据源对照 |
| CardiacUDC | [HF MedOtter/CardiacUDC](https://huggingface.co/datasets/MedOtter/CardiacUDC)，`annotated` 配置 178 MB，直链下载，单文件 SHA-256 `c286113c…499d7`；许可口径见下 | 293 段 A4C 视频、2,257 张手工标注帧；掩膜 5 类（背景/LV/LA/RA/RV）；两家医院 Site G / Site R | 四腔结构分割、跨院点对照 |

CAMUS 的预处理、训练与结果见 [CAMUS 多结构分割与影像质量](CAMUS.md)，CardiacUDC 见 [CardiacUDC 四腔分割](CARDIACUDC.md)。引用要求：

> S. Leclerc, E. Smistad, J. Pedrosa, A. Ostvik, et al. "Deep Learning for Segmentation using an Open Large-Scale Dataset in 2D Echocardiography" IEEE Transactions on Medical Imaging, vol. 38, no. 9, pp. 2198-2210, Sept. 2019. doi: 10.1109/TMI.2019.2900516

> J. Yang, X. Ding, Z. Zheng, X. Xu, X. Li. "GraphEcho: Graph-Driven Unsupervised Domain Adaptation for Echocardiogram Video Segmentation", ICCV 2023, pp. 11878-11887.

## CardiacUDC 许可口径

该 HF 再发布仓库标注不一致：数据卡 front-matter 写 `cc-by-nc-4.0`，README 正文与上游 Kaggle 记录声明 Apache License 2.0。本仓库按更严格口径处理，用于非商业研究：

- 遵守 CC BY-NC 4.0 的署名与非商业限制，只做非商业科学研究；
- 不再分发原始数据；只发布本机训练得到的权重与汇总指标，权重同样限定非商业研究用途；
- 若后续需要商业或再分发用途，应回到上游作者与再发布方确认 Apache-2.0 是否适用并取得书面许可。

## 未接入（审批制或无直链）

| 数据 | 状态 | 说明 |
| --- | --- | --- |
| EchoNet-Dynamic | 审批制 | Stanford 通过 Redivis 分发，需账号与使用条款；对儿童任务不是必需 |
| Tufts TMED / TMED-2 | 无直链 | 官网只提供联系作者获取；若开放可作切面与病理标签补充 |
| ACNET 2024 | 未找到 | 官方渠道与 Zenodo/HF 均未检索到可下载的公开版本 |
| CETUS | 可下载但与任务不符 | 3D/4D 心超左室分割，无影像质量标签，不适用于 2D 儿童模型 |

## 结论

- 结构分割与影像质量：CAMUS 是当前唯一同时提供多结构掩膜和质量金标准、且可直接下载的数据集；CardiacUDC 补上四腔（LV/LA/RA/RV）标注。两者都是成人队列，迁移到儿童必须单独验证。
- 异常征象：公开数据没有儿科征象金标准；目前可量化的是 EchoNet-Pediatric 的 EF（收缩功能异常），其余征象需要团队专家标注。
- 儿童数据缺口：要补 0–18 岁真实队列只能通过 EchoNet-Pediatric 的授权渠道或团队 DICOM 标注，本轮保持该缺口并记录在案。
