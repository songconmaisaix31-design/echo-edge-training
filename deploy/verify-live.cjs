const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { parseArgs } = require('node:util');
const { chromium } = require('playwright');

const { values } = parseArgs({ options: {
  url: { type: 'string', default: 'https://echo.davidwang.space' },
  user: { type: 'string', default: 'integration-operator' },
  patient: { type: 'string' },
  'source-exam': { type: 'string' },
  'source-url': { type: 'string', default: 'http://127.0.0.1:5192' },
  video: { type: 'string' },
  exam: { type: 'string' },
  'read-only': { type: 'boolean', default: false },
  output: { type: 'string', default: 'work/live-verification.json' },
} });

async function main() {
  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const context = await browser.newContext();
  const page = await context.newPage();
  page.setDefaultTimeout(30000);
  page.setDefaultNavigationTimeout(60000);
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const report = { url: values.url, checkedAt: new Date().toISOString(), jobs: [] };
  async function loadedImage(locator) {
    await locator.waitFor();
    await locator.evaluate(image => new Promise((resolve, reject) => {
      if (image.complete) return image.naturalWidth > 0 ? resolve() : reject(new Error('图像加载失败'));
      image.addEventListener('load', () => resolve(), { once: true });
      image.addEventListener('error', () => reject(new Error('图像加载失败')), { once: true });
    }));
  }
  const endpoint = route => new URL('/api/v1' + route, values.url).href;
  async function read(route) {
    const response = await context.request.get(endpoint(route));
    assert.equal(response.status(), 200, route + ': ' + await response.text());
    return response.json();
  }
  async function post(route, data) {
    const response = await context.request.post(endpoint(route), { data, timeout: 300000 });
    assert.ok(response.ok(), route + ': ' + await response.text());
    return response.json();
  }
  try {
    await page.goto(values.url);
    await page.getByLabel('账号', { exact: true }).fill(values.user);
    await page.getByLabel('密码', { exact: true }).fill(process.env.ECHO_VERIFICATION_PASSWORD);
    const login = page.waitForResponse(response => response.url() === endpoint('/login'));
    await page.getByRole('button', { name: '登录', exact: true }).click();
    assert.equal((await login).status(), 200);
    await page.getByRole('button', { name: '退出登录' }).waitFor();
    report.login = await read('/me');
    const cookie = (await context.cookies()).find(value => value.name === 'echo_session');
    assert.ok(cookie?.secure && cookie.httpOnly && cookie.sameSite === 'Strict');
    report.secureSession = true;
    const capabilities = await read('/capabilities');
    assert.equal(capabilities.structureConfigured, true);
    assert.equal(capabilities.reviewConfigured, true);
    assert.equal(capabilities.reviewModel, 'deepseek-flash');
    await page.goto(values.url + '/#settings');
    const structureStatus = page.locator('.record').filter({ has: page.getByText('结构分割', { exact: true }) });
    await structureStatus.getByText('已接入', { exact: true }).waitFor();
    report.settings = '结构分割已接入';

    let examId = values.exam;
    let patientInput;
    if (values.patient) patientInput = JSON.parse(await fs.readFile(values.patient, 'utf8'));
    if (values['source-exam']) {
      assert.equal(patientInput, undefined, '只允许一个档案来源');
      const source = await browser.newContext();
      try {
        const loginResponse = await source.request.post(values['source-url'] + '/api/v1/login', {
          data: { name: values.user, password: process.env.ECHO_VERIFICATION_PASSWORD },
        });
        assert.equal(loginResponse.status(), 200);
        const examsResponse = await source.request.get(values['source-url'] + '/api/v1/examinations');
        assert.equal(examsResponse.status(), 200);
        const sourceExam = (await examsResponse.json()).find(item => String(item.id) === values['source-exam']);
        assert.ok(sourceExam, '本地来源检查不存在');
        const patientResponse = await source.request.get(values['source-url'] + '/api/v1/patients/' + sourceExam.patient_id);
        assert.equal(patientResponse.status(), 200);
        const sourcePatient = await patientResponse.json();
        assert.match([sourcePatient.name, sourcePatient.notes, sourceExam.kind, sourceExam.reason].join(' '), /联调|验收|测试|演示|公开/, '仅允许已有明确联调档案');
        patientInput = Object.fromEntries(['name', 'birth', 'sex', 'notes', 'history', 'treatment_history', 'clinical'].map(key => [key, sourcePatient[key]]));
        report.sourceExam = Number(values['source-exam']);
      } finally {
        try {
          await source.request.post(values['source-url'] + '/api/v1/logout');
        } finally {
          await source.close();
        }
      }
    }
    if (patientInput) {
      assert.ok(values.video, '导入检查必须指定实际视频');
      const patient = await post('/patients', patientInput);
      const exam = await post('/examinations', { patient_id: patient.id, kind: '公网部署验收',
        reason: '公开教学视频链路验收，视频与本地联调档案的身份信息无关联。用于登录、四腔分割、DeepSeek质量和异常征象分析的数据流验证。https://pocusjournal.com/article/2021-06-01p10-12/' });
      examId = String(exam.id);
    }
    if (values.video && !values['read-only']) {
      assert.ok(examId, '视频必须导入正式检查');
      await page.goto(values.url + '/#media');
      await page.locator('.context-bar select').first().selectOption(examId);
      const uploaded = page.waitForResponse(response => response.url() === endpoint('/media') && response.request().method() === 'POST', { timeout: 300000 });
      await page.locator('input[type=file]').setInputFiles(values.video);
      const upload = await uploaded;
      assert.equal(upload.status(), 201, await upload.text());
      const imported = await upload.json();
      report.upload = { frameCount: imported.frameCount, media: imported.id };
    }

    if (examId) {
      report.exam = Number(examId);
      await page.goto(values.url + '/#workspace');
      await page.locator('.context-bar select').first().selectOption(examId);
      await page.getByRole('button', { name: '心脏结构', exact: true }).click();
      await page.locator('.diagnosis-card .context-bar select').selectOption('1');
      if (!values['read-only']) {
        const responsePromise = page.waitForResponse(response => response.url().endsWith('/structure-jobs') && response.request().method() === 'POST', { timeout: 300000 });
        await page.getByRole('button', { name: '运行四腔分割', exact: true }).click();
        const response = await responsePromise;
        assert.equal(response.status(), 201);
        const job = await response.json();
        assert.equal(job.state, 'succeeded', JSON.stringify(job));
        assert.equal(job.output.epoch, 18);
        report.jobs.push({ id: job.id, model: job.model_type, state: job.state, pixels: job.output.foreground_pixels });
      }
      const overlay = page.getByAltText('LV 蓝色、LA 绿色、RA 橙色、RV 紫色的四腔结构叠图');
      await loadedImage(overlay);
      report.overlay = true;

      for (const [label, kind] of [['影像质量', 'quality'], ['异常征象', 'findings']]) {
        await page.getByRole('button', { name: label, exact: true }).click();
        const panel = page.getByRole('article', { name: label + '大模型复核' });
        if (!values['read-only']) {
          const responsePromise = page.waitForResponse(response => response.url().endsWith('/review-jobs') && response.request().method() === 'POST', { timeout: 300000 });
          await panel.getByRole('button', { name: '运行' + label + '分析', exact: true }).click();
          const response = await responsePromise;
          assert.equal(response.status(), 201);
          const job = await response.json();
          assert.equal(job.state, 'succeeded', JSON.stringify(job));
          assert.equal(job.output.kind, kind);
          assert.equal(job.output.model, 'deepseek-flash');
          report.jobs.push({ id: job.id, model: job.model_type, state: job.state, frames: job.output.sampled_frames.length });
        }
        await panel.locator('.medical-review-result').waitFor();
        await panel.getByText('查看送入模型的采样帧', { exact: true }).click();
        await panel.getByRole('button', { name: /^第 \d+ 帧/ }).first().click();
        await loadedImage(panel.locator('figure img'));
      }
      const history = await read('/examinations/' + examId + '/analysis-jobs');
      const saved = history.filter(job => ['cardiacudc_fourchamber', 'llm_quality', 'llm_findings'].includes(job.model_type) && job.state === 'succeeded');
      assert.equal(new Set(saved.map(job => job.model_type)).size, 3);
      report.persistedJobIds = saved.map(job => job.id);
      await page.reload();
      await page.locator('.context-bar select').first().selectOption(examId);
      for (const label of ['心脏结构', '影像质量', '异常征象']) {
        await page.getByRole('button', { name: label, exact: true }).click();
        if (label === '心脏结构') await loadedImage(overlay);
        else await page.getByRole('article', { name: label + '大模型复核' }).locator('.medical-review-result').waitFor();
      }
      report.refreshReadback = true;
      const anonymous = await browser.newContext();
      for (const route of ['/me', '/examinations/' + examId + '/analysis-jobs', '/analysis-jobs/' + saved.find(job => job.model_type === 'cardiacudc_fourchamber').id + '/overlay', '/panel/state']) {
        assert.equal((await anonymous.request.get(endpoint(route))).status(), 401, route);
      }
      await anonymous.close();
      report.anonymousAccess = 401;
    }
    assert.deepEqual(errors, []);
    report.pageErrors = errors;
    await fs.mkdir(path.dirname(values.output), { recursive: true });
    await fs.writeFile(values.output, JSON.stringify(report, null, 2));
    console.log(JSON.stringify(report, null, 2));
  } finally {
    try {
      await context.request.post(endpoint('/logout'), { timeout: 10000 });
    } finally {
      await browser.close();
    }
  }
}

main().catch(error => { console.error(error.message.split('\n')[0]); process.exitCode = 1; });
