# EF 复测标注手册

配套 [EF 复测评估协议](EF_REVIEW_PROTOCOL.md)。本手册只定义**一位判读者**如何填写 EF 复测工作簿；切面与采集质量的既有口径沿用 [标注模板](../templates/儿童心超标注模板.xlsx) 的 `view_code` / `quality_code` / `quality_reason` 三列，不在此重复定义。

标注的目的不是"查模型的错"，而是产出一支独立、可复算的尺子。因此遇到判断不了的情况，**留空并写明原因**，不要猜。

## 一、工作表结构

每批一个工作簿，一行一例检查。前 4 行固定（标题、批次、中文表头、字段名），数据从第 5 行开始。

| 中文表头 | 字段名 | 类型 | 取值 / 单位 | 必填 | 说明 |
| --- | --- | --- | --- | --- | --- |
| 复核编号 | `review_id` | 文本 | `EF-01`…`EF-20` | 是 | 预填，勿改 |
| 检查日期 | `study_date` | 日期 | `YYYY-MM-DD` | 否 | 判读者无需填，程序回填 |
| 判读切面 | `view_code` | 枚举 | `A4C` / `PSAX` / `A4C+PSAX` / `UNK` | 是 | 用到的切面组合 |
| 采集质量 | `quality_code` | 枚举 | `GOOD` / `MEDIUM` / `POOR` | 是 | 沿用既有三级口径 |
| 左室可定位 | `lv_visible` | 布尔 | `是` / `否` | 是 | 心内膜能否在 ED 与 ES 均闭合勾勒 |
| 舒张末帧 | `ed_frame` | 整数 | 帧序号，从 0 起 | 是 | 见第二节判定规则 |
| 收缩末帧 | `es_frame` | 整数 | 帧序号，从 0 起 | 是 | 见第二节判定规则 |
| 测量方法 | `ef_method` | 枚举 | `A5/6` / `SIMPSON` / `VISUAL` | 是 | 实际使用的方法 |
| 目测 EF | `ef_visual` | 整数 | 5–75，% | 否 | 在描迹前先独立目测，减少描迹结果的锚定 |
| 复测 EF | `ef_value` | 数值 | 5–80，% | 是 | 按 `ef_method` 测得的最终值 |
| EF 是否 <50% | `ef_below_50` | 枚举 | `是` / `否` | 是 | 与 `ef_value` 一致性由程序校验 |
| 置信度 | `confidence` | 整数 | 1–5 | 是 | 1=完全不能确定，5=非常确定 |
| 无法判断原因 | `reason` | 文本 | 见第四节枚举 | 条件必填 | `lv_visible=否` 或 `confidence<=2` 时必填 |
| 复核状态 | `review_status` | 枚举 | `待复核` / `已确认` | 是 | 初填留 `待复核`，自检后改 `已确认` |
| 复测序号 | `repeat_of` | 文本 | 空 / `EF-xx` | 否 | 仅重测批填写，程序回填 |

## 二、ED / ES 帧怎么定

先定帧，再测 EF。帧选错的误差（常见 5–15 个 EF 点）比测量方法选择带来的差异更大。

- **ED（舒张末）**：二尖瓣刚关闭前、左室腔径最大的那一帧。以二尖瓣叶对合的第一帧往**前一帧**为 ED。
- **ES（收缩末）**：主动脉瓣关闭那一帧、左室腔径最小的那一帧。二尖瓣开始开放前即为 ES。
- 图像帧率低、瓣膜看不清时，用**最小/最大腔径**兜底，并在 `reason` 写 `valve_unclear`。
- 心律不齐时，选**最有代表性**的一个完整周期（腔径变化最大且边界清晰），不要取平均。

## 三、EF 测量方法

按可用切面选方法，并如实填写 `ef_method`：

| 条件 | 方法 | 代码 |
| --- | --- | --- |
| 同时有 A4C 与 PSAX 乳头肌水平 | 5/6 area-length | `A5/6` |
| 只有 A4C | Simpson 单平面（MOD） | `SIMPSON` |
| 心内膜无法闭合、仅能估读 | 目测分级 | `VISUAL` |

`VISUAL` 只能与 `confidence<=3` 一起出现；一旦使用 `VISUAL`，该例在 Q2 分析中标为低质量并单独报告，不进入主要终点。

先填 `ef_visual`（未描迹时的第一印象），再描迹填 `ef_value`。两步之间的差异本身是有用数据：它反映目测的可靠性。

## 四、留空与原因枚举

`reason` 只能取以下值（便于程序统计）：

| 代码 | 含义 |
| --- | --- |
| `lv_unclear` | 心内膜无法闭合勾勒 |
| `valve_unclear` | 瓣膜开闭看不清，只能按腔径兜底 |
| `poor_quality` | 采集质量差，判读者不愿下结论 |
| `view_insufficient` | 切面不足，无法完成所选方法 |
| `frame_rate_low` | 帧率过低，ED/ES 定位不可靠 |
| `other` | 其他，需在备注列写一句 |

## 五、填写顺序与自检

1. **先切面，再质量**——这两项决定该例是否值得继续。
2. `lv_visible=否` → 直接跳到 `reason`，其余数值列留空。
3. 定 ED/ES → 填 `ef_visual` → 描迹 → 填 `ef_value`。
4. 填 `ef_below_50` 与 `confidence`。
5. **每批完成后**随机抽 3 例重看一遍，不一致就改；全部确认后把 `review_status` 改为 `已确认`。

程序会校验：`ef_below_50` 与 `ef_value` 是否一致、`es_frame` 是否早于 `ed_frame`（允许跨周期，但会提示确认）、`VISUAL` 是否与低置信度搭配、必填项是否齐全。

## 六、命令

生成批次（在授权环境内运行，样例清单切勿入库）：

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\scripts\ef_review.py prepare `
  --cases '.\work\ef-cases.json' --out-dir '.\work\ef-batches' `
  --batches 6 --per-batch 20 --seed 42
```

`--cases` 是可用检查清单，每项至少包含 `clip_id`、`patient_id`、`report_ef`、`quality_code`。程序按协议第 3 节分层抽样、打乱、编号，写出 6 个工作簿和一份冻结文件 `freeze.json`（含映射表与 SHA-256）。**映射表不得入库。**

回收校验：

```powershell
& $python -X utf8 .\scripts\ef_review.py check --workbook '.\work\ef-batches\EF-批-1.xlsx' --out '.\work\ef-check-1.json'
```

统计（读入全部已确认工作簿）：

```powershell
& $python -X utf8 .\scripts\ef_review.py analyze `
  --workbook-dir '.\work\ef-batches' --freeze '.\work\ef-batches\freeze.json' `
  --predictions '.\runs\ef-model\test-predictions.json' --out '.\runs\ef-protocol\report.json'
```

重测批与主样本批的 `repeat_of` 映射由 `freeze.json` 提供；`analyze` 会分别报告 Q1（报告 vs 复测）、Q2（模型 vs 复测）、Q3（判读者自身重测）。

## 七、不要做的事

- 不要先看报告 EF 再测。看到即记录 `recognized=true` 并作废该例于盲法分析。
- 不要在批次之间回头修改已 `已确认` 的行；需要改就新增一行并注明。
- 不要为了"和模型一致"而调整测量；与模型不一致本身是有效数据。
- 不要把工作簿、映射表、任何患者标识提交进 Git。
