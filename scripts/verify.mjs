import assert from 'node:assert/strict'
import {spawn} from 'node:child_process'
import {randomBytes} from 'node:crypto'
import {once} from 'node:events'
import {mkdir, mkdtemp, writeFile} from 'node:fs/promises'
import {createRequire} from 'node:module'
import {join} from 'node:path'
import {fileURLToPath, pathToFileURL} from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
const frontend = join(root, 'frontend')
const backend = join(root, 'backend')
const require = createRequire(join(frontend, 'package.json'))
const {chromium} = require('playwright')
const {createServer} = await import(pathToFileURL(require.resolve('vite')).href)
const fixture = join(root, 'fixtures', 'flower.mp4')
const base = join(root, 'work', 'verify')
await mkdir(base, {recursive: true})
const run = await mkdtemp(join(base, 'run-'))
const python = process.env.ECHO_VERIFY_PYTHON || (process.platform === 'win32' ? 'python' : 'python3')
const user = 'verify-operator'
const password = randomBytes(18).toString('base64url')
const env = Object.fromEntries(Object.entries(process.env).filter(([name]) => !/^ECHO_|^PYTHONPATH$/i.test(name)))
Object.assign(env, {
  ECHO_DATABASE_URL: `sqlite:///${join(run, 'verify.db').replaceAll('\\', '/')}`,
  ECHO_MEDIA_ROOT: join(run, 'media'),
  ECHO_VERIFY_USER: user,
  ECHO_VERIFY_PASSWORD: password,
  PYTHONUTF8: '1',
  TMP: run,
  TEMP: run,
  TMPDIR: run,
})

const report = {
  status: 'failed',
  passed: [],
  browserErrors: [],
  uncovered: [
    '有权重的切面推理和结果读回',
    '证据不足变体（#29）',
    '患儿查重、列表分页与历史报告导出',
    '验收门禁的 HTTP 500 和页面异常负向用例',
    '截图',
  ],
}
let api
let web
let browser
let apiLog = ''

async function runPython(args) {
  const child = spawn(python, args, {cwd: backend, env, stdio: 'inherit', windowsHide: true})
  const [code] = await once(child, 'exit')
  assert.equal(code, 0, `Python 执行失败：${args.join(' ')}`)
}

function startApi() {
  api = spawn(python, ['-m', 'uvicorn', 'main:app', '--host', '127.0.0.1', '--port', '0', '--no-use-colors'], {
    cwd: backend, env, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true,
  })
  api.stdout.on('data', data => { apiLog += data })
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('后端 30 秒内未启动')), 30_000)
    api.once('error', error => { clearTimeout(timeout); reject(error) })
    api.once('exit', code => { clearTimeout(timeout); reject(new Error(`后端退出：${code}`)) })
    api.stderr.on('data', data => {
      apiLog += data
      const match = apiLog.match(/Uvicorn running on (http:\/\/127\.0\.0\.1:\d+)/)
      if (match) {
        clearTimeout(timeout)
        resolve(match[1])
      }
    })
  })
}

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
      .map(({id, nodes}) => ({id, targets: nodes.map(({target}) => target)}))
  })
  assert.deepEqual(violations, [], `${name} 存在严重的可访问性问题`)
}

async function checkWorkstation(origin) {
  browser = await chromium.launch({headless: true})
  const page = await browser.newPage({baseURL: origin, viewport: {width: 1440, height: 960}})
  const visit = hash => page.goto(`${origin}/#${hash}`)
  const button = name => page.getByRole('button', {name, exact: true})
  const link = name => page.getByRole('link', {name, exact: true})
  const heading = name => page.getByRole('heading', {name, exact: true})
  const label = name => page.getByLabel(name, {exact: true})
  await page.addInitScript({path: require.resolve('axe-core/axe.min.js')})

  observePage(page, origin)
  assert.equal((await page.request.get('/api/v1/cases')).status(), 401)
  const capabilities = await page.request.get('/api/v1/capabilities')
  assert.equal(capabilities.status(), 200)
  const features = await capabilities.json()
  assert.equal(features.model, 'unavailable')
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
  await page.waitForFunction(selector => document.querySelector(selector)?.disabled === false, uploadSelector)
  const [uploadResponse] = await Promise.all([
    page.waitForResponse(response => response.url() === `${origin}/api/v1/media`
      && response.request().method() === 'POST'),
    page.locator(uploadSelector).setInputFiles(fixture),
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
  assert.equal((await persisted.json()).originalName, 'flower.mp4')
  report.passed.push('上传公开非患者视频、解码、逐帧、播放及刷新读回')

  const patientId = patientUrl.split('/').at(-1)
  const examId = examUrl.split('/').at(-1)
  await visit(`exams/new?patient=${patientId}`)
  await label('检查类型 *').fill('隔离验收')
  await label('检查日期 *').fill('2024-02-01')
  await button('创建检查').click()
  await heading('检查详情').waitFor()
  const otherExamId = page.url().split('/').at(-1)
  assert.notEqual(otherExamId, examId)
  await visit(`diagnosis/${examId}`)
  await button('执行切面识别').waitFor()
  assert.equal(await button('执行切面识别').isDisabled(), true)
  assert.equal(await page.getByRole('button', {name: /发送.*终端/}).count(), 0)
  const examPicker = page.getByRole('combobox', {name: /^选择检查/})
  await examPicker.selectOption(otherExamId)
  await page.getByText('当前检查尚无影像。', {exact: true}).waitFor()
  assert.equal(await page.getByRole('img', {name: '源帧 1', exact: true}).count(), 0)
  await examPicker.selectOption(examId)
  await button('执行切面识别').waitFor()
  report.passed.push('切换检查后只显示当前检查的影像和分析入口')
  await checkPage(page, '辅助诊断（1440px）')
  await page.setViewportSize({width: 1024, height: 768})
  await checkPage(page, '辅助诊断（1024px）')
  report.passed.push('未配置模型时禁用运行；两种窗口宽度通过可访问性检查')

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
}

try {
  await runPython(['-m', 'alembic', 'upgrade', 'head'])
  await runPython([join(root, 'scripts', 'verify-bootstrap.py')])
  const target = await startApi()
  web = await createServer({root: frontend, server: {
    host: '127.0.0.1', port: 0, strictPort: true, proxy: {'/api': target},
  }})
  await web.listen()
  const address = web.httpServer.address()
  assert.ok(address && typeof address === 'object')
  await checkWorkstation(`http://127.0.0.1:${address.port}`)
  report.status = 'passed'
} catch (error) {
  report.error = error instanceof Error ? error.message : String(error)
  console.error(error)
  process.exitCode = 1
} finally {
  const closeResults = await Promise.allSettled([browser?.close(), web?.close()])
  const cleanupErrors = closeResults.filter(result => result.status === 'rejected')
  if (cleanupErrors.length) {
    report.status = 'failed'
    report.cleanupErrors = cleanupErrors.map(result => String(result.reason))
    process.exitCode = 1
  }
  if (api?.pid && api.exitCode === null && api.signalCode === null) {
    const exited = once(api, 'exit')
    api.kill()
    await exited
  }
  await writeFile(join(run, 'api.log'), apiLog)
  const summary = JSON.stringify(report, null, 2) + '\n'
  await writeFile(join(run, 'report.json'), summary)
  await writeFile(join(base, 'report.json'), summary)
  console.log(`验证报告 ${join(run, 'report.json')}`)
}
