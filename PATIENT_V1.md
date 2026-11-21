# 患儿管理 V1.0 交付

当前隔离浏览器验收使用 `npm --prefix frontend test`，配置见 [工作规则](AGENTS.md)。下文保留 V1.0 交付时的操作记录。

## 范围与路由
在 feature/web-ui 开发，没有合并 main、没有提交或推送，也未改训练代码。
保持现有蓝白样式、侧栏、Button、卡片及 hash 路由。
- #patients：独立列表，业务编号/姓名搜索、真实检查统计、最近检查日期、分页、刷新。
- #patients/new：独立建档，自动或手动业务编号，选填临床资料与带日期事件。
- #patients/:id：独立详情，基本资料/临床病史/检查记录/历史报告。
- 患儿至 #exams/new?patient=:id 的入口保留；检查详情及报告继续使用现有模块。

## 真实业务能力
业务编号是 EE- 加 12 位随机十六进制字符，可录入字母数字连字符，统一大写，数据库唯一约束。
内部主键不变。新档案记录建档时间；旧档案没有可靠建档时间则显示未记录。
保留 history、treatment_history、notes；结构化 clinical_data 存主要问题、诊断、出生胎龄/体重及医疗事件。
每次编辑保存修改前快照及操作者/时间，并沿用操作日志；没有删除患儿接口。
同名且同出生日期的可访问档案会提示人工核对，勾选确认才允许保存，不自动合并。
前端保存锁及禁用按钮防止重复点击；后端唯一约束阻止重复业务编号。
检查和生命体征不迁入患儿永久属性，仍按检查独立保存。
检查年龄沿用创建检查时出生日期快照；修改患儿出生日期不会重新计算旧检查年龄。
历史报告读取 clinical_entries 中真实 report 记录，保留各版本正文；不虚构报告签署或审核信息。

## 修改文件
- frontend/src/Patients.tsx：新增患儿三个页面与表单、详情标签页。
- frontend/src/Modules.tsx：移除旧混合患儿页面，其余模块保留。
- frontend/src/main.tsx：患儿子路由分派。
- frontend/src/shell.css：仅新增 patient-* 范围布局规则。
- backend/clinical.py：兼容原接口，增加业务编号、结构化病史、快照、详情/报告/历史接口。
- backend/migrations/versions/0004_patient_archive.py：新增兼容迁移。
- backend/migrations/versions/0003_parameters.py：增加已有字段判断，解决旧迁移引用当前模型导致新库重复加列问题。
- backend/test_patients.py、frontend/test-patients.cjs、test_patient_migration.py：新增测试。
- frontend/test-exams.cjs：仅更新新患儿路由下的原回归测试入口。
- apply_patient_migration.py：备份、迁移并逐列核对旧数据的本地升级脚本。

## 迁移结果
本地 work/platform.db 已升级到 0004；升级前备份位于同一 work 目录，文件名 platform-before-patient-v1-20261114-121842.db。
验证原有所有表（除迁移版本表）的所有旧列及记录与备份一致，SQLite integrity_check = ok。
没有复制或上传真实影像。备份未放入 Git 仓库。
迁移不提供破坏性 downgrade，需要回退时应先停服务并由维护者处理备份恢复。

## 验证结果
- npm --prefix frontend run build：通过（保留现有 Vite 配置及第三方库打包警告）。
- pytest backend/test_patients.py backend/test_clinical.py backend/test_media.py -q：3 passed。
- 新库完整迁移、模拟 0003 旧库升级：均通过。
- Playwright 患儿测试：建档/编辑/刷新持久化、搜索、分页、重复编号提示、疑似重复提醒、关联创建检查、两次独立检查、真实报告读取及报告跳转通过，未捕获浏览器异常。
- 原检查 Playwright 回归：搜索/筛选/分页、创建检查、参数编辑、BSA、独立保存、历史记录、影像入口、刷新持久化通过。
- 后端权限测试：其他普通用户无权读取/编辑，审核角色只读，重复编号 409，非法出生日期/医疗事件日期/胎龄返回 422。
- 已查看 1440px 桌面详情截图；未执行全部浏览器和全部分辨率兼容矩阵。
- 本地 5176 页面与代理 API 返回 200。

## 启动与测试
正常启动沿用原项目方式，先在 backend 下设置 ECHO_DATABASE_URL 后运行 python -m alembic upgrade head，再启动 uvicorn main:app --host 127.0.0.1 --port 8004；frontend 下运行 npm run dev（5176）。
本地正常运行库为 work/platform.db，backend 下相对 URL 为 sqlite:///../../platform.db。
测试：在项目根目录使用 ../api-venv/Scripts/python.exe test_exam_server.py 启动独立 8005 测试库，然后 node frontend/test-patients.cjs。每次用新测试库运行；原检查测试同理。
浏览器测试将 API 转到隔离库，只创建明确标记的功能测试数据。

## 限制与后续
- 历史报告当前是研究报告文本版本，没有独立签署、报告专属审核和撤回流程。
- 重复提醒仅按可访问档案的姓名+出生日期精确匹配；没有跨权限身份检索或模糊匹配。
- 列表分页在前端进行；大规模数据建议后续增加服务端分页及聚合统计。
- 档案历史快照从本版本启用；旧修改不追补，不构成防篡改医疗审计。
- 历史出生日期快照更正流程、乐观并发控制及请求幂等键尚待后续完善。
- PostgreSQL 实机迁移及生产安全验证未运行；当前验证为 SQLite 开发环境。
- 本系统仍为研究及内部验证系统。
