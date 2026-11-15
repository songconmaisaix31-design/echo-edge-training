import {createRequire} from 'node:module';
import {mkdir, writeFile, readFile} from 'node:fs/promises';
import {resolve, dirname} from 'node:path';

const require = createRequire(new URL('../frontend/package.json', import.meta.url));
const {chromium} = require('playwright');
const origin = process.env.ECHO_EXPORT_ORIGIN || 'http://127.0.0.1:5192';
const username = process.env.ECHO_EXPORT_USER;
const password = process.env.ECHO_EXPORT_PASSWORD;
const output = resolve(process.argv[2] || 'work/html-export/index.html');
if (!username || !password) throw new Error('请设置 ECHO_EXPORT_USER 和 ECHO_EXPORT_PASSWORD。');

const tabs = ['切面识别', '影像质量', '心脏结构', '异常征象'];
const diagnosisTabs = ['AI 诊断结果', '诊断证据', '鉴别与复核', '医生审核'];
const browser = await chromium.launch({channel: 'chrome', headless: true});
const context = await browser.newContext({viewport: {width: 1440, height: 1000}, locale: 'zh-CN'});
const page = await context.newPage();
const assets = {};
const snapshots = [];
const media = {};
const jobs = {};
const css = new Set();
let navigation = 0;
const api = async path => {
  const response = await context.request.get(origin + '/api/v1' + path);
  if (!response.ok()) throw new Error(`读取 ${path} 失败：${response.status()}`);
  return response.json();
};
const asset = async path => {
  if (assets[path]) return;
  const response = await context.request.get(origin + path);
  if (!response.ok()) throw new Error(`读取影像资源失败：${response.status()}`);
  assets[path] = 'data:' + response.headers()['content-type'].split(';')[0] + ';base64,' + (await response.body()).toString('base64');
};
const settle = async () => {
  await page.evaluate(() => new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  await page.waitForLoadState('networkidle');
  await page.locator('.app-shell').waitFor();
  await page.waitForFunction(() => !/正在读取分析记录|正在读取影像…|正在读取关联影像|正在读取检查…|正在加载患儿资料|正在读取模型状态|正在检查影像原件/.test(document.getElementById('root').innerText));
  const alerts = await page.locator('[role=alert]').allTextContents();
  if (alerts.some(text => /读取.*失败|请求失败|服务请求失败/.test(text))) throw new Error(alerts.join('\n'));
};
const navigate = async (route, exam = '', active = '') => {
  await page.evaluate(({exam, active}) => {
    sessionStorage.setItem('echo-exam', String(exam));
    if (exam && active) sessionStorage.setItem('echo-media:' + exam, active);
  }, {exam, active});
  await page.goto(origin + '/?offline-export=' + (++navigation) + '#' + route);
  await settle();
};

async function capture(meta) {
  for (const rule of await page.evaluate(() => [...document.styleSheets].flatMap(sheet => [...sheet.cssRules].map(rule => rule.cssText)))) css.add(rule);
  const result = await page.evaluate(({meta, username, tabs, diagnosisTabs}) => {
    const source = document.getElementById('root');
    const player = source.querySelector('.cine img');
    if (meta.media && player && new URL(player.getAttribute('src'),location.origin).pathname.split('/')[4] !== meta.media) throw new Error('实际影像与导出快照不一致');
    const examSelect = [...source.querySelectorAll('select')].find(select=>['当前检查','选择检查'].includes(select.closest('label')?.childNodes[0]?.textContent.trim()));
    if (meta.exam && examSelect && examSelect.value !== meta.exam) throw new Error('实际检查与导出快照不一致');
    if (meta.route === 'workspace' && source.querySelector('h1')?.textContent !== 'AI影像分析') throw new Error('工作站页面尚未完成切换');
    const clone = source.cloneNode(true);
    const originals = source.querySelectorAll('input,select,textarea');
    clone.querySelectorAll('input,select,textarea').forEach((element, index) => {
      const original = originals[index];
      if (element.tagName === 'SELECT') [...element.options].forEach(option => option.toggleAttribute('selected', option.value === original.value));
      else if (element.tagName === 'TEXTAREA') element.textContent = original.value;
      else element.setAttribute('value', original.value);
      element.disabled = true;
    });
    clone.querySelectorAll('script,input[type=password],input[type=file]').forEach(element => element.remove());
    const userArea = clone.querySelector('.user-area');
    if (userArea) userArea.textContent = '离线只读展示';
    const sidebarFoot = clone.querySelector('.sidebar-foot');
    if (sidebarFoot) sidebarFoot.textContent = '已保存数据快照';
    const walker = document.createTreeWalker(clone, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) walker.currentNode.textContent = walker.currentNode.textContent.replaceAll(username, '导出用户');
    const assetPaths = [];
    for (const image of clone.querySelectorAll('img')) {
      const path = new URL(image.getAttribute('src'), location.origin).pathname;
      if (!path.startsWith('/api/v1/')) throw new Error('页面包含未知图片来源');
      image.dataset.asset = path;
      image.removeAttribute('src');
      assetPaths.push(path);
    }
    for (const cine of clone.querySelectorAll('.cine')) {
      const image = cine.querySelector('img');
      if (!image) continue;
      const id = image.dataset.asset.split('/')[4];
      cine.dataset.player = id;
      for (const button of cine.querySelectorAll('button')) {
        button.dataset.playback = button.textContent.trim();
        button.disabled = false;
      }
      const range = cine.querySelector('input[type=range]');
      if (range) {range.disabled = false; range.dataset.seek = id;}
    }
    for (const button of clone.querySelectorAll('button')) {
      const text = button.textContent.trim();
      if (button.dataset.playback) continue;
      button.disabled = true;
      button.title = '离线快照不执行联网、分析或修改操作';
      const thumbnail = button.closest('.series-list') && button.querySelector('img');
      if (thumbnail) {button.dataset.media = thumbnail.dataset.asset.split('/')[4]; button.disabled = false;}
      if (meta.route === 'workspace' && tabs.includes(text)) {button.dataset.tab = text; button.disabled = false;}
      if (meta.route.startsWith('diagnosis') && diagnosisTabs.includes(text)) {button.dataset.tab = text; button.disabled = false;}
      if (meta.route.startsWith('exams/') && button.getAttribute('role') === 'tab') {button.dataset.tab = text; button.disabled = false;}
      if (meta.route.startsWith('patients/') && button.getAttribute('role') === 'tab') {button.dataset.tab = text; button.disabled = false;}
      if (/^(证据 · )?第 \d+ 帧 ·/.test(text)) {
        button.dataset.evidence = String(Number(text.match(/第 (\d+) 帧/)[1]) - 1);
        button.disabled = false;
      }
      const route = {'继续检查':'workspace','打开影像工作站':'workspace','医生审核':'workspace','影像导入':'media','阅览影像':'media','检查报告':'reports','专家复核':'experts'}[text];
      if (route) {
        button.dataset.route = route;
        button.dataset.exam = button.closest('tr')?.querySelector('td')?.textContent.trim().replace('#','') || String(meta.exam || '');
        button.disabled = false;
      }
      if (!button.disabled) button.removeAttribute('title');
    }
    for (const select of clone.querySelectorAll('select')) {
      const label = select.closest('label')?.childNodes[0]?.textContent.trim();
      if (label === '当前检查' || label === '选择检查') {select.dataset.choose = 'exam'; select.disabled = false;}
      if (label === '选择影像') {select.dataset.choose = 'media'; select.disabled = false;}
      if (label === '分析记录' && meta.route === 'workspace') {select.dataset.choose = 'job'; select.disabled = false;}
      if (select.dataset.choose) select.setAttribute('aria-label',label);
    }
    for (const anchor of clone.querySelectorAll('a')) {
      const href = anchor.getAttribute('href') || '';
      if (!href.startsWith('#') || href.includes('/new') || href.includes('/edit')) {
        anchor.removeAttribute('href'); anchor.setAttribute('aria-disabled', 'true');
        anchor.title = '此操作需要在线工作站，离线快照仅供阅览';
      }
    }
    return {html: clone.innerHTML, assetPaths};
  }, {meta, username, tabs, diagnosisTabs});
  for (const path of result.assetPaths) await asset(path);
  snapshots.push({...meta, html: result.html});
}

try {
  const login = await context.request.post(origin + '/api/v1/login', {data: {name: username, password}});
  if (!login.ok()) throw new Error(`登录失败：${login.status()}`);
  await page.goto(origin);
  await settle();
  const [cases, exams, patients] = await Promise.all([api('/cases'), api('/examinations'), api('/patients')]);
  for (const item of cases) {
    const detail = await api('/cases/' + item.id);
    item.media = detail.media;
    for (const id of detail.media) {
      media[id] = await api('/media/' + id);
      jobs[id] = await api('/media/' + id + '/analysis-jobs');
      for (const frame of media[id].frames) await asset('/api/v1/media/' + id + '/frames/' + frame.index);
      for (const job of jobs[id]) if (job.model_type === 'cardiacudc_fourchamber' && job.state === 'succeeded') await asset('/api/v1/analysis-jobs/' + job.id + '/overlay');
    }
  }
  console.log(JSON.stringify({stage:'真实数据已读取', cases:cases.length, exams:exams.length, patients:patients.length, media:Object.keys(media).length, frames:Object.values(media).reduce((n,item)=>n+item.frameCount,0), jobs:Object.values(jobs).flat().length}));
  for (const route of ['home','patients','exams','settings']) {await navigate(route); await capture({route});}
  for (const patient of patients) {
    const route = 'patients/' + patient.id;
    await navigate(route);
    for (const tab of ['基本资料','临床病史','检查记录','历史报告']) {
      await page.getByRole('tab', {name:tab, exact:true}).click();
      await settle();
      await capture({route,tab});
    }
  }
  for (const exam of exams) {
    const route = 'exams/' + exam.id;
    await navigate(route);
    for (const tab of ['临床参数','超声测量','影像资料','AI分析','审核记录','修改历史']) {
      await page.getByRole('tab', {name:tab, exact:true}).click();
      await settle();
      await capture({route, exam:String(exam.id), tab});
    }
  }
  for (const item of cases) {
    const exam = String(item.id);
    const ids = item.media.length ? item.media : [''];
    for (const route of ['media','workspace','multi','experts','reports']) {
      for (const id of (['media','workspace','multi'].includes(route) ? ids : ids.slice(0,1))) {
        await navigate(route, exam, id);
        if (route !== 'workspace') {await capture({route,exam,media:id}); continue;}
        for (const tab of tabs) {
          await page.getByRole('button', {name:tab, exact:true}).click();
          await settle();
          const selector = page.locator('.medical-review select');
          if (await selector.count()) {
            const values = await selector.locator('option').evaluateAll(options=>options.map(option=>option.value));
            for (const job of values) {await selector.selectOption(job); await settle(); await capture({route,exam,media:id,tab,job});}
          } else await capture({route,exam,media:id,tab});
        }
      }
    }
    if (!exams.some(value => value.id === item.id)) continue;
    for (const id of ids) {
      const route = 'diagnosis/' + exam;
      await navigate(route + (id ? '/analysis/' + id : ''), exam, id);
      for (const tab of diagnosisTabs) {
        await page.getByRole('tab', {name:tab, exact:true}).click();
        await settle();
        await capture({route,exam,media:id,tab});
      }
    }
  }
  const savedJobs = Object.fromEntries(Object.entries(jobs).map(([id,rows])=>[id,rows.map(({id,model_type,state,created,error,output,evidence_frames})=>({id,model_type,state,created,error,output,evidence_frames}))]));
  const data = {created:new Date().toISOString(),cases:cases.map(({id,title,media})=>({id,title,media})),media,jobs:savedJobs,assets,snapshots};
  const serialized = JSON.stringify(data).replaceAll('<','\\u003c');
  const runtime = await readFile(new URL('./export-workstation-runtime.js', import.meta.url), 'utf8');
  const html = await page.evaluate(({serialized, runtime, css}) => {
    const doc = document.implementation.createHTMLDocument('Echo Edge · 已保存数据展示');
    doc.documentElement.lang = 'zh-CN';
    const charset = doc.createElement('meta'); charset.setAttribute('charset','utf-8'); doc.head.prepend(charset);
    const viewport = doc.createElement('meta'); viewport.name='viewport'; viewport.content='width=device-width, initial-scale=1'; doc.head.append(viewport);
    const policy = doc.createElement('meta'); policy.httpEquiv='Content-Security-Policy'; policy.content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; connect-src 'none'; font-src data:; base-uri 'none'; form-action 'none'"; doc.head.append(policy);
    const style = doc.createElement('style'); style.textContent=css.join('\n')+'\n.offline-banner{padding:12px 24px;background:#fff8db;color:#634c06;border-bottom:1px solid #e9dca4;font-size:13px;line-height:1.6}.offline-banner strong{margin-right:12px}.offline-banner button{margin-left:12px;text-decoration:underline}.offline-evidence{margin-top:16px}.offline-evidence img{max-width:100%;max-height:440px}.offline-empty{padding:40px}[aria-disabled=true]{cursor:not-allowed;opacity:.6}.offline-banner+main{margin-top:0}'; doc.head.append(style);
    const root=doc.createElement('div');root.id='root';doc.body.append(root);
    const state=doc.createElement('script');state.type='application/json';state.id='offline-data';state.textContent=serialized;doc.body.append(state);
    const script=doc.createElement('script');script.textContent=runtime;doc.body.append(script);
    return '<!doctype html>\n'+doc.documentElement.outerHTML;
  }, {serialized,runtime,css:[...css]});
  if (html.includes(password) || html.includes(username) || /(?:C:|D:)[\\/]|echo_session|Bearer |sk-[A-Za-z0-9]{12}/.test(html)) throw new Error('导出内容包含不允许交付的凭据或内部路径');
  await mkdir(dirname(output), {recursive:true});
  await writeFile(output, html, 'utf8');
  console.log(JSON.stringify({stage:'HTML已生成', output, bytes:Buffer.byteLength(html), snapshots:snapshots.length, images:Object.keys(assets).length}));
} finally {
  await browser.close();
}
