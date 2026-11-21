# 浏览器验证视频

`flower.mp4` 是 MDN 示例中的花卉视频，来源目录标记为 CC0。验证仅使用视频上传、解码、播放和刷新后的持久化状态。视频不含患者影像。

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

真实推理使用 PyTorch CPU FP32，验收运行在独立 SQLite、媒体目录和本机端口。启用后，配置缺失、推理失败或刷新读回不一致均使验收失败。默认运行 `npm --prefix frontend test` 时，`stanfordInference.status` 记录为 `needs_weights`，真实推理保留在未覆盖项中。
