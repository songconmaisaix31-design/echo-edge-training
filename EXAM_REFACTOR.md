# 检查管理重构与验收

本次在 feature/web-ui 分支完成，保留原有视觉及哈希路由技术。

## 页面
- `#exams`：独立表格列表，编号/患儿搜索、业务状态和类型筛选、每页 10 条。
- `#exams/new`：分组双列表单。`?patient=编号` 用于患儿详情入口预选。
- `#exams/编号`：独立详情，临床概览及按需编辑、超声测量、影像资料、AI分析、审核记录、修改历史。
- 影像、工作站、专家及报告入口使用现有检查上下文。

## 数据
使用既有 examinations / examination_parameters 表，不需要数据库结构迁移。
临床备注保存到参数版本 JSON 的 clinical_notes 字段，旧数据回退显示 reason。
参数保存追加版本，不覆盖旧版本；列表展示最新参数版本。
年龄使用建档快照出生日期，BSA 继续由后端 Mosteller 计算；前端提供实时预览。
检查类型及患儿在创建后只读；本次编辑范围为检查临床参数。
AI 和 Z-score 不生成虚构输出。未接入模型状态与检查业务状态独立展示。

## 验证
- `npm --prefix frontend run build`：通过。
- `../api-venv/Scripts/python.exe -m pytest backend/test_clinical.py backend/test_media.py -q`：2 passed。
- Playwright 浏览器验收：通过搜索、状态/类型筛选、分页、列表新建、患儿入口新建、详情、编辑、实时BSA、历史版本、多次检查独立保存、影像导入上下文、刷新持久化。
- 1440px 详情页截图已人工查看。

## 重跑浏览器验收
在项目根目录运行 `../api-venv/Scripts/python.exe test_exam_server.py`（占用 8005，使用系统临时目录中的全新测试库）。
保持现有 Vite 在 5176 运行，然后执行 `node frontend/test-exams.cjs`。
测试浏览器将 API 路由转向 8005；只写入明确标识为测试的资料，不修改业务数据库。
每次重跑需重启测试服务器以获得全新临时库。截图输出到 exam-detail-test.png。

本轮验证影像导入入口与当前检查关联，未使用真实患者影像进行上传；现有媒体解析测试通过。
没有修改 AI 训练代码、原始影像、原有用户密码或真实病例。
