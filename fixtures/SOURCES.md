# 浏览器验证视频

`flower.mp4` 是 MDN 示例中的花卉视频，来源目录标记为 CC0。用于验证视频上传、解码、播放、持久化和四腔分割的非 A4C 帧处理。视频不含患者影像。

- [固定版本的视频](https://raw.githubusercontent.com/mdn/interactive-examples/13024db581b08fae11a0ca229497198b9b0534e7/live-examples/media/cc0-videos/flower.mp4)
- [MDN 视频元素示例](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/video)

## Stanford 真实切面验收

使用 Damji 等人在 POCUS Journal 发布的教学视频 S3。原文标注为心尖四腔心切面。验收使用期刊原始 MP4，仅在本机运行，校验推理任务、模型版本和刷新后的结果读回。样例文件保存在 Git 忽略目录。[期刊文章](https://pocusjournal.com/article/2021-06-01p10-12/) · [原始视频 S3](https://pocusjournal.com/wp-content/uploads/2021/04/Damji-et-al-Video-S3-A4C-TV-Endo-1.mp4) · [期刊许可](https://pocusjournal.com/policies/#copyright-licenses-and-preprints)

原始 MP4 的 SHA-256 为 `543c6a330eda334e498d8bb2ee7880a46a899c660b5b669d7613ee746d4a9d6c`。

在仓库根目录运行。将 `$modelDir` 和 `ECHO_INFERENCE_PYTHON` 指向本机已有的 [Stanford Release](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/stanford-fp16-epoch10) 文件与安装了推理依赖的 Python。

```powershell
$modelDir = 'C:\path\to\stanford-release'
$env:ECHO_INFERENCE_PYTHON = 'C:\path\to\model-venv\Scripts\python.exe'
$env:ECHO_ENGINE_REPORT = Join-Path $modelDir 'Stanford-TensorRT-FP32-FP16-test.json'
$env:ECHO_STANFORD_CHECKPOINT = Join-Path $modelDir 'Stanford-A4C-PSAX-best.pt'
$env:ECHO_VERIFY_STANFORD_VIDEO = '.\work\verify-fixtures\Damji-et-al-Video-S3-A4C-TV-Endo-1.mp4'
npm --prefix frontend test
```

真实推理使用 PyTorch CPU FP32，验收运行在独立 SQLite、媒体目录和本机端口。第一轮使用发布策略。随后仅将验收目录中的策略副本切面阈值设为 `1`，用同一权重和同一视频再次真实推理。验收核对概率低于阈值、任务状态为 `insufficient_evidence` 且 `view: null`、页面显示「证据不足」和复核建议，以及刷新后的同一条记录，并保存 1440px 和 1024px 全页截图。`1` 仅用于覆盖拒判路径。发布报告和模型权重从原路径只读加载。

启用后，配置缺失、推理失败或刷新读回不一致均使验收失败。默认运行 `npm --prefix frontend test` 时，报告将真实推理和低置信度场景记为 `needs_weights`，两档截图使用未配置模型的页面状态。

## 四腔分割的非 A4C 帧验收

四腔分割采用操作员对所选帧提交的切面确认。使用花卉视频时验收会选中具体帧，确认该帧为非 A4C 并检查证据不足的原因、补充影像入口、刷新读回和无叠图。也可在未配置 `ECHO_VERIFY_STANFORD_VIDEO` 的一轮验收中，将 `ECHO_VERIFY_NON_A4C_VIDEO` 指向本机保存的教学视频 S1，期刊标注为胸骨旁长轴切面。验收核对文件的 SHA-256 为 `dddbf2def6a7923c1348fc2ccdb398e08b3a2e414d6ea86e1515349ababe9bff`。[期刊文章](https://pocusjournal.com/article/2021-06-01p10-12/) · [原始视频 S1](https://pocusjournal.com/wp-content/uploads/2021/04/Damji-et-al-Video-S1-PSLX-1.mp4)
