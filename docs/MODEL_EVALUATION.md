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
