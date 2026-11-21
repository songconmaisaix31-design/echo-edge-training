# 独立多切面评价与验收

[研究协议](MULTIVIEW_EVALUATION.md)和[征象标签定义](MULTIVIEW_LABELS.md) v0.2 等待机构审定。评价工具读取授权目录中的冻结检查清单、人工仲裁结果和模型预测记录。模型推理、源影像解码和训练由各自的流程负责。

## 审定与授权

机构数据管理员核验正式签核文件后，在受管主机上设置 `ECHO_MULTIVIEW_REVIEW_ROOT` 的绝对路径，并在研究 ID 子目录下登记下列记录。根目录、研究目录、配置与冻结文件应由机构控制写入，评价账号只获得读取权限。签核编号须能回查机构原件，程序依据该目录的访问控制、记录内容和关联编号执行门禁。统一由机构管理员配置入口环境和目录权限。

| 文件 | 必填字段和口径 |
| --- | --- |
| `protocol-approval.json` | `status` 为 `approved`，登记 `record_id`、`protocol_version`、`dictionary_version`、`approved_on`、`clinical_signoff_id`、`statistics_signoff_id`、`reference_review_signoff_id`、`model_version`、`threshold_version` 和 `analysis_version`。当前支持协议及字典版本 `0.2`、分析版本 `1`。 |
| `protocol-approval.json` 中的统计参数 | `signs` 以获批的字典征象代码为键，每个征象的 `positive`、`negative` 为冻结的最低参考数。`age_bands`、`device_groups` 为签核分组名称列表。`bootstrap_seed`、`bootstrap_repetitions`、`minimum_centers` 来自正式签核的统计计划。 |
| `data-authorization.json` | `status` 为 `authorized`，`purpose` 为 `independent_multiview_evaluation`，登记 `record_id`、对应的 `protocol_approval_id`、`cohort_id`、`cohort_freeze_id`、`references_freeze_id`、`predictions_freeze_id` 及 `effective_on`、`expires_on`。 |

日期使用 `YYYY-MM-DD`。缺少任一记录、格式或版本不合规、授权与审定记录不对应，或者授权尚未生效或已到期，命令返回退出码 `1`，评估输入和报告不会被打开。

## 冻结输入和结果

三个输入均为 UTF-8 JSON 对象。公共字段为 `cohort_id`、`protocol_version`、`dictionary_version`、`freeze_id`、`rows`。队列与冻结编号须与数据授权相符。

| 文件 | `rows` 口径 |
| --- | --- |
| `cohort.json` | 按检查登记 `exam_id`、`patient_id`、`site_code`、`enrollment_site`、`split`、`age_band`、`device`、`adequacy`、`planned_signs`、`required_views`、`media` 和 `confirmed_frames`。提交实际生效的 `train`、`val`、`test` 分组元数据。`planned_signs` 记录该测试检查冻结计划中的获批征象，训练和验证检查填空列表。每位患儿的重复检查使用同一纳入中心作为统计重抽单位的归属。年龄、设备分层使用冻结的分组名称。 |
| `references.json` | 按测试检查和征象登记 `exam_id`、`sign_code`、`state`、`adequacy`、`adjudication_id`、`supporting`、`opposing`、`missing_views`。参考状态为 `present`、`absent`、`indeterminate` 或 `not_assessed`。后两项须填写 `reason`。 |
| `predictions.json` | 文件顶层登记获批的 `model_version`、`threshold_version`。按测试检查和征象登记 `exam_id`、`sign_code`、`state`、`supporting`、`opposing`、`missing_views`。保留 `present`、`absent`、`insufficient_evidence`、`failed`。`reason` 对拒判使用 `coverage`、`quality` 或 `out_of_scope`，对执行失败使用 `execution`。 |

`media` 按 `media_id` 保存源帧的时间偏移列表，以毫秒计并从首帧的零时刻开始。`confirmed_frames` 每项记录 `view`、`media_id`、`frame_index` 和 `timestamp_ms`。支持帧与反对帧使用相同的媒体编号、源帧索引及时间戳。切面沿用工作站的切面代码。有计划征象的检查须填写必要切面。判为阳性的记录须有支持帧，阴性记录须有反对帧。必要切面缺失时，模型应记录 `insufficient_evidence`，人工参考可保留无法判断的原因。发现患儿或中心跨组、同一媒体跨检查、帧引用越界、重复记录或测试计划外的结果时，命令以非零状态退出。

在仓库根目录使用已安装后端依赖的 Python 运行，将 `study-id` 换为机构登记的研究 ID。报告路径须位于受管目录之外、父目录已存在，且文件名未被使用。报告目录应位于支持硬链接的本地文件系统，例如 NTFS，以便无覆盖地保存完整报告。

```powershell
python -X utf8 evaluate_multiview.py evaluate --study study-id --report .\work\multiview-evaluation.json
```

独立报告采用 `independent_multiview_evaluation` 范围，记录审定和授权编号、冻结版本、统计参数、按征象交叉计数、证据帧符合数和必要切面覆盖数。拒判、失败和结果缺失以计划评估该征象的全部测试检查为分母。主要敏感度和特异度保留对应参考阳性、阴性中的拒判和失败。报告同时给出总体覆盖率以及参考明确检查中的覆盖率，便于解读条件指标。参考和预测均给出明确判读时，分别用模型提交的支持帧数和反对帧数作为对应分母，以同类且相同媒体与源帧索引的匹配数为分子。按已冻结分组进行探索性分层。当参考数低于签核下限时保留计数。零分母的指标显示为 `null`。总体主要终点在满足签核的样本量和最低中心数，且至少达到计划重抽次数的 95% 可以计算该终点时给出区间。报告包含受控研究统计，按机构授权范围保存和审核。

## 公开样例验收

使用[已登记公开教学视频](../fixtures/SOURCES.md) S3 与 S1 的期刊原件。命令校验两段视频的 SHA-256，解码源帧，检查本次验收指定的 A4C、PLAX 已覆盖。再只提交 A4C 的确认帧，检查 PLAX 缺失时返回 `insufficient_evidence`。报告标为 `public_workflow_verification`，记录技术验收和帧来源。

```powershell
$env:ECHO_VERIFY_MULTIVIEW_DIR = (Resolve-Path .\work\verify-fixtures).Path
python -X utf8 evaluate_multiview.py verify-public --media-dir $env:ECHO_VERIFY_MULTIVIEW_DIR --report .\work\multiview-public-verification.json
python -X utf8 -m unittest -v backend.test_multiview_independent
```

验收覆盖机构记录缺失、用送审稿代替正式审定、路径越界、缺少公开样例、替换原视频、报告覆盖保护和源帧越界。未设置样例目录时，依赖期刊视频的测试显示为跳过。
