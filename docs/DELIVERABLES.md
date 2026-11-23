# 可交付标准

本文件定义"什么算做完"。每一条判据都是二值的、可复核的；`scripts/check_deliverables.py` 能机器检查的部分会自动检查，其余在授权环境内人工核对并留下记录。

## 三级放行

| 级别 | 含义 | 前置 |
| --- | --- | --- |
| **L1 内部可引用** | 团队内部可以引用这些数字做决策 | D0 + D1 + D2 + D7 |
| **L2 可对外发布** | 可以写进公开文档、演示、对外材料 | L1 + D3 + D6 |
| **L3 研究投稿 / 临床研究** | 可以进入论文、多中心研究、临床研究流程 | L2 + D4 + D5 + 前瞻性或外部中心验证 |

任何一级未达成时，对外材料只能写"进行中"，不能写数字。

## D0 · 数据与许可台账

**交付物**：每个用到的数据集在 [PUBLIC_DATASETS.md](PUBLIC_DATASETS.md) 与 [TRAINING_DATA.md](TRAINING_DATA.md) 有来源、许可、口径和统计单位；仓库不含任何患者数据。

**判据**

- [ ] 每个数据集记录了来源链接、许可证名称、获取方式（直链 / DUA / 授权）
- [ ] 权重与数据的非商业声明在 README 与相关文档可见
- [ ] `git ls-files` 中不存在影像、嵌入、检查表、工作簿、权重
- [ ] 受限资产（EchoNet 系列）只记录口径，不记录路径清单

## D1 · 编码产物

**交付物**：`embed_panecho.py` 对全量归档跑完，产出可复用的 `(n_clips, 768)` 缓存与索引。

**判据**

- [ ] 全量完成，索引条目数 = 归档视频数
- [ ] 标签校验 0 条错误（`embedding_labels.py` 的 subject / ef / split 三项）
- [ ] 训练、验证、测试的患儿交集为 0；按中心隔离时中心交集也为 0
- [ ] 记录设备、精度（fp16/fp32）、每段耗时、总耗时
- [ ] 产物路径、命名规则、可否断点续跑写在文档里

## D2 · 阶段二性能报告

**交付物**：`train_emb_head.py` 在冻结嵌入上的测试结果，进入 `docs/EMBEDDING_PIPELINE.md`。

**判据**（缺一项即未完成）

- [ ] 表格含：数据集、训练/验证/测试的**视频数**与**患儿数**
- [ ] 主指标 MAE，单位写明"EF 百分点"
- [ ] 同时给出 Pearson r 与 EF<50 的 AUROC，以及阳性例数
- [ ] 给出常数基线（训练均值）的同一批指标
- [ ] 给出患儿级 bootstrap 的 95% 区间
- [ ] 给出校准前后两组数字，并说明校准只在验证集拟合
- [ ] 给出随机种子数与跨种子波动（至少 3 个种子）或患儿级 K 折均值±标准差
- [ ] 写明测试集从未参与训练与选型
- [ ] 附重现命令与运行日期

**为什么这条最严**：没有它，前面两条只是"跑通了"，不是"有结果"。

## D3 · 标签可信度（EF 复测）

**交付物**：[EF_REVIEW_PROTOCOL.md](EF_REVIEW_PROTOCOL.md) 冻结并执行完毕。

**判据**

- [ ] 协议与标注手册在判读开始前定稿，判据 S1–S4 预先写死
- [ ] 120 例主样本完成，`scripts/ef_review.py check` 对每批退出码 0
- [ ] 40 例洗脱重测完成，`repeat_of` 映射存在
- [ ] 报告 Q1（报告 vs 复测）、Q2（模型 vs 复测）、Q3（自身重测）三项，各带 n 与区间
- [ ] 明写"单判读者、无观察者间一致性"，结论表述为"与一位专家的一致性"
- [ ] 冻结文件的 SHA-256 记录在案

## D4 · 外部基线对照

**交付物**：EchoFocus（BCH）在同一批测试数据、同一标签口径、同一指标下的结果。

**判据**

- [ ] 同一测试集、同一标签来源、同一统计单位
- [ ] 先报告预测分布（均值、标准差），方差接近 0 时只报失效、不参与排序
- [ ] 与本地模型并列呈现，标注"零样本外部方法，未在本院数据训练"
- [ ] 权重许可证与署明写清楚（CC BY-NC-SA 4.0 / AGPLv3）
- [ ] 不把 EchoFocus 当作参考标准，也不用于生成伪标签

## D5 · 多切面独立评价

**交付物**：按 [MULTIVIEW_EVALUATION.md](MULTIVIEW_EVALUATION.md) v0.2 执行独立评价。

**判据**

- [ ] 协议、字典、统计计划签核，版本与签核编号可回查
- [ ] `protocol-approval.json` 与 `data-authorization.json` 齐备且在有效期内
- [ ] 三件冻结输入（队列 / 参考 / 预测）齐备，`freeze_id` 与授权对应
- [ ] `evaluate_multiview.py` 退出码 0，报告落在受管目录之外
- [ ] 报告含各征象敏感度/特异度、`insufficient_evidence` 比例、覆盖率与分母

