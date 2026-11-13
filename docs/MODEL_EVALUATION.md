# 独立评估与 ONNX 核对

团队模型训练前，在回收表中按患儿锁定 `train`、`val` 和 `test`。用正式参数执行预检。训练 checkpoint 保存分组、标签顺序和抽样参数，独立评估会核对这些值。缺少这些训练记录的旧 checkpoint 会被拒绝，当前入口需要用包含这些记录的训练 checkpoint。

在验证组确定每个输出头的置信阈值。将这些值填入本地 JSON 文件，`min_confidence` 中包含 `view`，团队模型还包含 `quality`。对于团队模型，按临床确认的口径使用 `usable_quality`、`low_quality` 和 `reject_quality` 三个列表，分别填写可放行、已确认低质量和直接拒判的质量标签代码。三个列表覆盖质量字典，每个代码只出现一次。阈值使用 `[0,1]` 范围内的概率。为每组候选值用验证组出具报告，由临床负责人确认采用的阈值，再用相同命令加 `--lock` 写出冻结文件。

```powershell
$python = '.\.venv\Scripts\python.exe'
$team = @('--media', $media, '--labels', $labels, '--checkpoint', $checkpoint, '--scratch', '.\work\cache')
& $python -X utf8 .\evaluate_echo.py @team --split val --policy $reviewedPolicy `
  --report '.\work\team-val.json' --lock '.\work\team-locked.json'
& $python -X utf8 .\evaluate_echo.py @team --split test --policy '.\work\team-locked.json' `
  --report '.\work\team-test.json'
```

Stanford 灰阶模型授权完成后，用 `--archive $archive` 代替 `--media` 和 `--labels`，使用该模型对应的 checkpoint 和仅含 `view` 的阈值文件。程序复核第 0 次轮转和每段所属折，并在测试阶段使用原始折 `0`。

片段预测为各帧 softmax 概率的平均值。未达置信阈值的预测单列为拒判。质量报告按确认为低质量的区间计算误放行比例，按所有已标注质量区间计算拒判率和覆盖率。95% 区间按患儿重抽样 1,000 次。分母为零时记录 `null`。团队报告按设备、成像模式和院点代码分别统计。院点分组结果可用于追踪差异，独立院点评估须使用事先设计的外部测试组。

## 导出与核对

在 CUDA PyTorch 环境安装导出和运行时依赖。需要 CPU 核对时，在单独环境安装同版本 `onnxruntime` 并传 `--device cpu`。

```powershell
& $python -m pip install onnx==1.22.0 onnxscript==0.7.2 onnxruntime-gpu==1.27.0
& $python -X utf8 .\edge_onnx.py export --checkpoint $checkpoint `
  --policy '.\work\team-locked.json' --onnx '.\work\team.onnx'
& $python -X utf8 .\edge_onnx.py verify --checkpoint $checkpoint `
  --policy '.\work\team-locked.json' --onnx '.\work\team.onnx' `
  --media $representativeCine --start-ms $startMs --end-ms $endMs `
  --max-abs-error $reviewedTolerance --scratch '.\work\cache' --report '.\work\onnx-verify.json'
```

导出固定的 `[1,3,224,224]` RGB 输入及 `view_logits`、`quality_logits` 两个输出。模型内保存标签顺序、抽样、预处理和冻结阈值。核对相同真实帧上的输出偏差、逐帧类别、片段级判定和质量放行结果。目录或 ZIP 中的所有影像使用同一时间区间。区间不同的代表性影像应逐个执行。报告每个视频的解码与抽样、预处理、传输、运行时执行和完整链路的延迟及吞吐。Windows 显存采样包含同进程中的 PyTorch 参考模型和 ONNX Runtime。原有 ImageNet 测速结果单列。

## Stanford 单头导出、量化和独立对照

在装有 PyTorch 和仓库依赖的 CPU 环境安装 `onnx==1.22.0`、`onnxscript==0.7.2` 和 `onnxruntime==1.27.0`。用对应 checkpoint 的验证冻结文件导出同样的固定输入，单头只输出 `view_logits`。

```powershell
$fp32 = '.\work\stanford-fp32.onnx'
$int8 = '.\work\stanford-int8.onnx'
& $python -X utf8 .\edge_onnx.py export --checkpoint $checkpoint --policy $locked --onnx $fp32
$edge = @('--archive', $archive, '--checkpoint', $checkpoint, '--policy', $locked, '--fp32', $fp32, '--int8', $int8)
& $python -X utf8 .\stanford_edge.py quantize @edge
& $python -X utf8 .\stanford_edge.py compare @edge --report '.\work\stanford-fp32-int8-test.json'
```

