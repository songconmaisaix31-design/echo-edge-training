import assert from 'node:assert/strict'
import {createHash, randomBytes} from 'node:crypto'
import {mkdir, mkdtemp, readFile, stat, writeFile} from 'node:fs/promises'
import {basename, join, resolve} from 'node:path'
import {createWeb, isolatedEnvironment, prepareDatabase, requireFrontend, root, startApi, stopApi} from './workstation-services.mjs'

const {chromium} = requireFrontend('playwright')
const fixture = join(root, 'fixtures', 'flower.mp4')
const base = join(root, 'work', 'verify')
await mkdir(base, {recursive: true})
const run = await mkdtemp(join(base, 'run-'))
const python = process.env.ECHO_VERIFY_PYTHON || (process.platform === 'win32' ? 'python' : 'python3')
const user = 'verify-operator'
const password = randomBytes(18).toString('base64url')
const env = isolatedEnvironment(run, 'verify.db')
const stanfordVideo = process.env.ECHO_VERIFY_STANFORD_VIDEO
const stanfordUncovered = '有权重的切面推理和结果读回'
const [fault, ...extraArgs] = process.argv.slice(2)
const faultModes = ['--fault=http-500', '--fault=page-error']
const faultName = faultModes.includes(fault) ? fault.slice('--fault='.length) : null

const report = {
  status: 'failed',
  passed: [],
  browserErrors: [],
  faultProbe: faultName,
  stanfordInference: {status: stanfordVideo ? 'failed' : 'needs_weights'},
  uncovered: [
    stanfordUncovered,
    '证据不足变体（#29）',
    '截图',
  ],
}
let api
let web
let localWeb
let browser
let apiLog = ''

function observePage(page, origin) {
  page.on('pageerror', error => report.browserErrors.push(error.message))
  page.on('response', response => {
    if (response.status() >= 500) report.browserErrors.push(`${response.status()} ${response.url()}`)
    if (response.status() === 401 && response.url() !== `${origin}/api/v1/me`) {
      report.browserErrors.push(`401 ${response.url()}`)
    }
  })
  page.on('console', message => {
    if (message.type() !== 'error') return
    const isExpectedSessionCheck = message.location().url === `${origin}/api/v1/me`
      && /status of 401\b/.test(message.text())
    if (!isExpectedSessionCheck) report.browserErrors.push(message.text())
  })
}