## D6 · 对外宣称门禁

**交付物**：README 与对外材料不越界。

**判据**

- [ ] `.github/workflows/redlines.yml` 在 main 上为绿
- [ ] 模型卡（适用人群、输入要求、已知失效场景、非器械声明）存在并被 README 引用
- [ ] 所有性能数字都能指到本文件某一条判据的证据位置
- [ ] 未使用 `rules/banned-claims.txt` 中的表述（对应 issue #37–#42）

## D7 · 工程验收

**判据**

- [ ] `npm --prefix frontend run typecheck && lint && build` 通过
- [ ] `python -m pytest backend -q` 通过
- [ ] `npm --prefix frontend test` 通过
- [ ] `scripts/verify.mjs` 退出码 0，报告写入 `work/verify/report.json`
- [ ] 团队数据训练前 `test_split_preflight.py` 预检退出码 0

## D2 怎么关掉（具体登记步骤）

这一条是当前两块未达标判据之一。按仓库既有的指标登记方式，缺的不是数字，是把数字**接进追溯链**：

1. 把 `train_emb_head.py` 产出的 `test.json` 作为 Release 资产上传（例如 tag `emb-head-v1`）。
2. 在 `rules/metrics.json` 的 `reports` 增加一条，`kind` 为 `project`，`release` 与 `report` 指向该资产。
3. 在 `docs/EMBEDDING_PIPELINE.md` 写出结果表格（列：数据集、视频数 / 患儿数、MAE、偏差、r、AUROC + 阳性数、校准前后、常数基线、种子数或折）。
4. 在 `rules/metrics.json` 的 `claims` 增加该表格对应的条目：`path`、`text`（表行原文）、`headers`、`reports`（第 2 步的键）。
5. 运行 `python -X utf8 scripts/check_redlines.py` 与 `python -X utf8 scripts/check_deliverables.py`，两者退出码均为 0。

第 2、4 步是仓库的既有约定：没有登记的指标数字会被红线检查拦下，这正是它该有的行为。

## 当前状态

2026-11-22 复核（`python -X utf8 scripts/check_deliverables.py`）：**通过 5 / 待人工 3 / 未达标 0**。

| 判据 | 状态 | 依据 / 差距 |
| --- | --- | --- |
| D0 | 通过 | 仓库跟踪 669 个文件，无影像、权重、数据库与检查表；数据源文档记录许可 |
| D1 | 通过 | 阶段一全量编码完成并记录设备、精度、耗时；标签校验与患儿交集检查有据 |
| D2 | 通过 | 结果表进入 [EMBEDDING_PIPELINE.md](EMBEDDING_PIPELINE.md)；报告资产见 Release `emb-head-camus-v1`；`rules/metrics.json` 登记 2 个 reports 与 4 条 claims |
| D3 | 未开始 | 协议、手册与 `scripts/ef_review.py` 已合并；判读按负责人安排在 D3 之前暂不启动 |
| D4 | 通过 | 同一批嵌入上运行 EchoFocus 测量头并登记；按判据先看预测分布，标准差比值过低即判失效、不参与排序 |
| D5 | 进行中 | 协议 0.2 送审、`evaluate_multiview.py` 就绪；等机构签核与冻结输入 |
| D6 | 通过 | 红线检查退出码 0；[模型卡](MODEL_CARD.md) 已建并被 README 引用 |
| D7 | 部分通过 | 后端测试、类型检查、Lint、构建通过；浏览器验收见 `work/verify/report.json` |

按三级放行：

- **L1（内部可引用）**：D0、D1、D2 已通过，D7 待浏览器验收收尾 → **实质达成**。
- **L2（对外发布）**：L1 之外差 D3。按负责人决定，D3 暂不启动，因此对外材料仍只写"进行中"。
- **L3（研究投稿 / 临床研究）**：L2 之外差 D5 与前瞻性或外部中心验证。

结论：**D2 与 D4 已关闭**，当前对外的阻塞点是 D3（人工判读，暂缓）。

### 本轮关闭 D2 / D4 的证据

```text
Release      emb-head-camus-v1
资产         EmbeddingHead-CAMUS-summary.json（3 个种子 + 校准 + 患儿级区间）
             echofocus_camus.json（同一批嵌入上的零样本外部头）
登记         rules/metrics.json → reports: emb-head-camus, echofocus-camus
                               → claims: 4 行表格
命令         python -X utf8 train_emb_head.py --emb work/emb --run-dir runs/rel_seedN
             python -X utf8 evaluate_echofocus.py --emb work/emb --echofocus-repo ... --checkpoint ...
门禁         python -X utf8 scripts/check_redlines.py        → 0
             python -X utf8 scripts/check_deliverables.py    → 0
```

## 维护

判据只增不减；放宽任何一条须在同一提交内写明原因、影响范围和补做计划。机器可查的条目由 `scripts/check_deliverables.py` 在 CI 或本地执行，见该脚本头部的用法。