量化用固定随机种子从原始验证折 `1` 各抽取 80 段 A4C 和 PSAX，复用正式预处理和抽帧，对卷积和矩阵乘法采用静态 S8S8 QDQ、逐通道权重和 MinMax 校准。两份模型保留 FP32 输入与输出。 [ONNX Runtime 量化说明](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)

对照入口核对模型来源、固定阈值、量化节点和实际运行的整数算子。原始测试折 `0` 每个抽样帧送入两份模型，按各帧 softmax 平均值决定片段类别。报告文件大小和 SHA-256、片段级混淆矩阵及召回率，以及 CPU 单线程、batch 1 各预热 40 次后的逐帧模型执行耗时。解码、预处理和输入复制不计入模型执行耗时，预处理单独报告。报告同时记录 FP32 与 PyTorch 的输出偏差和预测分歧。

## Stanford TensorRT FP32 与 FP16

使用 CUDA 13 PyTorch 环境，安装本节所用的 TensorRT 版本 `tensorrt-cu13==10.16.1.11` 和导出依赖。将 `$fp32` 指向对应锁定 checkpoint 的 FP32 ONNX。

```powershell
& $python -m pip install tensorrt-cu13==10.16.1.11 onnx==1.22.0 onnxscript==0.7.2 onnxruntime==1.27.0
& $python -X utf8 .\stanford_tensorrt.py --archive $archive --checkpoint $checkpoint `
  --policy $locked --onnx $fp32 --output-dir '.\work\stanford-tensorrt'
```

同一 FP32 ONNX 构建两份 TensorRT 引擎，两份均关闭 TF32，FP16 引擎允许使用半精度算子并保留 FP32 输入输出。输出目录包含两份序列化引擎和 JSON 报告，报告记录构建和实测所使用的 GPU 与 TensorRT 版本。独立测试复用原始折 `0` 的抽帧与片段聚合。两份引擎均在 CUDA 单流上逐帧执行，每份预热 40 次，通过 CUDA Events 记录 GPU 推理 P50、P95 和 P99。解码、预处理和 CPU 与 GPU 之间的复制时间不参与计时。 [NVIDIA TensorRT 计时方法](https://docs.nvidia.com/deeplearning/tensorrt/10.x.x/performance/measurement-techniques.html)

## 电脑端视频推理与全链路复测

使用上一步导出的 TensorRT 测试报告，将报告和对应的引擎文件放在同一目录。电脑端默认加载 FP16 引擎，校验引擎 SHA-256，并用报告中的预处理、时间抽样和锁定阈值判定整段视频。输入为本地 AVI、MP4 或 MOV，标准输出为单条 JSON。传 `--precision fp32` 可指定 FP32。

```powershell
$engineReport = '.\work\stanford-tensorrt\Stanford-TensorRT-FP32-FP16-test.json'
& $python -X utf8 .\infer_stanford.py predict --engine-report $engineReport --video $video
& $python -X utf8 .\infer_stanford.py benchmark --engine-report $engineReport --archive $archive `
  --report '.\work\stanford-tensorrt\Stanford-TensorRT-FP32-FP16-pipeline.json'
```

全链路复测与本地推理调用同一个视频判定函数。每个引擎用验证折的 5 段视频预热，独立测试时交替先后顺序。每份引擎分别打开同一段已从 ZIP 读取的编码视频，计入单遍解码、按 PTS 抽帧、缩放和归一化、CPU 与 GPU 之间的传输、TensorRT 前向计算以及片段聚合和判定。报告按片段给出耗时百分位汇总，并用各引擎的累计片段耗时计算片段每秒和抽样帧每秒。ZIP 读取、引擎加载、预热和报告写入在计时区间外。

单遍解码暂存当前视频的原始解码帧，用最后一帧的真实 PTS 确定终点，按原时间分箱取样，随后释放未选中的原始帧。峰值内存随单段视频的解码帧数和分辨率增长。

```powershell
$test = @('--archive', $archive, '--engine-report', $engineReport)
& $python -X utf8 .\infer_stanford.py profile @test --decode-passes 2 --report '.\work\stanford-fp16-two-pass.json'
& $python -X utf8 .\infer_stanford.py profile @test --decode-passes 1 --report '.\work\stanford-fp16-single-pass.json'
& $python -X utf8 .\infer_stanford.py benchmark-decode @test --report '.\work\stanford-fp16-decode-comparison.json'
```

`profile` 报告每段的解码和抽帧、预处理、输入上传、GPU 推理和输出回传耗时。GPU 推理使用 CUDA Events，传输边界显式同步。使用 `benchmark-decode` 比较同一个 FP16 引擎处理同一视频的两种方式，该入口用普通推理路径计时，并在计时区间外逐帧核对 PTS 和归一化输入。两遍解码保留用于复核。