async function checkPage(page, name) {
  const displayedText = await page.locator('body').innerText()
  assert.doesNotMatch(displayedText, /\b(?:undefined|NaN)\b|\[object Object\]/)
  const violations = await page.evaluate(async () => {
    const result = await window.axe.run({runOnly: ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']})
    return result.violations.filter(({impact}) => impact === 'serious' || impact === 'critical')
      .map(({id, nodes}) => ({id, nodes: nodes.map(({target, failureSummary}) => ({target, failureSummary}))}))
  })
  assert.deepEqual(violations, [], `${name} 存在严重的可访问性问题`)
}

async function configureStanford(video) {
  const files = {}
  for (const name of ['ECHO_INFERENCE_PYTHON', 'ECHO_ENGINE_REPORT', 'ECHO_STANFORD_CHECKPOINT']) {
    assert.ok(process.env[name], `执行真实切面验收需要配置 ${name}。`)
    const path = resolve(root, process.env[name])
    assert.ok((await stat(path)).isFile(), `${name} 须指向已有文件。`)
    files[name] = path
  }
  const fixturePath = resolve(root, video)
  assert.ok((await stat(fixturePath)).isFile(), 'ECHO_VERIFY_STANFORD_VIDEO 须指向已登记的视频文件。')
  const inputSha256 = createHash('sha256').update(await readFile(fixturePath)).digest('hex')
  const release = JSON.parse((await readFile(files.ECHO_ENGINE_REPORT, 'utf8')).replace(/^\uFEFF/, ''))
  assert.match(release.checkpoint_sha256, /^[a-f0-9]{64}$/)
  Object.assign(env, files, {ECHO_INFERENCE_SCRIPT: join(root, 'infer_stanford.py')})
  return {fixturePath, inputSha256, modelVersion: `fp32:${release.checkpoint_sha256}`}
}

async function checkViewResult(page, output) {
  await page.getByText('分析状态：成功', {exact: true}).waitFor()
  const result = page.getByRole('status').filter({hasText: 'AI 切面结论'})
  const viewNames = {A4C: '心尖四腔心切面', PSAX: '胸骨旁短轴切面'}
  const summary = output.view === null
    ? '模型未达到判定阈值，暂无法确认这段视频的切面。'
    : `模型判断这段视频更符合${viewNames[output.view]}。`
  await result.getByText(summary, {exact: true}).waitFor()
  await result.getByText('查看模型详情', {exact: true}).click()
  const a4c = (output.probabilities.A4C * 100).toFixed(1)
  const psax = (output.probabilities.PSAX * 100).toFixed(1)
  await result.getByText(`抽样 ${output.sampled_frames} 帧 · A4C ${a4c}% · PSAX ${psax}%`, {exact: true}).waitFor()
}

async function checkStanfordInference(page, origin, examId, media, stanford) {
  const historyUrl = `${origin}/api/v1/media/${media.id}/analysis-jobs`
  await page.getByText('分析状态：暂无记录', {exact: true}).waitFor()
  const [response] = await Promise.all([
    page.waitForResponse(item => item.url() === historyUrl && item.request().method() === 'POST', {timeout: 200_000}),
    page.getByRole('button', {name: '执行切面识别', exact: true}).click(),
  ])
  assert.equal(response.status(), 201)
  const job = await response.json()
  assert.equal(job.state, 'succeeded', job.error || '切面推理未成功')
  assert.equal(job.error, null)
  assert.equal(job.model_type, 'stanford_view')
  assert.equal(job.model_version, stanford.modelVersion)
  assert.equal(job.input_sha256, stanford.inputSha256)
  assert.equal(job.exam_id, Number(examId))
  assert.equal(job.media_id, media.id)
  assert.ok(Number.isInteger(job.id) && job.id > 0)
  assert.ok(job.completed)
  const {output} = job
  assert.equal(output.precision, 'fp32')
  assert.ok([null, 'A4C', 'PSAX'].includes(output.view))
  assert.ok(Number.isInteger(output.sampled_frames) && output.sampled_frames > 0 && output.sampled_frames <= media.frameCount)
  await checkViewResult(page, output)

  const [readback] = await Promise.all([
    page.waitForResponse(item => item.url() === historyUrl && item.request().method() === 'GET'),
    page.reload(),
  ])
  assert.equal(readback.status(), 200)
  assert.deepEqual(await readback.json(), [job])
  await checkViewResult(page, output)

  report.stanfordInference = {
    status: 'passed', jobId: job.id, modelVersion: job.model_version, inputSha256: job.input_sha256,
    precision: output.precision, view: output.view, sampledFrames: output.sampled_frames,
  }
  report.uncovered = report.uncovered.filter(item => item !== stanfordUncovered)
  report.passed.push('Stanford 真实切面推理，重新加载页面后从分析历史读回同一任务和结果')
}

async function checkReportHistory(page, origin, {patientUrl, examUrl, otherExamUrl}) {
  const openReport = async url => {
    await page.goto(url)
    await page.getByRole('button', {name: '检查报告', exact: true}).click()
    return url.split('/').at(-1)
  }
  const saveReport = async (examId, text) => {
    await page.getByLabel('记录内容', {exact: true}).fill(text)
    const [response] = await Promise.all([
      page.waitForResponse(item => item.url() === `${origin}/api/v1/cases/${examId}/entries`
        && item.request().method() === 'POST'),
      page.getByRole('button', {name: '保存记录', exact: true}).click(),
    ])
    assert.equal(response.status(), 201)
    await page.getByText(text, {exact: true}).waitFor()
  }
  const otherText = '另一检查的隔离验收草稿。'
  const reportText = '当前检查的隔离验收草稿。'
  const otherId = await openReport(otherExamUrl)
  await saveReport(otherId, otherText)
  const examId = await openReport(examUrl)
  await saveReport(examId, reportText)

  await page.reload()
  await page.getByText(reportText, {exact: true}).waitFor()
  assert.equal(await page.getByText(otherText, {exact: true}).count(), 0)
  await page.goto(patientUrl)
  await page.getByRole('tab', {name: '历史报告', exact: true}).click()
  const record = page.getByRole('article').filter({hasText: `检查 #${examId} · 报告日期`})
  await record.getByText('查看此报告版本', {exact: true}).click()
  await record.getByText(reportText, {exact: true}).waitFor()
  await record.getByRole('button', {name: '进入该检查报告', exact: true}).click()
  await page.getByRole('heading', {name: '报告中心', exact: true}).waitFor()
  await page.getByText(reportText, {exact: true}).waitFor()
  assert.equal(await page.getByRole('combobox', {name: '当前检查', exact: true}).inputValue(), examId)

  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('link', {name: '导出检查记录 TXT', exact: true}).click(),
  ])
  assert.equal(download.suggestedFilename(), `examination-${examId}.txt`)
  const exported = await readFile(await download.path(), 'utf8')
  assert.ok(exported.includes(reportText))
  assert.ok(!exported.includes(otherText))
  report.passed.push('报告草稿保存、刷新读回、患儿历史关联及 TXT 导出按检查隔离')
}

