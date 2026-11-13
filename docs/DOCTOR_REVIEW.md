# 20 段儿童心超人工复核

`review_echonet.py` 按 A4C、PSAX 与原始 `Split 0–9` 的组合，各随机选取一段。默认随机种子为 `42`。程序读取 ZIP，检查选中 AVI 的解码和帧时间，再打乱复核编号的排列。医生独立填写切面和质量。医生表保留 `REV` 编号，发布方切面和原始 `Split` 保留在院内索引。

## 生成复核批次

在仓库根目录使用 [README](../README.md) 配好的 Python 环境。`$archive` 指向已取得访问权的原始 ZIP。

```powershell
$python = '.\.venv\Scripts\python.exe'
& $python -X utf8 .\review_echonet.py prepare --archive $archive `
  --index '.\work\echo-review-private.json' --review-input '.\work\echo-review-input.json'
```

`build_review_workbook.mjs` 使用 Node.js 和 Codex 工作区的 `@oai/artifact-tool`，读取 `echo-review-input.json` 并生成 Excel。复核表只使用 `review_id` 和批次编号，医生判断字段留空。若 Node.js 尚无法解析该包，把 `$artifactModules` 设为 Codex 工作区依赖返回的 Node 模块目录，在仓库根目录建立本地 junction。

```powershell
if (!(Test-Path .\node_modules)) {
  New-Item -ItemType Junction -Path .\node_modules -Target $artifactModules | Out-Null
}
node .\build_review_workbook.mjs '.\work\echo-review-input.json' '.\work\医生复核.xlsx'
```

已获访问授权的项目联系人可按编号查找对应的 ZIP 内路径。查询结果含源文件名，留在授权环境。依据 Stanford 公开协议，使用团队内的其他参与者各自注册并遵守许可，转交数据副本需书面许可。获得项目所需访问授权后，按 `REV` 编号安排回看。 [Stanford 研究使用协议](https://echonet.github.io/pediatric/)

```powershell
& $python -X utf8 .\review_echonet.py locate --index '.\work\echo-review-private.json' --review-id REV-01
```

## 医生填写和回收检查

两位医生可以各填一份。每行填写一个编号。整段适用同一判断时，起止两格留空。只评一段时，以首帧为零点填写毫秒半开区间。先独立判断切面和采集质量，后复核分歧。采集质量标准未定或无法判断时可留空并说明原因。

```powershell
& $python -X utf8 .\review_echonet.py check --index '.\work\echo-review-private.json' `
  --workbook '.\work\医生复核.xlsx' --report '.\work\echo-review-check.json'
```

报告给出已确认和待复核数量，并定位非法选项、编号缺失、重复和无帧时间区间所在行。训练仍按独立入口的数据和授权要求执行。
