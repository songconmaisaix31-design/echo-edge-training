# 儿童心超模型与采集终端

RTX 5060 电脑负责心超影像解码、MobileNetV3-Small 切面与采集质量模型的训练和推理。ESP32-S3 负责外设采集、OLED 显示、语音和振动提示，以及台架上的舵机控制。终端接线、开发顺序和实机验收见 [ESP32-S3 终端方案与任务](docs/ESP32_TERMINAL.md)。

## 模型开发任务

- [#7 回收团队标注并通过数据预检](https://github.com/songconmaisaix31-design/echo-edge-training/issues/7)
- [#8 建立独立测试入口和评价报告](https://github.com/songconmaisaix31-design/echo-edge-training/issues/8) · [评估命令和报告口径](docs/EVALUATION.md)
- [#9 训练 EchoNet-Pediatric A4C／PSAX 基线](https://github.com/songconmaisaix31-design/echo-edge-training/issues/9)，待 Stanford 确认训练和权重用途。
- [#10 在 RTX 5060 微调团队切面与采集质量双头](https://github.com/songconmaisaix31-design/echo-edge-training/issues/10)，使用团队标注和独立测试入口。
- [#11 导出 ONNX 并验证 RTX 5060 推理](https://github.com/songconmaisaix31-design/echo-edge-training/issues/11)
- [#12 将电脑推理结果接入 ESP32-S3 提示](https://github.com/songconmaisaix31-design/echo-edge-training/issues/12)

## 电脑端处理

电脑端支持从二维多帧 DICOM cine、AVI、MP4 或 MOV 读取原始时间戳和抽帧。FFmpeg 将图像等比例缩放并填充为 `224×224` RGB。训练入口先做数据预检，报告通过后初始化 CUDA 并开始微调。

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

## 数据交付

请把 [Excel 标注模板](templates/儿童心超标注模板.xlsx) 和 [训练数据交付要求](docs/TRAINING_DATA.md) 发给团队。首批优先收集拟部署设备上的完整去标识儿童经胸心超检查，包含灰阶、彩色多普勒、低质量画面、过渡和补扫。由儿科心超专家先确认切面和采集质量标签定义，再交付源影像和已复核的 Excel。

使用斯坦福公开研究数据时，见 [EchoNet-Pediatric 训练准备](docs/ECHONET_PEDIATRIC.md)。

第 01 批资料见[批量清洗与预处理报告](docs/batch-01/cleaning-report.md)和[儿童心脏超声专家交接](docs/batch-01/expert-handoff.md)。[团队抽帧索引](docs/batch-01/team-selected-frames.csv)记录批内编号、源帧号和相对时间。获准访问影像后，可按索引定位原始画面。

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

默认前两轮只训练两个分类头，之后解冻骨干最后三个特征模块。训练完成写出 `best.pt` 和 `metrics.json`。验证报告包含各任务的 Macro-F1、支持数和混淆矩阵。Macro-F1 按该分组中实际出现的类别计算，解读时应同时检查类别支持数。`test` 分组由预检核对，当前训练入口只训练和评估 `train`、`val`。

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

GitHub 仅存放代码、依赖版本、空白模板和说明文档。原始影像、填好的标注、患者关联表、私有报告、抽帧缓存和模型权重应留在授权环境。`data/`、`work/` 和 `runs/` 已加入 Git 忽略规则。上传其他文件前检查 `git status`，尤其留意无扩展名的 DICOM 文件。