async function checkPagination(page) {
  const rows = page.locator('tbody tr')
  await page.getByText('第 1 / 2 页 · 每页 10 条', {exact: true}).waitFor()
  assert.equal(await rows.count(), 10)
  await page.getByRole('button', {name: '下一页', exact: true}).click()
  await page.getByText('第 2 / 2 页 · 每页 10 条', {exact: true}).waitFor()
  assert.equal(await rows.count(), 1)
}

async function checkPatientAndExamLists(page, patientId) {
  await page.goto('/#patients/new')
  await page.getByLabel('姓名或脱敏标识 *', {exact: true}).fill('工作站验收记录')
  await page.getByLabel('出生日期 *', {exact: true}).fill('2020-01-01')
  await page.getByText('发现同名且出生日期相同的可访问档案，请人工核对。', {exact: true}).waitFor()
  const save = page.getByRole('button', {name: '保存患儿档案', exact: true})
  assert.equal(await save.isDisabled(), true)
  await page.getByRole('checkbox', {name: /^已核对，仍需保存独立档案/}).check()
  assert.equal(await save.isEnabled(), true)
  await page.getByLabel('患儿业务编号', {exact: true}).fill('VERIFY-001')
  await page.getByText('业务编号已存在，请核对。', {exact: true}).waitFor()
  assert.equal(await save.isDisabled(), true)

  // 分页记录通过正式接口写入隔离数据库，只使用验收编号。
  for (let index = 2; index <= 11; index++) {
    const response = await page.request.post('/api/v1/patients', {
      data: {name: `分页验收记录 ${index}`, birth: '2020-01-01', sex: 'unknown'},
    })
    assert.equal(response.status(), 201)
  }
  await page.goto('/#patients')
  await checkPagination(page)
  await page.getByLabel('搜索患儿', {exact: true}).fill('VERIFY-001')
  await page.getByRole('cell', {name: 'VERIFY-001', exact: true}).waitFor()
  assert.equal(await page.locator('tbody tr').count(), 1)
  report.passed.push('患儿按姓名和生日查重、业务编号查重、搜索与分页')

  for (let index = 0; index < 9; index++) {
    const response = await page.request.post('/api/v1/examinations', {
      data: {patient_id: Number(patientId), kind: '分页验收', parameters: {exam_date: '2024-01-01'}},
    })
    assert.equal(response.status(), 201)
  }
  await page.goto('/#exams')
  await checkPagination(page)
  await page.getByLabel('筛选检查类型', {exact: true}).selectOption('界面验收')
  await page.getByRole('cell', {name: '界面验收', exact: true}).waitFor()
  const state = page.getByLabel('筛选检查状态', {exact: true})
  await state.selectOption('reviewed')
  assert.equal(await page.locator('tbody tr').count(), 0)
  await state.selectOption('draft')
  assert.equal(await page.locator('tbody tr').count(), 1)
  report.passed.push('检查类型和状态筛选、列表分页')
}

