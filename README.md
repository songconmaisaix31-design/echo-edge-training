# 儿童心超模型与采集终端

RTX 5060 电脑负责心超影像解码、MobileNetV3-Small 切面与采集质量模型的训练和推理。ESP32-S3 负责外设采集、OLED 显示、语音和振动提示，以及台架上的舵机控制。终端接线、开发顺序和实机验收见 [ESP32-S3 终端方案与任务](docs/ESP32_TERMINAL.md)。

终端固件源码、构建与烧录命令见 [firmware/](firmware/README.md)；排查记录与踩过的坑见 [docs/firmware/pitfalls.md](docs/firmware/pitfalls.md)，硬件关键事实（引脚禁区、I²C 地址、ECW 寄存器）见 [docs/firmware/hardware-facts.md](docs/firmware/hardware-facts.md)。厂商参赛套件的模块定义收在 [reference/moce-kit/](reference/README.md)。

## 模型开发任务

- [#7 回收团队标注并通过数据预检](https://github.com/songconmaisaix31-design/echo-edge-training/issues/7)
- [#8 建立独立测试入口和评价报告](https://github.com/songconmaisaix31-design/echo-edge-training/issues/8) · [评估命令和报告口径](docs/EVALUATION.md)
- [#9 训练 EchoNet-Pediatric A4C／PSAX 基线](https://github.com/songconmaisaix31-design/echo-edge-training/issues/9) · [第 10 轮 FP16 研究版](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-fp16-epoch10)
- [#10 在 RTX 5060 微调团队切面与采集质量双头](https://github.com/songconmaisaix31-design/echo-edge-training/issues/10)，使用团队标注和独立测试入口。
- [#11 导出 ONNX 并验证 RTX 5060 推理](https://github.com/songconmaisaix31-design/echo-edge-training/issues/11)
- [#12 将电脑推理结果接入 ESP32-S3 提示](https://github.com/songconmaisaix31-design/echo-edge-training/issues/12)

## 电脑端处理

电脑端支持从二维多帧 DICOM cine、AVI、MP4 或 MOV 读取原始时间戳和抽帧。FFmpeg 将图像等比例缩放并填充为 `224×224` RGB。训练入口先做数据预检，报告通过后初始化 CUDA 并开始微调。

[Stanford 切面推理入口](docs/MODEL_EVALUATION.md#电脑端视频推理与全链路复测)使用 TensorRT FP16 和 CUDA Graph 对本地 AVI、MP4 或 MOV 输出整段切面判定、平均类别概率和抽样帧数。

## 环境要求

| 项目 | 已验证配置和用途 |
| --- | --- |
| 操作系统 | Windows x64、PowerShell |
| Python | 64 位 Python 3.13.13 |
| GPU | RTX 5060 Laptop 8 GB，NVIDIA 驱动 582.05。运行训练、`verify-media` 和 GPU 性能测试需要 CUDA。`preflight` 和 `train --preflight-only` 可以在 CPU 上完成 |
| PyTorch | `torch 2.12.1+cu130`、`torchvision 0.27.1+cu130`，从 PyTorch 官方 CUDA 13.0 索引安装 |
| 解码和表格 | `requirements.txt` 固定 PyAV、pydicom、JPEG/JPEG 2000/JPEG-LS 解码插件、Pillow 和 openpyxl 等依赖。PyAV wheel 自带 FFmpeg 运行库 |
| 网络 | 首次安装需要能访问 PyPI 和 `download.pytorch.org`。第一次运行 GPU 路径时，TorchVision 会将预训练权重下载到指定缓存目录。可通过受控渠道预置权重 |
| 磁盘 | CUDA 版 PyTorch 下载约 1.9 GB。安装及缓存另外占用磁盘。训练还需要给单个解包影像、临时抽帧 PNG 和检查点留出空间 |

使用预编译 PyTorch wheel 时，需要兼容的 NVIDIA 驱动和 wheel 自带的 CUDA 运行库。`nvidia-smi` 用来检查驱动，下面的实际 CUDA 张量运算用来验证 PyTorch 环境。 [PyTorch 官方安装版本](https://pytorch.org/get-started/previous-versions/) · [NVIDIA CUDA 兼容性](https://docs.nvidia.com/deploy/cuda-compatibility/)

从仓库根目录执行以下 PowerShell 命令。已将 Python 3.13 设为默认时可以用 `python` 代替 `py -3.13`。

```powershell
py -3.13 -m venv .venv
$python = '.\.venv\Scripts\python.exe'
& $python -m pip install torch==2.12.1+cu130 torchvision==0.27.1+cu130 --index-url https://download.pytorch.org/whl/cu130
& $python -m pip install -r requirements.txt --index-url https://pypi.org/simple
& $python -m pip check
nvidia-smi
& $python -c "import torch, torchvision; assert torch.cuda.is_available(); print(torch.__version__, torchvision.__version__, torch.version.cuda, torch.cuda.get_device_name(0)); print(torch.arange(16, device='cuda').square().sum().item())"
```

最后一行应输出 `1240`。只做 CPU 数据预检的环境，可以把 PyTorch 安装命令换成 `torch==2.12.1+cpu torchvision==0.27.1+cpu --index-url https://download.pytorch.org/whl/cpu`。其他依赖仍从 `requirements.txt` 安装。

## 下载并运行 Release 的 FP16 模型

[Stanford EchoNet-Pediatric 第 10 轮研究版](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-fp16-epoch10) 供有私有仓库访问权限的成员用于非商业研究。已验证的组合为 Windows x64、NVIDIA GeForce RTX 5060 Laptop GPU、CUDA 13 PyTorch 和 TensorRT 10.16.1.11。推理入口会核对 GPU 型号、TensorRT 版本和引擎 SHA-256。

安装 GitHub CLI，用有仓库权限的账号完成 `gh auth login --hostname github.com`。首次获取代码时，在 PowerShell 执行以下命令。

```powershell
gh repo clone songconmaisaix31-design/echo-edge-training -- --branch main --single-branch
Set-Location .\echo-edge-training
```

已有本地副本时，在仓库根目录更新 `main` 即可。

按[环境要求](#环境要求)配置 CUDA 版 PyTorch 和基础依赖，再在仓库根目录安装推理依赖。

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -m pip install 'tensorrt-cu13==10.16.1.11' 'onnx==1.22.0' 'onnxruntime-gpu==1.27.0' --index-url https://pypi.org/simple
```

下载以下两个附件到同一目录。也可以用有仓库权限的账号登录 GitHub 后，从 Release 页面直接下载这两个文件。

```powershell
$releaseDir = '.\work\stanford-fp16-epoch10'
gh release download stanford-fp16-epoch10 `
  --repo songconmaisaix31-design/echo-edge-training `
  --dir $releaseDir `
  --pattern 'Stanford-A4C-PSAX-TensorRT-FP16.engine' `
  --pattern 'Stanford-TensorRT-FP32-FP16-test.json'
if ($LASTEXITCODE -ne 0) { throw 'Release 附件下载失败' }
```

将 `$video` 改为获授权的本地灰阶心超 AVI、MP4 或 MOV 路径。程序从引擎报告读取锁定参数，运行只需上述两个 Release 附件。

```powershell
$engineReport = Join-Path $releaseDir 'Stanford-TensorRT-FP32-FP16-test.json'
$video = '.\data\sample.avi'
& $python -X utf8 .\infer_stanford.py predict --engine-report $engineReport --video $video --precision fp16
```

成功时输出一条 JSON，`precision` 为 `fp16`，`view` 为 `A4C` 或 `PSAX`，另有 `sampled_frames` 和两类平均概率 `probabilities`。

## Stanford 左室分割模型

EchoNet-Pediatric 左室心内膜腔单帧分割模型，训练、独立测试和单帧预测入口都在 `train_stanford_lv.py`。接口约定、接口测试记录、训练配置和独立测试评分见 [Stanford 左室分割模型的接口与独立测试](docs/STANFORD_LV_MODEL.md)。

锁定独立测试折（原始折 0，186 名患儿、658 段视频、1,298 个有描迹帧）的帧级 Dice 为 0.9047、IoU 为 0.8323；按患儿平均的 Dice 为 0.9074、IoU 为 0.8362（A4C 0.8938 / 0.8147，PSAX 0.9151 / 0.8482）。权重、验证曲线、独立测试报告和接口测试记录通过 [stanford-lv-epoch8](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-lv-epoch8) 提供。

## 公开数据扩展：CAMUS 与射血分数

在 EchoNet-Pediatric 之外接入了 [CAMUS](docs/CAMUS.md) 公开数据（500 名病人、2CH/4CH、ED/ES 手工结构掩膜、Good/Medium/Poor 质量分级与 EF），新增多结构分割、影像质量分级和射血分数回归三项能力。数据源的可下载性、许可与引用见 [公开心超数据源调研](docs/PUBLIC_DATASETS.md)。

CAMUS 独立测试（50 名留出病人、200 帧）：左室心内膜 Dice 0.9275、左室心肌 0.8553、左房 0.8835，前景平均 Dice 0.8888；影像质量患者-视位 macro-F1 0.591。EchoNet-Pediatric EF 模型患儿级 MAE 3.80，EF < 50% 异常筛查 AUROC 0.996（阳性 5 例，仅方向性证据）。模型与报告通过 [camus-structure-quality-v1](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/camus-structure-quality-v1) 与 [stanford-ef-epoch5](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-ef-epoch5) 提供。

### CardiacUDC 四腔分割

补充 [CardiacUDC](docs/CARDIACUDC.md)（293 段 A4C 视频、2,257 张标注帧、两家医院），把结构能力扩展到 LV / LA / RA / RV 四个腔室。许可存在标注不一致，按更严格口径限定非商业研究。独立测试（43 组 / 367 帧）：LV Dice 0.8243、LA 0.8279、RA 0.8841、RV 0.8420，前景平均 Dice 0.8446（帧级）/ 0.7300（组级宏平均）。权重与报告通过 [cardiacudc-fourchamber-v1](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/cardiacudc-fourchamber-v1) 提供。

## 数据交付

请把 [Excel 标注模板](templates/儿童心超标注模板.xlsx) 和 [训练数据交付要求](docs/TRAINING_DATA.md) 发给团队。首批优先收集拟部署设备上的完整去标识儿童经胸心超检查，包含灰阶、彩色多普勒、低质量画面、过渡和补扫。由儿科心超专家先确认切面和采集质量标签定义，再交付源影像和已复核的 Excel。

使用斯坦福公开研究数据时，见 [EchoNet-Pediatric 训练准备](docs/ECHONET_PEDIATRIC.md)。

第 01 批 8 段团队视频为演示录屏。[工程清洗报告](docs/batch-01/cleaning-report.md)和[抽帧索引](docs/batch-01/team-selected-frames.csv)用于解码和预处理联调。[专家交接说明](docs/batch-01/expert-handoff.md)适用于后续收到的真实心超 cine。

把数据放在院方允许的目录。下面使用仓库内被 Git 忽略的 `data/` 作为路径示例。`$media` 可以指向影像目录、ZIP 或单个影像。`$labels` 指向填好的工作簿。Excel `relative_path` 从影像目录或 ZIP 根目录填写。

## 先检查，再训练

```powershell
$python = '.\.venv\Scripts\python.exe'
$media = '.\data\cine'
$labels = '.\data\team_annotations.xlsx'
$config = @(
  '--media', $media, '--labels', $labels,
  '--cache-dir', '.\work\cache', '--run-dir', '.\runs\finetune-01',
  '--sample-fps', '10', '--max-frames-per-segment', '8',
  '--val-fraction', '0.2', '--seed', '42', '--batch-size', '8',
  '--epochs', '12', '--head-only-epochs', '2',
  '--head-lr', '0.0003', '--backbone-lr', '0.00003'
)
& $python -X utf8 .\train_echo.py train @config --preflight-only
```

预检报告默认为 `runs/finetune-01.preflight.json`，可用 `--report` 指定训练目录外的路径。检查包括文件是否存在及可读、确认区间是否抽到帧、标签字典及标注是否有效、同一患儿是否跨组，以及训练标签与验证任务覆盖。报告包含工作表行号、影像相对路径和源帧时间。退出码 `0` 表示通过，`1` 表示发现数据问题。只预检时可以使用尚未创建的训练目录。

人工复核报告后，在同一个 PowerShell 会话运行正式命令。训练会重新执行预检，报告通过后直接使用已校验的标注和分组。每次正式训练应指定新的 `--run-dir`。

```powershell
& $python -X utf8 .\train_echo.py train @config
```

默认前两轮只训练两个分类头，之后解冻骨干最后三个特征模块。训练帧按每名患儿的已抽取帧数倒数加权，保持各患儿在抽样中的总权重相同。训练输出 `best.pt`、`config.json` 和 `metrics.json`，记录标签顺序、患儿划分、训练参数、每轮耗时和峰值显存。训练日志中的验证指标按帧计算。锁定模型和阈值后的片段级评估与导出见 [独立评估与 ONNX 核对](docs/MODEL_EVALUATION.md)。

按患儿评估及 Stanford 分折只读核查的 `team` 和 `stanford` 子命令见 [心超模型独立评估](docs/EVALUATION.md)。

## 单独验证影像和测速

先创建报告存放目录。`verify-media` 报告 DICOM 源帧号、帧时间和彩色像素，并对每个片段执行一次 CUDA 前向。该命令可使用空模板检查表头。

```powershell
New-Item -ItemType Directory -Force .\work | Out-Null
& $python -X utf8 .\train_echo.py verify-media --media $media `
  --template '.\templates\儿童心超标注模板.xlsx' --cache-dir '.\work\cache' `
  --sample-fps 10 --report '.\work\media-check.json'
```

`benchmark_echo.py` 对 MP4/MOV ZIP 测量解码、预处理、显存传输和 GPU 前向。默认 `--mode optimized` 使用合并的 FFmpeg 缩放与 RGB 转换、锁页内存和 CUDA Graph。加 `--mode baseline` 可以对比原路径。

```powershell
& $python -X utf8 .\benchmark_echo.py '.\data\videos.zip' `
  --cache-dir '.\work\cache' --sample-fps 10 --precision fp16 `
  --mode optimized --report '.\work\benchmark.json'
```

## 验证边界和公开内容

已在 RTX 5060 上验证 CUDA 运算、真实视频解码与抽帧、常规 DICOM cine 的灰阶与彩色读取、双头前向和预检拦截。团队设备的二维彩色多普勒、变帧间隔样本、正式标注和训练结果仍需在实际数据上验证。该仓库提供研究训练与工程验证代码，尚无临床准确率和目标终端部署结果。

Git 版本库跟踪代码、依赖版本、空白模板和说明文档。获准共享的 Stanford 权重、推理引擎和汇总测试报告通过本私有仓库的 Release 提供。原始影像、填好的标注、患者关联表、其他私有报告和抽帧缓存应留在授权环境。`data/`、`work/` 和 `runs/` 已加入 Git 忽略规则。上传其他文件前检查 `git status`，尤其留意无扩展名的 DICOM 文件。
