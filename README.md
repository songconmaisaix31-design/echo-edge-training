<div align="center">

# Echo Edge · 儿童心超研究工作站

A Windows web workstation and FastAPI backend for reviewing pediatric echocardiography videos and results from local research models.

网页工作站和 FastAPI 后端用于查看儿童心超视频与本地研究模型的结果。

[![python](https://img.shields.io/badge/python-3.13.13-blue.svg)](requirements.txt)
[![platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](docs/WORKSTATION_TERMINAL.md)
[![cuda](https://img.shields.io/badge/torch-2.12.1%2Bcu130-76B900.svg)](requirements.txt)
[![weights](https://img.shields.io/badge/weights-GitHub%20Releases%20(private)-orange.svg)](docs/WORKSTATION_TERMINAL.md)

</div>

> **How to read this page / 怎么读这页**
> English first, then the same content in Chinese — not a summary, not a separate version. Tables keep both languages in one cell.
> 每段英文在前，同一内容的中文紧跟在后；不是摘要，也没有单独的中文版。表格把两种语言放进同一格。

---

## At a glance · 一眼看懂

| | |
|---|---|
| **Problem<br/>问题** | A pediatric echo model that only lives in a notebook cannot be reviewed at the bed side, and a demo that only runs on synthetic clips says nothing about whether the pipeline handles real DICOM cine, colour Doppler or variable frame intervals.<br/>只活在 notebook 里的儿童心超模型，在床旁没法复核；只在合成片段上跑通的演示，也说明不了管线能不能处理真实 DICOM cine、彩色多普勒和变帧间隔。 |
| **Workflow<br/>流程** | The browser creates exams, imports videos and reads frames. The API controls access and stores media and analysis records. Configured local models process eligible inputs.<br/>网页负责建检查、导入视频和阅帧。API 管理访问权限，保存影像和分析记录。已配置的本地模型处理符合条件的输入。 |
| **Model environment<br/>模型环境** | The GPU inference environment uses Windows x64, PowerShell, Python 3.13 and a CUDA GPU. Weights are available in repository Releases with repository access.<br/>GPU 推理环境使用 Windows x64、PowerShell、Python 3.13 和 CUDA GPU。具备仓库权限即可访问 Release 中的权重。 |
| **Not<br/>不是** | Not a diagnostic device, not regulatory-cleared software, and not clinically validated: there is no clinical-accuracy evaluation yet, and every model here is labelled as research use.<br/>不是诊断设备、没有器械认证、也没有临床验证：目前没有临床准确率评估，这里每个模型都标为研究用途。 |

## The problem · 问题在哪

The workstation links a patient's exam, the original cine, and the analysis so a clinician can reopen the record and review the underlying evidence.

工作站将患儿检查、原始动态影像和分析结果关联起来，方便医生重新打开记录并复核证据。

Two constraints shape the repository. Patient video cannot enter Git, so originals, filled annotation workbooks, patient mapping tables, frame caches and run directories are all ignored, and only code, pinned dependency versions, blank templates and documents are tracked. Model weights follow the same rule: they are attached to Releases, not committed.

两个约束决定了这个仓库的形状。病人影像不能进 Git，所以原始影像、填好的标注工作簿、患者关联表、抽帧缓存和训练目录都被忽略，只跟踪代码、固定的依赖版本、空白模板和文档。模型权重同理：挂在 Release 上，不提交进仓库。

Analysis jobs record their status and any failure reason. The workstation displays saved results and evidence.

分析任务会记录状态和失败原因。工作站展示已保存的结果和证据。

## The idea · 思路

| Noun · 名词 | Where · 位置 | Rule · 规则 |
|---|---|---|
| **Exam**<br/>检查 | `backend/clinical.py`, `frontend/` | Every analysis belongs to one exam and one patient; parameters are stored per exam, not globally.<br/>每次分析都归属一个检查和一个患儿；参数按检查保存，不做全局默认。 |
| **Analysis job**<br/>分析任务 | `backend/analysis_jobs.py` | Asynchronous, with explicit states; an interrupted job is recovered as `interrupted`, not silently retried.<br/>异步、状态显式；中断的任务恢复为 `interrupted`，不静默重试。 |
| **Weights**<br/>权重 | GitHub Releases | Engines, checkpoints and reports are release attachments; the inference entry re-checks GPU, TensorRT version and engine SHA-256.<br/>引擎、检查点与报告是 Release 附件；推理入口会复核 GPU、TensorRT 版本与引擎 SHA-256。 |
| **Data boundary**<br/>数据边界 | `data/`, `work/`, `runs/` (ignored) | Originals, annotations and caches stay in the authorised environment; nothing patient-related is committed.<br/>原始影像、标注与缓存留在授权环境里；任何与患者相关的文件都不提交。 |
| **Research label**<br/>研究标签 | Releases, `docs/` | Non-commercial research use only; dataset licences are recorded, and the stricter reading wins where sources disagree.<br/>仅限非商业研究；数据集许可逐项记录，来源冲突时按更严格的口径执行。 |

The isolated browser verification starts both services and exercises exam creation, media import, review and persistence. See [AGENTS.md](AGENTS.md) for the command and the report location.

隔离的浏览器验证会启动两项服务，并操作新建检查、导入影像、阅片和刷新读回。命令和报告位置见 [AGENTS.md](AGENTS.md)。

## Capabilities · 能力清单

Models, with the numbers as recorded in this repository's own reports:

模型与记录在仓库自己报告里的数字：

| Model · 模型 | Data · 数据 | Recorded result · 记录的结果 | Where the record is · 记录在哪 |
|---|---|---|---|
| A4C/PSAX view classification<br/>切面识别 | EchoNet-Pediatric | FP16 TensorRT engine; output is the view, per-class mean probability and the number of sampled frames.<br/>FP16 TensorRT 引擎；输出切面、两类平均概率与抽样帧数。 | [docs/MODEL_EVALUATION.md](docs/MODEL_EVALUATION.md), release `stanford-fp16-epoch10` |
| LV endocardium segmentation<br/>左室分割 | EchoNet-Pediatric fold 0 — 186 patients, 658 videos, 1,298 traced frames<br/>EchoNet-Pediatric 折 0——186 名患儿、658 段视频、1,298 个有描迹帧 | Frame-level Dice 0.9047, IoU 0.8323; per-patient Dice 0.9074.<br/>帧级 Dice 0.9047、IoU 0.8323；按患儿平均 Dice 0.9074。 | [docs/STANFORD_LV_MODEL.md](docs/STANFORD_LV_MODEL.md), release `stanford-lv-epoch8` |
| Four-chamber segmentation<br/>四腔分割 | CardiacUDC — 293 A4C videos, 2,257 annotated frames, two hospitals<br/>CardiacUDC——293 段 A4C 视频、2,257 张标注帧、两家医院 | Independent test (43 groups / 367 frames): LV 0.8243, LA 0.8279, RA 0.8841, RV 0.8420; foreground mean Dice 0.8446 frame-level, 0.7300 group macro.<br/>独立测试（43 组 / 367 帧）：LV 0.8243、LA 0.8279、RA 0.8841、RV 0.8420；前景平均 Dice 帧级 0.8446、组级 0.7300。 | [docs/CARDIACUDC.md](docs/CARDIACUDC.md), release `cardiacudc-fourchamber-v1` |
| Structures, quality grade and EF<br/>结构、质量与射血分数 | CAMUS — 500 patients, 2CH/4CH, ED/ES masks, quality grades<br/>CAMUS——500 名病人、2CH/4CH、ED/ES 掩膜、质量分级 | Held-out 50 patients / 200 frames: LV endocardium Dice 0.9275, myocardium 0.8553, left atrium 0.8835, foreground mean 0.8888; image-quality patient-view macro-F1 0.591.<br/>留出 50 名病人 / 200 帧：左室心内膜 Dice 0.9275、心肌 0.8553、左房 0.8835、前景平均 0.8888；影像质量患者-视位 macro-F1 0.591。 | [docs/CAMUS.md](docs/CAMUS.md), release `camus-structure-quality-v1` |
| EF regression<br/>EF 回归 | EchoNet-Pediatric | Patient-level MAE 3.80; AUROC 0.996 for EF < 50% on 5 positives — direction only, not a screening claim.<br/>患儿级 MAE 3.80；EF < 50% 的 AUROC 0.996 只基于 5 例阳性——只是方向性证据，不是筛查结论。 | [docs/STANFORD_EF.md](docs/STANFORD_EF.md), release `stanford-ef-epoch5` |

These figures are quoted from the reports linked above. This rewrite did not re-run any training or evaluation.

这些数字引自上面链接的报告。这次改写没有重跑任何训练或评估。

| End · 端 | Path · 路径 | What it does · 做什么 |
|---|---|---|
| Web<br/>网页 | `frontend/` | React 19 and Vite support exam creation, video import and playback, and four-chamber overlays. Default `http://127.0.0.1:5181/`.<br/>React 19 和 Vite 提供新建检查、导入与播放视频和四腔叠图。默认 `http://127.0.0.1:5181/`。 |
| API<br/>接口 | `backend/` | FastAPI, SQLAlchemy and Alembic handle permissions, media originals and SQLite analysis records. Default `http://127.0.0.1:8019/`.<br/>FastAPI、SQLAlchemy 和 Alembic 管理权限、媒体原件和 SQLite 分析记录。默认 `http://127.0.0.1:8019/`。 |
| Models<br/>模型 | root scripts | `train_echo.py`, `train_stanford_lv.py`, `train_stanford_ef.py`, `train_camus.py`, `train_cardiacudc.py`, `infer_stanford.py`, `evaluate_echo.py`, `benchmark_echo.py`.<br/>训练、推理、评估与测速入口各一支脚本。 |
| Legacy terminal<br/>历史终端 | `firmware/` | ESP32-S3 sketches and historical device tools.<br/>ESP32-S3 固件与历史设备工具。 |

## Quickstart · 快速开始

### Requirements · 环境要求

| Item · 项目 | Verified configuration · 已验证配置 |
|---|---|
| OS · 系统 | Windows x64, PowerShell<br/>Windows x64、PowerShell |
| Python | 64-bit Python 3.13.13<br/>64 位 Python 3.13.13 |
| GPU · 显卡 | RTX 5060 Laptop 8 GB, NVIDIA driver 582.05. Training, `verify-media` and the GPU benchmark need CUDA; `preflight` runs on CPU.<br/>RTX 5060 Laptop 8 GB、NVIDIA 驱动 582.05。训练、`verify-media` 和 GPU 测速需要 CUDA；`preflight` 可以在 CPU 上跑。 |
| PyTorch | `torch 2.12.1+cu130`, `torchvision 0.27.1+cu130` from the official CUDA 13.0 index<br/>从 PyTorch 官方 CUDA 13.0 索引安装 |
| Inference · 推理 | `tensorrt-cu13==10.16.1.11`, `onnx==1.22.0`, `onnxruntime-gpu==1.27.0` |
| Disk · 磁盘 | About 1.9 GB for the CUDA PyTorch download, plus caches, extracted frames and checkpoints<br/>CUDA 版 PyTorch 下载约 1.9 GB，另需缓存、抽帧与检查点空间 |

### 1. Python environment · Python 环境

```powershell
py -3.13 -m venv .venv
$python = '.\.venv\Scripts\python.exe'
& $python -m pip install torch==2.12.1+cu130 torchvision==0.27.1+cu130 --index-url https://download.pytorch.org/whl/cu130
& $python -m pip install -r requirements.txt --index-url https://pypi.org/simple
& $python -m pip check
```

### 2. Verify the GPU path · 验证 GPU 路径

```powershell
& $python -c "import torch, torchvision; assert torch.cuda.is_available(); print(torch.__version__, torchvision.__version__, torch.version.cuda, torch.cuda.get_device_name(0)); print(torch.arange(16, device='cuda').square().sum().item())"
```

The last value must be `1240`; a different number or an assertion error means the CUDA wheel and the driver do not match. For CPU-only preflight work, install the `+cpu` wheels from the CUDA-free index instead.

最后一行的值必须是 `1240`；数字不对或断言失败，说明 CUDA wheel 与驱动不匹配。只做 CPU 预检时改装 `+cpu` 的 wheel。

### 3. Get weights and run one video · 取权重跑一段视频

```powershell
$releaseDir = '.\work\stanford-fp16-epoch10'
gh release download stanford-fp16-epoch10 `
  --repo songconmaisaix31-design/echo-edge-training `
  --dir $releaseDir `
  --pattern 'Stanford-A4C-PSAX-TensorRT-FP16.engine' `
  --pattern 'Stanford-TensorRT-FP32-FP16-test.json'

$engineReport = Join-Path $releaseDir 'Stanford-TensorRT-FP32-FP16-test.json'
& $python -X utf8 .\infer_stanford.py predict --engine-report $engineReport --video $video --precision fp16
```

A successful run prints one JSON object with `precision` `fp16`, a `view` of `A4C` or `PSAX`, `sampled_frames` and per-class `probabilities`.

成功时输出一条 JSON，`precision` 为 `fp16`，`view` 为 `A4C` 或 `PSAX`，另有 `sampled_frames` 和两类平均概率。

### 4. Run the whole workstation · 跑整台工作站

For a local workstation that opens directly in the browser, prepare the frontend and backend dependencies and run `npm --prefix frontend run dev:local`. Open `http://127.0.0.1:5176/`. Records and media for this mode are stored in `work/local-workstation/`. Set `ECHO_LOCAL_PYTHON` if the backend dependencies are in a separate Python environment.

安装前后端依赖后，运行 `npm --prefix frontend run dev:local`，在 `http://127.0.0.1:5176/` 直接进入本机工作站。该模式的记录与影像保存在 `work/local-workstation/`。如果后端依赖装在独立的 Python 环境中，可以用 `ECHO_LOCAL_PYTHON` 指定解释器。

To configure Stanford and CardiacUDC, set the model variables from the deployment guide in `work/local-workstation/settings.env` or in the launch environment. Use absolute file paths, including the path to the repository's `infer_stanford.py` for `ECHO_INFERENCE_SCRIPT`. The models run on the local workstation.

要配置 Stanford 和 CardiacUDC，请在 `work/local-workstation/settings.env` 或启动环境中填写部署文档中的模型变量，使用文件的绝对路径。将 `ECHO_INFERENCE_SCRIPT` 指向仓库中的 `infer_stanford.py`。两个模型均在本机执行推理。

Build the public sample workstation with `ECHO_PUBLIC_SAMPLES=1`. After building, run `python scripts/build-public-samples.py <approved-archive.zip> frontend/dist` in a Python environment with PyAV and H.264 encoding. Publish `dist`, point `ECHO_PUBLIC_SAMPLE_ROOT` to its `samples` directory, and set `ECHO_PUBLIC_SAMPLE_SCRATCH` to a separate directory writable by the API process. The operator interface is at `/workstation` and uses an authorized session.

设置 `ECHO_PUBLIC_SAMPLES=1` 构建公开样例工作台。构建后用支持 PyAV 和 H.264 编码的 Python 环境执行 `python scripts/build-public-samples.py <获准公开的样例压缩包> frontend/dist`。发布 `dist`，把 `ECHO_PUBLIC_SAMPLE_ROOT` 指向其中的 `samples` 目录，并把 `ECHO_PUBLIC_SAMPLE_SCRATCH` 指向 API 可写的独立目录。操作员使用已授权会话访问 `/workstation`。

For a full public demo, build with `ECHO_DEMO_BUILD=1` and run the API as a separate service with `ECHO_OPEN_DEMO_ROOT` set to a dedicated directory. Initialize `demo.sqlite3` with Alembic, create `local-operator` with `scripts/ensure-operator.py`, and import approved data into `demo.sqlite3` and `demo-media`. Public visitors share edits in the demo.

完整公网演示使用 `ECHO_DEMO_BUILD=1` 构建。API 使用独立服务，`ECHO_OPEN_DEMO_ROOT` 指向专用目录。用 Alembic 初始化 `demo.sqlite3`，用 `scripts/ensure-operator.py` 创建 `local-operator`，随后将获准公开的数据导入 `demo.sqlite3` 和 `demo-media`。访客共享演示中的修改。

To enable StepFun image review, set `ECHO_STEPFUN_KEY_FILE` to a service-readable key file and `ECHO_STEPFUN_SAMPLES_FILE` to a JSON object mapping the five approved media IDs to their source MP4 SHA-256 hashes. Use absolute paths. The local launcher also reads both paths from `work/local-workstation/settings.env`.

要启用阶跃星辰影像复核，将 `ECHO_STEPFUN_KEY_FILE` 指向服务可读取的密钥文件，将 `ECHO_STEPFUN_SAMPLES_FILE` 指向五个获准影像编号及其 MP4 原件 SHA-256 的 JSON 映射。使用绝对路径。本机启动器也可从 `work/local-workstation/settings.env` 读取这两个路径。

Start the workstation with `Start-EchoEdge.ps1` after preparing the API environment, migrations and operator account. The isolated verification instructions are in [AGENTS.md](AGENTS.md). Run the preflight check before training on team data.

准备好 API 环境、数据库迁移和操作员账号后，使用 `Start-EchoEdge.ps1` 启动工作站。隔离验收见 [AGENTS.md](AGENTS.md)。训练团队数据前先运行预检并查看报告。

```powershell
& $python -X utf8 .\train_echo.py train @config --preflight-only
```

Exit code `0` means the data passed; `1` means the report found a problem. The report names the worksheet row, the relative media path and the source frame time.

退出码 `0` 表示数据通过；`1` 表示报告发现问题。报告会指出工作表行号、影像相对路径和源帧时间。

## Documentation · 文档

| Doc · 文档 | Read it when · 什么时候读 |
|---|---|
| [docs/WORKSTATION_TERMINAL.md](docs/WORKSTATION_TERMINAL.md) | Historical workstation and terminal deployment record.<br/>工作站和终端的历史部署记录。 |
| [docs/MODEL_EVALUATION.md](docs/MODEL_EVALUATION.md) | Locked evaluation, ONNX checks and the video inference path.<br/>锁定模型后的评估、ONNX 核对与视频推理路径。 |
| [docs/EVALUATION.md](docs/EVALUATION.md) | Per-patient evaluation and the read-only Stanford fold review.<br/>按患儿评估与 Stanford 分折只读核查。 |
| [docs/CAMUS.md](docs/CAMUS.md), [docs/CARDIACUDC.md](docs/CARDIACUDC.md) | The two public datasets and their segmentations.<br/>两个公开数据集与对应分割能力。 |
| [docs/ECHONET_PEDIATRIC.md](docs/ECHONET_PEDIATRIC.md), [docs/STANFORD_LV_MODEL.md](docs/STANFORD_LV_MODEL.md), [docs/STANFORD_EF.md](docs/STANFORD_EF.md) | Stanford-side training prep, the LV model, the EF model.<br/>Stanford 侧训练准备、左室模型、EF 模型。 |
| [docs/TRAINING_DATA.md](docs/TRAINING_DATA.md) | What to deliver and how the workbook is filled.<br/>交什么数据、工作簿怎么填。 |
| [docs/DOCTOR_REVIEW.md](docs/DOCTOR_REVIEW.md) | The clinician review flow.<br/>医生复核流程。 |
| [docs/PUBLIC_DATASETS.md](docs/PUBLIC_DATASETS.md) | Availability, licences and citations for every public source.<br/>每个公开数据源的可下载性、许可与引用。 |
| [docs/ESP32_TERMINAL.md](docs/ESP32_TERMINAL.md), [docs/firmware/](docs/firmware) | Terminal plan, wiring, pin limits, I²C addresses, ECW registers and pitfalls.<br/>终端方案、接线、引脚禁区、I²C 地址、ECW 寄存器与踩坑记录。 |
| [docs/V1_E2E_ACCEPTANCE.md](docs/V1_E2E_ACCEPTANCE.md) | The full end-to-end acceptance record, module by module.<br/>逐模块的全系统端到端验收记录。 |
| [docs/batch-01/](docs/batch-01) | The first batch: cleaning report, selected-frame index, expert handoff.<br/>第 01 批：清洗报告、抽帧索引、专家交接。 |
| [reference/README.md](reference/README.md) | The vendor competition kit's module definitions and wiring.<br/>厂商参赛套件的模块定义与接线。 |

## Repository layout · 仓库结构

```text
echo-edge-training/
├── backend/                # FastAPI app, SQLAlchemy models, Alembic migrations, tests
├── frontend/               # React 19 + Vite workstation UI
├── firmware/
│   ├── edge-terminal/      # ESP32-S3: receives HINT, returns ACK
│   ├── echo-panel/         # 128×64 OLED + SYN6288E voice panel
│   └── ecw-*/ sensor-hub/ probe/ tools/
├── Start-EchoEdge.ps1      # local launcher; -Lan and Stanford variants next to it
├── train_echo.py           # team dual-head fine-tune + preflight gate
├── train_stanford_lv.py    # left-ventricle segmentation
├── train_stanford_ef.py    # ejection fraction regression
├── train_camus.py          # CAMUS structures, quality, EF
├── train_cardiacudc.py     # four-chamber segmentation
├── infer_stanford.py       # TensorRT FP16 video inference
├── evaluate_echo.py        # locked evaluation and ONNX checks
├── benchmark_echo.py       # decode/preprocess/GPU timing
├── echo_dicom.py           # DICOM cine reading
├── templates/              # blank annotation workbook
├── docs/                   # deployment, models, data, firmware, acceptance
└── reference/moce-kit/     # vendor kit module definitions
```

## Status · 状态

The historical acceptance results are in `docs/V1_E2E_ACCEPTANCE.md`. The current isolated verification writes its results to `work/verify/report.json`.

历史验收结果保存在 `docs/V1_E2E_ACCEPTANCE.md`。当前隔离验证把实际结果写入 `work/verify/report.json`。

Known limits, stated rather than hidden:

边界写出来而不是藏起来：

- **No clinical accuracy claim.** Nothing here has been evaluated against clinical ground truth; the models are research artefacts.
- **Team data is not validated.** Two-dimensional colour Doppler, variable frame intervals, formal annotations and the resulting fine-tune still have to be exercised on real hospital data.
- **No CI.** The repository has no `.github/workflows`, so there is no CI badge on this page; `pytest` is run by hand. Test counts above are the numbers recorded in the acceptance document, not a run repeated for this rewrite.
- **No licence file.** There is no `LICENSE` at the repository root; see below.

- **不做临床准确率声明。** 这里的东西没有和临床真值对照评估过；模型是研究产物。
- **团队数据没有验证。** 二维彩色多普勒、变帧间隔、正式标注以及由此得到的微调，仍需在真实院方数据上跑过。
- **没有 CI。** 仓库里没有 `.github/workflows`，所以这页不放 CI 徽章；`pytest` 是手工跑的。上面的测试数字是验收文档里记录的数字，不是这次改写重跑的。
- **没有许可文件。** 仓库根目录没有 `LICENSE`，见下。

## License · 许可

There is no `LICENSE` file, so the code is not offered under an open licence. Model weights, engines and reports are distributed through this repository's Releases for non-commercial research to members with repository access. Third-party datasets keep their own terms; where a source's licence is ambiguous, the stricter reading is applied.

仓库里没有 `LICENSE` 文件，所以代码没有以开源许可提供。模型权重、引擎与报告通过本仓库的 Release 提供给有权限的成员，仅限非商业研究。第三方数据集沿用各自条款；来源许可存在歧义时，按更严格的口径执行。