async function triggerGateFailure(origin, {page, examId, mediaId}) {
  if (fault === '--fault=page-error') {
    const [error] = await Promise.all([
      page.waitForEvent('pageerror', {timeout: 5000}),
      page.evaluate(() => setTimeout(() => { throw new Error('verify: uncaught page error') }, 0)),
    ])
    assert.equal(error.message, 'verify: uncaught page error')
    return
  }

  // 仅破坏本轮导入副本的元数据，让正式接口自身返回 HTTP 500。
  const metadata = join(run, 'media', mediaId, 'metadata.json')
  const original = await readFile(metadata)
  try {
    await writeFile(metadata, '{', 'utf8')
    const [response] = await Promise.all([
      page.waitForResponse(item => item.url() === `${origin}/api/v1/media/${mediaId}`
        && item.request().method() === 'GET'),
      page.goto(`/#diagnosis/${examId}`),
    ])
    assert.equal(response.status(), 500)
  } finally {
    await writeFile(metadata, original)
  }
}

async function checkWorkstation(origin, stanford) {
  browser = await chromium.launch({headless: true, downloadsPath: join(run, 'downloads')})
  const page = await browser.newPage({baseURL: origin, viewport: {width: 1440, height: 960}})
  const visit = hash => page.goto(`${origin}/#${hash}`)
  const button = name => page.getByRole('button', {name, exact: true})
  const link = name => page.getByRole('link', {name, exact: true})
  const heading = name => page.getByRole('heading', {name, exact: true})
  const label = name => page.getByLabel(name, {exact: true})
  await page.addInitScript({path: requireFrontend.resolve('axe-core/axe.min.js')})

  observePage(page, origin)
  assert.equal((await page.request.get('/api/v1/cases')).status(), 401)
  assert.equal((await page.request.post('/api/v1/local-session', {headers: {Origin: origin}})).status(), 404)
  const capabilities = await page.request.get('/api/v1/capabilities')
  assert.equal(capabilities.status(), 200)
  const features = await capabilities.json()
  assert.equal(features.model, stanford ? 'Stanford 切面模型' : 'unavailable')
  assert.equal(features.reviewConfigured, false)
  assert.equal(features.structureConfigured, false)
  report.passed.push('未登录访问被拒绝，外部复核关闭')

  await visit('patients')
  await label('账号').fill(user)
  await label('密码').fill(password)
  await button('登录').click()
  await link('新建患儿').click()
  await label('姓名或脱敏标识 *').fill('工作站验收记录')
  await label('患儿业务编号').fill('VERIFY-001')
  await label('出生日期 *').fill('2020-01-01')
  await button('保存患儿档案').click()
  await heading('患儿详情').waitFor()
  const patientUrl = page.url()
  await button('编辑档案').click()
  await label('其他重要病史').fill('隔离验收编辑记录。')
  await button('保存患儿档案').click()
  await page.getByRole('tab', {name: '临床病史', exact: true}).click()
  await page.getByText('隔离验收编辑记录。', {exact: true}).waitFor()
  await page.reload()
  await page.getByRole('tab', {name: '临床病史', exact: true}).click()
  await page.getByText('隔离验收编辑记录。', {exact: true}).waitFor()
  report.passed.push('患儿档案编辑及刷新读回')
  await checkPage(page, '患儿详情')
  await link('创建检查').click()
  await label('检查类型 *').fill('界面验收')
  await label('检查日期 *').fill('2024-01-01')
  await label('身高/身长（cm）').fill('100')
  await label('体重（kg）').fill('36')
  await page.getByText('BSA：1.0000 m² · Mosteller', {exact: true}).waitFor()
  await button('创建检查').click()
  await heading('检查详情').waitFor()
  const examUrl = page.url()
  await button('编辑参数').click()
  await label('体重（kg）').fill('9')
  await button('保存检查参数').click()
  await page.getByText('0.5000', {exact: true}).waitFor()
  await page.reload()
  await page.getByText('0.5000', {exact: true}).waitFor()
  await page.getByRole('tab', {name: '修改历史', exact: true}).click()
  await page.getByText(/BSA 1.0000/).waitFor()
  report.passed.push('浏览器登录、建档、建检查、保存参数与刷新读回')

  await button('导入影像').click()
  await heading('导入影像').waitFor()
  const uploadSelector = 'input[type=file]'
  const uploadFile = stanford?.fixturePath ?? fixture
  await page.waitForFunction(selector => document.querySelector(selector)?.disabled === false, uploadSelector)
  const [uploadResponse] = await Promise.all([
    page.waitForResponse(response => response.url() === `${origin}/api/v1/media`
      && response.request().method() === 'POST'),
    page.locator(uploadSelector).setInputFiles(uploadFile),
  ])
  assert.equal(uploadResponse.status(), 201)
  const media = await uploadResponse.json()
  assert.ok(media.frameCount > 2)
  assert.equal(media.original.exists, true)
  await page.getByRole('img', {name: '源帧 1', exact: true}).waitFor()
  await page.waitForFunction(() => document.querySelector('img[alt="源帧 1"]')?.naturalWidth > 0)
  await button('下一帧').click()
  await page.getByRole('img', {name: '源帧 2', exact: true}).waitFor()
  await button('播放').click()
  await page.waitForFunction(() => {
    const alt = document.querySelector('.cine-image img')?.alt
    return Number(alt?.match(/^源帧 (\d+)$/)?.[1]) > 2
  })
  await button('暂停').click()
  await page.reload()
  await page.getByRole('img', {name: '源帧 1', exact: true}).waitFor()
  const persisted = await page.request.get(`/api/v1/media/${media.id}`)
  assert.equal(persisted.status(), 200)
  assert.equal((await persisted.json()).originalName, basename(uploadFile))
  report.passed.push(stanford
    ? '上传公开教学视频、解码、逐帧、播放及刷新读回'
    : '上传公开非患者视频、解码、逐帧、播放及刷新读回')

  const patientId = patientUrl.split('/').at(-1)
  const examId = examUrl.split('/').at(-1)
  await visit(`exams/new?patient=${patientId}`)
  await label('检查类型 *').fill('隔离验收')
  await label('检查日期 *').fill('2024-02-01')
  await button('创建检查').click()
  await heading('检查详情').waitFor()
  const otherExamUrl = page.url()
  const otherExamId = otherExamUrl.split('/').at(-1)
  assert.notEqual(otherExamId, examId)
  await visit(`diagnosis/${examId}`)
  await button('执行切面识别').waitFor()
  if (!stanford) assert.equal(await button('执行切面识别').isDisabled(), true)
  assert.equal(await page.getByRole('button', {name: /发送.*终端/}).count(), 0)
  const examPicker = page.getByRole('combobox', {name: /^选择检查/})
  await examPicker.selectOption(otherExamId)
  await page.getByText('当前检查尚无影像。', {exact: true}).waitFor()
  assert.equal(await page.getByRole('img', {name: '源帧 1', exact: true}).count(), 0)
  await examPicker.selectOption(examId)
  await button('执行切面识别').waitFor()
  report.passed.push('切换检查后只显示当前检查的影像和分析入口')
  if (stanford) await checkStanfordInference(page, origin, examId, media, stanford)
  await checkPage(page, '辅助诊断（1440px）')
  await page.setViewportSize({width: 1024, height: 768})
  await checkPage(page, '辅助诊断（1024px）')
  report.passed.push(stanford
    ? '两种窗口宽度通过可访问性检查'
    : '未配置模型时禁用运行；两种窗口宽度通过可访问性检查')

  const patientReload = page.waitForRequest(request => request.url() === `${origin}/api/v1/patients/${patientId}`)
  await page.goto(patientUrl)
  await patientReload
  await button('退出登录').click()
  await label('账号').waitFor()
  assert.equal((await page.request.get('/api/v1/cases')).status(), 401)
  await label('账号').fill(user)
  await label('密码').fill(password)
  await button('登录').click()
  await heading('基本资料').waitFor()
  assert.equal((await page.request.get('/api/v1/cases')).status(), 200)
  assert.deepEqual(report.browserErrors, [])
  report.passed.push('患儿详情加载期间注销，会话失效，重新登录后恢复；页面没有运行错误或 HTTP 5xx')
  assert.equal((await page.request.get('/api/v1/panel/state')).status(), 404)
  assert.equal((await page.request.post('/api/v1/analysis-jobs/1/terminal')).status(), 404)
  assert.equal(Object.hasOwn(features, 'terminalConfigured'), false)
  report.passed.push('旧终端接口和能力字段已移除')
  await checkReportHistory(page, origin, {patientUrl, examUrl, otherExamUrl})
  await checkPatientAndExamLists(page, patientId)
  return {page, examId, mediaId: media.id}
}

