import assert from 'node:assert/strict'
import {createHash, randomBytes} from 'node:crypto'
import {mkdir, mkdtemp, readFile, stat, writeFile} from 'node:fs/promises'
import {basename, join, relative, resolve} from 'node:path'
import {checkAnalysisContract} from '../frontend/scripts/check-analysis-contract.mjs'
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
const nonA4CVideo = process.env.ECHO_VERIFY_NON_A4C_VIDEO
const multiviewDir = process.env.ECHO_VERIFY_MULTIVIEW_DIR
const plaxSha256 = 'dddbf2def6a7923c1348fc2ccdb398e08b3a2e414d6ea86e1515349ababe9bff'
const multiviewUncovered = '用公开教学影像确认两种切面并补齐证据'
const stanfordUncovered = '有权重的切面推理和结果读回'
const insufficientUncovered = '切面置信度不足与结果读回'
const screenshotsUncovered = '1440px 和 1024px 截图'
const [fault, ...extraArgs] = process.argv.slice(2)
const faultModes = ['--fault=http-500', '--fault=page-error']
const faultName = faultModes.includes(fault) ? fault.slice('--fault='.length) : null

const report = {
  status: 'failed',
  passed: [],
  browserErrors: [],
  faultProbe: faultName,
  screenshots: [],
  stanfordInference: {status: stanfordVideo ? 'failed' : 'needs_weights'},
  insufficientEvidence: {status: stanfordVideo ? 'failed' : 'needs_weights'},
  uncovered: [
    stanfordUncovered,
    insufficientUncovered,
    screenshotsUncovered,
    multiviewUncovered,
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
  const {checkpoint_sha256, labels, preprocessing, sampling, aggregation, policy} = release
  const runtime = {checkpoint_sha256, labels, preprocessing, sampling, aggregation, policy}
  // 推理服务只读取验收目录内的策略副本。权重仍从原路径只读加载。
  const runtimePath = join(run, 'stanford-runtime.json')
  await writeFile(runtimePath, JSON.stringify(runtime), 'utf8')
  files.ECHO_ENGINE_REPORT = runtimePath
  Object.assign(env, files, {ECHO_INFERENCE_SCRIPT: join(root, 'infer_stanford.py')})
  return {fixturePath, inputSha256, modelVersion: `fp32:${checkpoint_sha256}`, runtime, runtimePath}
}

async function checkInsufficientNotice(panel, evidence) {
  const result = panel.getByRole('status').filter({hasText: evidence.message})
  await result.getByText('证据不足', {exact: true}).waitFor()
  await result.getByText(evidence.message, {exact: true}).waitFor()
  await result.getByText(evidence.next_step, {exact: true}).waitFor()
  await result.getByRole('link', {name: '补充影像', exact: true}).waitFor()
  return result
}

async function checkViewResult(page, job) {
  const {state, output, insufficient_evidence: evidence} = job
  const insufficient = state === 'insufficient_evidence'
  await page.getByText(insufficient ? '分析状态：证据不足' : '分析状态：成功', {exact: true}).waitFor()
  const result = insufficient ? await checkInsufficientNotice(page, evidence) : page.getByRole('status').filter({hasText: 'AI 切面结论'})
  const viewNames = {A4C: '心尖四腔心切面', PSAX: '胸骨旁短轴切面'}
  if (insufficient) {
    assert.equal(evidence.reason, 'quality')
    assert.doesNotMatch(await result.innerText(), /模型判断这段视频更符合/)
    assert.equal(await page.getByText('AI 切面结论', {exact: true}).count(), 0)
  } else {
    assert.equal(Object.hasOwn(job, 'insufficient_evidence'), false)
    await result.getByText(`模型判断这段视频更符合${viewNames[output.view]}。`, {exact: true}).waitFor()
  }
  if (await result.locator('details[open]').count() === 0) {
    await result.getByText('查看模型详情', {exact: true}).click()
  }
  const a4c = (output.probabilities.A4C * 100).toFixed(1)
  const psax = (output.probabilities.PSAX * 100).toFixed(1)
  await result.getByText(`抽样 ${output.sampled_frames} 帧 · A4C ${a4c}% · PSAX ${psax}%`, {exact: true}).waitFor()
}

async function runStanfordInference(page, origin, examId, media, stanford, previousJobs) {
  const historyUrl = `${origin}/api/v1/media/${media.id}/analysis-jobs`
  await page.getByText(previousJobs.length ? '分析状态：成功' : '分析状态：暂无记录', {exact: true}).waitFor()
  const [response] = await Promise.all([
    page.waitForResponse(item => item.url() === historyUrl && item.request().method() === 'POST', {timeout: 200_000}),
    page.getByRole('button', {name: '执行切面识别', exact: true}).click(),
  ])
  assert.equal(response.status(), 201)
  const job = await response.json()
  assert.equal(job.state, previousJobs.length ? 'insufficient_evidence' : 'succeeded', job.error || '切面推理状态不符')
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
  await checkViewResult(page, job)

  const [readback] = await Promise.all([
    page.waitForResponse(item => item.url() === historyUrl && item.request().method() === 'GET'),
    page.reload(),
  ])
  assert.equal(readback.status(), 200)
  assert.deepEqual(await readback.json(), [job, ...previousJobs])
  await checkViewResult(page, job)
  return job
}

async function checkStanfordInference(page, origin, examId, media, stanford) {
  const job = await runStanfordInference(page, origin, examId, media, stanford, [])
  const {output} = job
  report.stanfordInference = {
    status: 'passed', jobId: job.id, modelVersion: job.model_version, inputSha256: job.input_sha256,
    precision: output.precision, view: output.view, sampledFrames: output.sampled_frames,
  }
  report.uncovered = report.uncovered.filter(item => item !== stanfordUncovered)
  report.passed.push('Stanford 真实切面推理，重新加载页面后从分析历史读回同一任务和结果')

  const threshold = 1
  const {runtime, runtimePath} = stanford
  const testPolicy = {...runtime.policy, min_confidence: {...runtime.policy.min_confidence, view: threshold}}
  try {
    await writeFile(runtimePath, JSON.stringify({...runtime, policy: testPolicy}), 'utf8')
    const abstained = await runStanfordInference(page, origin, examId, media, stanford, [job])
    assert.equal(abstained.output.view, null, '置信度不足时应拒绝判定切面')
    const confidence = Math.max(...Object.values(abstained.output.probabilities))
    assert.ok(confidence < threshold)
    report.insufficientEvidence = {status: 'passed', jobId: abstained.id, state: abstained.state, reason: abstained.insufficient_evidence.reason, view: null, threshold, confidence}
    report.uncovered = report.uncovered.filter(item => item !== insufficientUncovered)
    report.passed.push('切面置信度低于隔离验收阈值，页面显示证据不足及复核建议，刷新读回同一结果')
  } finally {
    await writeFile(runtimePath, JSON.stringify(runtime), 'utf8')
  }
}

async function checkNonA4CFrame(page, origin, examId, media, inputSha256) {
  await page.goto(`${origin}/#diagnosis/${examId}/structure/${media.id}`)
  const panel = page.getByRole('tabpanel')
  const frame = panel.getByLabel('选择证据帧')
  const frameView = panel.getByLabel('所选帧切面')
  await frameView.selectOption('A4C')
  await frame.selectOption('1')
  assert.equal(await frameView.inputValue(), 'unconfirmed')
  await panel.getByRole('img', {name: '所选证据帧 2', exact: true}).waitFor()
  const mediaUrl = `${origin}/api/v1/media/${media.id}`
  let job
  for (const [view, reason] of [['unconfirmed', 'coverage'], ['non_a4c', 'out_of_scope']]) {
    await frameView.selectOption(view)
    const [response] = await Promise.all([
      page.waitForResponse(item => item.url() === mediaUrl + '/structure-jobs' && item.request().method() === 'POST'),
      panel.getByRole('button', {name: '运行四腔分割', exact: true}).click(),
    ])
    assert.equal(response.status(), 201)
    job = await response.json()
    assert.equal(job.state, 'insufficient_evidence')
    assert.equal(job.error, null)
    assert.equal(job.insufficient_evidence.reason, reason)
    assert.equal(job.input_sha256, inputSha256)
    assert.equal(job.output.frame_view, view)
    assert.deepEqual(job.evidence_frames, [{media_id: media.id, frame_index: 1, timestamp_ms: media.frames[1].offsetMs}])
    await checkInsufficientNotice(panel, job.insufficient_evidence)
    assert.equal(await panel.getByRole('heading', {name: '四腔结构叠图'}).count(), 0)
    assert.equal((await page.request.get('/api/v1/analysis-jobs/' + job.id + '/overlay')).status(), 404)
  }
  const [readback] = await Promise.all([
    page.waitForResponse(item => item.url() === mediaUrl + '/analysis-jobs' && item.request().method() === 'GET'),
    page.reload(),
  ])
  assert.equal(readback.status(), 200)
  assert.deepEqual((await readback.json())[0], job)
  assert.deepEqual(await (await page.request.get('/api/v1/analysis-jobs/' + job.id)).json(), job)
  await checkInsufficientNotice(panel, job.insufficient_evidence)
  assert.equal(await frame.inputValue(), '1')
  assert.equal(await frameView.inputValue(), 'non_a4c')
  await checkPage(page, '非 A4C 帧的分割证据不足')
  report.nonA4CFrame = {jobId: job.id, state: job.state, reason: job.insufficient_evidence.reason, frameIndex: 1, inputSha256}
  report.passed.push('真实非 A4C 帧提交后返回适用范围不足，无叠图，刷新后读回同一帧及结果')
}

async function registeredMultiviewVideos(directory) {
  if (!directory) return []
  const fixtures = [
    ['A4C', 'Damji-et-al-Video-S3-A4C-TV-Endo-1.mp4', '543c6a330eda334e498d8bb2ee7880a46a899c660b5b669d7613ee746d4a9d6c'],
    ['PLAX', 'Damji-et-al-Video-S1-PSLX-1.mp4', plaxSha256],
  ]
  return Promise.all(fixtures.map(async ([view, name, sha256]) => {
    const file = resolve(root, directory, name)
    assert.equal(createHash('sha256').update(await readFile(file)).digest('hex'), sha256, '多切面验收须使用登记的教学视频')
    return {view, file}
  }))
}

async function checkMultiview(page, origin, {examId, otherExamId, media, clips}) {
  const responsePath = id => '/api/v1/examinations/' + id + '/multiview'
  const panel = page.getByRole('region', {name: '多切面评估', exact: true})
  const picker = page.getByRole('combobox', {name: '当前检查', exact: true})
  const required = ['A4C', 'PLAX']
  const labels = {
    A4C: '心尖四腔心切面（A4C）',
    PLAX: '胸骨旁长轴切面（PLAX）',
  }
  const selectExam = async id => {
    await picker.locator(`option[value="${id}"]`).waitFor({state: 'attached'})
    if (await picker.inputValue() !== id) await picker.selectOption(id)
  }
  const read = async () => {
    const response = await page.request.get(responsePath(examId))
    assert.equal(response.status(), 200)
    return response.json()
  }
  const save = async () => {
    const [response] = await Promise.all([
      page.waitForResponse(result => result.url() === origin + responsePath(examId) && result.request().method() === 'PUT'),
      panel.getByRole('button', {name: '保存本次评估', exact: true}).click(),
    ])
    assert.equal(response.status(), 200)
    return response.json()
  }
  const assertCoverage = async (result, covered) => {
    assert.deepEqual(result.required_views, required)
    assert.deepEqual(result.coverage, required.map(view => ({view, status: covered.includes(view) ? 'covered' : 'missing'})))
    assert.equal(result.state, covered.length === required.length ? 'succeeded' : 'insufficient_evidence')
    assert.equal(Object.hasOwn(result, 'disease_candidates'), false)
    if (result.state === 'insufficient_evidence') {
      assert.equal(result.insufficient_evidence.reason, 'coverage')
      assert.equal(Object.hasOwn(result, 'findings'), false)
      await checkInsufficientNotice(panel, result.insufficient_evidence)
    } else {
      await panel.getByText('必要切面均已有确认帧。', {exact: true}).waitFor()
    }
    for (const {view, status} of result.coverage) {
      await panel.getByText(labels[view] + ' · ' + (status === 'covered' ? '已有确认帧' : '缺少确认帧'), {exact: true}).waitFor()
    }
  }

  await page.goto('/#multi')
  await selectExam(examId)
  await panel.getByText('本次检查尚未指定多切面评估所需的切面。').waitFor()
  assert.equal(await panel.getByRole('link', {name: '补充影像'}).count(), 0)
  for (const view of required) await panel.getByRole('checkbox', {name: labels[view], exact: true}).check()
  const missing = await save()
  await assertCoverage(missing, [])
  await page.reload()
  await assertCoverage(missing, [])
  assert.deepEqual(await read(), missing)
  await checkPage(page, '多切面评估证据不足')
  assert.equal((await page.request.put(responsePath(examId), {
    data: {revision: 0, required_views: [], frames: []},
  })).status(), 409)

  await panel.getByRole('link', {name: '补充影像', exact: true}).click()
  await page.getByRole('heading', {name: '导入影像', exact: true}).waitFor()
  await picker.locator(`option[value="${examId}"]`).waitFor({state: 'attached'})
  assert.equal(await picker.inputValue(), examId)
  const uploads = []
  for (const {view, file} of clips) {
    await page.waitForFunction(() => document.querySelector('input[type=file]')?.disabled === false)
    const [response] = await Promise.all([
      page.waitForResponse(result => result.url() === origin + '/api/v1/media' && result.request().method() === 'POST'),
      page.getByLabel('选择视频文件', {exact: true}).setInputFiles(file),
    ])
    assert.equal(response.status(), 201)
    uploads.push({view, media: await response.json()})
    await page.waitForFunction(() => document.querySelector('input[type=file]')?.disabled === false)
  }
  await page.goto('/#multi')
  await selectExam(examId)
  const confirmed = []
  for (const entry of uploads) {
    const {view, media: clip} = entry
    await panel.getByRole('combobox', {name: '证据影像', exact: true}).selectOption(clip.id)
    const viewPicker = panel.getByRole('combobox', {name: '确认切面', exact: true})
    await viewPicker.selectOption(view)
    await panel.getByRole('combobox', {name: '原始帧', exact: true}).selectOption('1')
    assert.equal(await viewPicker.inputValue(), '')
    await viewPicker.selectOption(view)
    await page.waitForFunction(() => document.querySelector('img[alt="待确认原始帧 2"]')?.naturalWidth > 0)
    await panel.getByRole('button', {name: '添加已确认帧', exact: true}).click()
    confirmed.push({view, media_id: clip.id, frame_index: 1, timestamp_ms: clip.frames[1].offsetMs})
    const result = await save()
    await assertCoverage(result, confirmed.map(frame => frame.view))
    assert.deepEqual(result.frames, confirmed)
    await panel.getByRole('link', {name: '查看' + labels[view] + ' · 第 2 帧 · ' + clip.frames[1].offsetMs + ' ms', exact: true}).waitFor()
    await page.reload()
    await assertCoverage(result, confirmed.map(frame => frame.view))
    assert.deepEqual(await read(), result)
  }
  if (uploads.length) {
    const covered = await read()
    assert.deepEqual(covered.findings, [])
    report.uncovered = report.uncovered.filter(item => item !== multiviewUncovered)
    report.passed.push('两段真实教学影像逐帧确认，先拒判再补齐，刷新读回完整覆盖')
  }

  await selectExam(otherExamId)
  await panel.getByText('本次检查尚未指定多切面评估所需的切面。').waitFor()
  assert.equal(await panel.getByRole('list', {name: '切面覆盖'}).count(), 0)
  assert.equal((await page.request.put(responsePath(otherExamId), {
    data: {revision: 0, required_views: ['A4C'], frames: [{view: 'A4C', media_id: media.id, frame_index: 0}]},
  })).status(), 422)
  report.passed.push('多切面评估缺少必要切面时给出证据不足和补充入口，刷新、并发修改和切换检查隔离通过')
}

async function checkDiagnosisScreenshots(page, stanford) {
  const scenario = stanford ? 'insufficient_evidence' : 'needs_weights'
  await page.waitForFunction(() => document.querySelector('img[alt="源帧 1"]')?.naturalWidth > 0)
  for (const viewport of [{width: 1440, height: 960}, {width: 1024, height: 768}]) {
    await page.setViewportSize(viewport)
    if (stanford) await page.getByText('证据不足', {exact: true}).waitFor()
    await checkPage(page, `辅助诊断（${viewport.width}px）`)
    await page.evaluate(() => window.scrollTo({top: 0, left: 0, behavior: 'instant'}))
    const path = join(run, `diagnosis-${viewport.width}.png`)
    await page.screenshot({path, fullPage: true, animations: 'disabled', scale: 'css'})
    report.screenshots.push({scenario, viewport, file: relative(base, path).replaceAll('\\', '/')})
  }
  report.uncovered = report.uncovered.filter(item => item !== screenshotsUncovered)
  report.passed.push(stanford
    ? '1440px 和 1024px 证据不足页面截图及可访问性检查'
    : '未配置模型时禁用运行，完成两种窗口宽度的页面截图及可访问性检查')
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

async function checkWorkstation(origin, stanford, clips) {
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
  assert.equal((await page.request.get('/api/v1/examinations/1/multiview')).status(), 401)
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
  const uploadFile = stanford?.fixturePath ?? (nonA4CVideo ? resolve(root, nonA4CVideo) : fixture)
  const inputSha256 = createHash('sha256').update(await readFile(uploadFile)).digest('hex')
  if (!stanford && nonA4CVideo) assert.equal(inputSha256, plaxSha256, '非 A4C 验收须使用登记的 S1 视频')
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
    : '上传已登记的公开视频，解码、逐帧、播放及刷新读回')

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
  await checkDiagnosisScreenshots(page, stanford)
  if (!stanford) await checkNonA4CFrame(page, origin, examId, media, inputSha256)
  await checkMultiview(page, origin, {examId, otherExamId, media, clips})

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
  await checkAnalysisContract(python)
  const stanford = stanfordVideo ? await configureStanford(stanfordVideo) : null
  const clips = await registeredMultiviewVideos(multiviewDir)
  const operator = {name: user, password}
  await prepareDatabase(python, env, operator)
  const service = startApi(python, env, data => { apiLog += data })
  api = service.child
  const target = await service.ready
  web = await createWeb({target, port: 0, cacheDir: join(run, 'shared-vite')})
  const origin = await webOrigin(web)
  const workstation = await checkWorkstation(origin, stanford, clips)
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
