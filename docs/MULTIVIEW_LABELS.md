# 多切面征象标签定义

版本 0.2 · 送审稿 · 待临床负责人审定。随[研究协议](MULTIVIEW_EVALUATION.md)审阅。候选标签描述可见征象，待签核后用于参考读者校准。当前工作站保存确认帧、切面覆盖和已有征象复核任务的证据引用。以下参考标签及仲裁记录用于独立研究标注。

## 标注单位和字段

| 字段 | 记录口径 |
| --- | --- |
| `patient_id`、`site_code`、`exam_id` | 去标识患儿、采集中心和检查编号。 |
| `sign_code`、`dictionary_version` | 稳定征象代码及字典版本。变更时保留既有记录和转换依据。 |
| `reader_id`、`round`、`reference_source` | 读者代号、首阅或仲裁、参考资料及对应检查日期。两次首阅各自留存。 |
| `view`、`media_id`、`frame_index`、`timestamp_ms` | 已确认切面、原媒体、从 0 开始的源帧索引和原始时间。记录跨帧变化时保留起止帧。 |
| `adequacy` | `adequate` 表示目标结构充分显示，`limited` 表示局部可见但信息不足，`uninterpretable` 表示无法判断。逐征象记录。 |
| `stance` | `supporting` 和 `opposing` 均须带可回看的原始帧。缺失证据另列所需切面或目标结构及原因。 |
| `verdict`、`reason`、`adjudication` | 人工判读、不明确或未评估的原因，以及仲裁结论与依据。 |

`covered` 和 `missing` 描述所需切面是否有确认帧。可判读性单独记录。大模型复核的 `no_specific_finding` 不能作为参考阴性。

## 判读状态

| 状态 | 可复核判据 |
| --- | --- |
| `present` | 满足征象判据，至少记录一个支持帧。另列观察到的反对证据或缺失证据。 |
| `absent` | 所需切面与目标结构清楚可判读，读者明确判断未见该征象，记录可回看的依据。 |
| `indeterminate` | 已复核，但存在缺切面、结构不可判读、疑似伪影或未解决冲突。记录具体原因。 |
| `not_assessed` | 尚未复核、超出字典范围或参考资料缺失。记录原因，不进入参考阳性或阴性。 |

## 首批待审征象

| 代码 | 候选支持条件 | 反对证据或无法判断条件 |
| --- | --- | --- |
| `SEPTAL_CONTOUR_INTERRUPTION` | 同一解剖段在不同取向的确认切面可重复看到局部连续性异常。标记解剖段及支持帧。 | 改变取向后该处连续，或异常仅见于短暂脱落。无法区分伪影时记 `indeterminate`。 |
| `VALVE_FOCAL_ECHO` | 同一瓣区可重复看到有边界的局灶异常回声，运动片段中可定位与周围结构的关系。标记帧与切面。 | 回声随取向消失、与正常结构难以区分、边界无法确认或缺少必要运动片段。 |

审阅时逐项签核解剖范围、所需切面、最低显示条件、参考判据和边缘例。证据不足沿用 `insufficient_evidence`，分别记录 `coverage`、`quality` 或 `out_of_scope`。工作站不推导疾病候选。 [ASE 儿童经胸心超指南](https://www.asecho.org/wp-content/uploads/2024/02/2024-Peds-TTE_PIIS0894731723006223.pdf)