async function webOrigin(server) {
  await server.listen()
  const address = server.httpServer.address()
  assert.ok(address && typeof address === 'object')
  return `http://127.0.0.1:${address.port}`
}

async function checkLocalEntry(origin) {
  const page = await browser.newPage({baseURL: origin})
  observePage(page, origin)
  assert.equal((await page.request.get('/api/v1/cases')).status(), 401)
  assert.equal((await page.request.post('/api/v1/local-session')).status(), 403)
  assert.equal((await page.request.post('/api/v1/local-session', {headers: {Origin: 'http://example.invalid'}})).status(), 403)

  const newPatient = page.getByRole('link', {name: '新建患儿', exact: true})
  const patientList = page.getByRole('heading', {name: /^患儿档案 · \d+$/})
  await page.goto('/#patients')
  await newPatient.waitFor()
  await patientList.waitFor()
  assert.equal(await page.getByLabel('账号', {exact: true}).count(), 0)
  assert.equal(await page.getByRole('button', {name: /^(登录|退出登录)$/}).count(), 0)
  const identity = await page.request.get('/api/v1/me')
  assert.equal(identity.status(), 200)
  assert.deepEqual(await identity.json(), {name: user, role: 'operator'})
  assert.equal((await page.request.get('/api/v1/cases')).status(), 200)
  const cookie = (await page.context().cookies(origin)).find(value => value.name === 'echo_session')
  assert.ok(cookie?.httpOnly && cookie.sameSite === 'Strict')

  await page.reload()
  await patientList.waitFor()
  const reused = (await page.context().cookies(origin)).find(value => value.name === 'echo_session')
  assert.ok(reused?.value === cookie.value, '刷新后应复用本机会话。')
  assert.deepEqual(report.browserErrors, [])
  report.passed.push('本机浏览器直接进入患儿管理，刷新复用操作员会话，跨来源和无来源请求均被拒绝')
}

