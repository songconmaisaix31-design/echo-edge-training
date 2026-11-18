# Echo Edge 工作规则

- 保持研究用途。AI 输出只能提供辅助判断，需保留证据、适用范围和不确定性。证据不足时提示下一步。报告由医生审核签发。危急病例按临床流程立即处理。
- 患者资料、数据库、影像、凭证和受限资产不得进入 Git。浏览器测试使用隔离的 SQLite、独立媒体目录和 `fixtures/SOURCES.md` 登记的视频。外部影像复核默认关闭。
- 不启用自动训练或自动发布。保留训练、评价和应用的边界。项目性能数字须有可追溯的评估记录、样本量及统计单位。第三方结果标注来源。
- 避免解释代码表面行为的注释。能由类型、Lint、导入约束或真实操作测试表达的约定写进对应检查。页面经 `frontend/src/api.ts` 请求数据，由应用入口组合。共享组件放进独立模块。
- 保留既有哈希路由、已存数据和可用的工作流程。外部响应从 `unknown` 开始校验错误结构。用判别联合表示业务状态，对适用的渲染分支进行穷尽检查。
- 本机直接进入模式仅在 `127.0.0.1` 运行，使用 `work/local-workstation/` 内的数据库和媒体目录。共享部署采用现有会话与角色检查。
- 公开首页只读取单独发布、逐项获准的四份脱敏档案和五段影像。每次发布保持固定快照，样例文件不得进入 Git。生产数据库、媒体目录和模型写入接口继续使用会话与角色检查。
- 主线工作站不增加终端、床旁面板或串口依赖。历史固件与验收记录等待独立归档。

## 提交前验证

首次准备运行环境时安装依赖。

```text
python -m pip install -r backend/requirements.txt
npm --prefix frontend ci
npm --prefix frontend exec -- playwright install chromium
```

每次改动后运行类型检查、Lint、构建、后端测试和浏览器流程。

```text
npm --prefix frontend run typecheck
npm --prefix frontend run lint
npm --prefix frontend run build
python -m pytest backend -q
npm --prefix frontend test
```

`ECHO_VERIFY_PYTHON` 可指向已安装后端依赖的 Python。`scripts/verify.mjs` 用空闲端口启动 Uvicorn 和 Vite，再由 Playwright 操作真实页面。用户入口变更须验证对应操作路径。退出码非零即验收失败，`work/verify/report.json` 记录实际执行结果和未覆盖项。旧检查和患儿脚本保留供补齐历史场景时参考。

## 后续任务

| Issues | 交付条件 |
| --- | --- |
| [#23](https://github.com/songconmaisaix31-design/echo-edge-training/issues/23)、[#24](https://github.com/songconmaisaix31-design/echo-edge-training/issues/24)、[#25](https://github.com/songconmaisaix31-design/echo-edge-training/issues/25)、[#26](https://github.com/songconmaisaix31-design/echo-edge-training/issues/26) | 补齐后端 Lint 和导入约束、前端 `strictTypeChecked`、基于 FastAPI 响应模型的 OpenAPI 类型生成和漂移检查。重设计合并后启用完整样式变量规则。补齐获准影像及权重的推理、证据不足、结果读回和门禁失败用例。 |
| [#27](https://github.com/songconmaisaix31-design/echo-edge-training/issues/27)–[#30](https://github.com/songconmaisaix31-design/echo-edge-training/issues/30) | 由专家确认协议及数据要求，交付覆盖状态、证据不足和多切面评估。 |
| [#31](https://github.com/songconmaisaix31-design/echo-edge-training/issues/31)–[#34](https://github.com/songconmaisaix31-design/echo-edge-training/issues/34) | 交付结构化报告、审核版本关联、专家状态规则和受控反馈。 |
| [#35](https://github.com/songconmaisaix31-design/echo-edge-training/issues/35)–[#42](https://github.com/songconmaisaix31-design/echo-edge-training/issues/42) | 维护数据许可与评估记录，补齐隐私和临床约束的可执行验证。 |
| [#7](https://github.com/songconmaisaix31-design/echo-edge-training/issues/7)–[#11](https://github.com/songconmaisaix31-design/echo-edge-training/issues/11) | 等待授权原始数据、评估协议和独立验证，再进行训练与导出。 |
| [#12](https://github.com/songconmaisaix31-design/echo-edge-training/issues/12)、[#43](https://github.com/songconmaisaix31-design/echo-edge-training/issues/43)、[#46](https://github.com/songconmaisaix31-design/echo-edge-training/issues/46) | 由负责人决定旧设备联动任务；按归档结果更新三份历史文档的卷首说明。 |