try {
  assert.ok(extraArgs.length === 0 && (!fault || faultModes.includes(fault)), '验收参数无效')
  const stanford = stanfordVideo ? await configureStanford(stanfordVideo) : null
  const operator = {name: user, password}
  await prepareDatabase(python, env, operator)
  const service = startApi(python, env, data => { apiLog += data })
  api = service.child
  const target = await service.ready
  web = await createWeb({target, port: 0, cacheDir: join(run, 'shared-vite')})
  const origin = await webOrigin(web)
  const workstation = await checkWorkstation(origin, stanford)
  localWeb = await createWeb({target, port: 0, cacheDir: join(run, 'local-vite'), operator})
  await checkLocalEntry(await webOrigin(localWeb))
  if (fault) await triggerGateFailure(origin, workstation)
  report.status = 'passed'
} catch (error) {
  report.error = error instanceof Error ? error.message : String(error)
  console.error(error)
  process.exitCode = 1
} finally {
  const closeResults = await Promise.allSettled([browser?.close()])
  closeResults.push(...await Promise.allSettled([web?.close(), localWeb?.close()]))
  closeResults.push(...await Promise.allSettled([stopApi(api)]))
  const cleanupErrors = closeResults.filter(result => result.status === 'rejected')
  if (cleanupErrors.length) {
    report.status = 'failed'
    report.cleanupErrors = cleanupErrors.map(result => String(result.reason))
    process.exitCode = 1
  }
  if (report.browserErrors.length > 0) {
    report.status = 'failed'
    process.exitCode = 1
  }
  await writeFile(join(run, 'api.log'), apiLog)
  const summary = JSON.stringify(report, null, 2) + '\n'
  await writeFile(join(run, 'report.json'), summary)
  if (!fault) await writeFile(join(base, 'report.json'), summary)
  else if (faultName) await writeFile(join(base, `${faultName}.json`), summary)
  console.log(`验证报告 ${join(run, 'report.json')}`)
}
