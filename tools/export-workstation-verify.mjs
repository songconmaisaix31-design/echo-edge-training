import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {readFile} from 'node:fs/promises';

const require=createRequire(new URL('../frontend/package.json',import.meta.url));
const {chromium}=require('playwright');
const input=process.argv[2]||'work/html-export/index.html';
const remote=/^https?:/.test(input);
const url=remote?input:pathToFileURL(resolve(input)).href;
// 远程单文件含全部影像，可通过环境变量调整导航上限；交互断言保持 30 秒。
const navigationTimeout=remote?Number(process.env.ECHO_EXPORT_NAVIGATION_TIMEOUT_MS||120000):30000;
assert(Number.isInteger(navigationTimeout)&&navigationTimeout>=1000&&navigationTimeout<=600000,'ECHO_EXPORT_NAVIGATION_TIMEOUT_MS 必须是 1000 至 600000 的整数');
if(!remote){
  const raw=await readFile(resolve(input),'utf8');
  assert(!/(?:C:|D:)[\\/]|echo_session|Bearer |sk-[A-Za-z0-9]{12}/.test(raw));
  if(process.env.ECHO_EXPORT_PASSWORD)assert(!raw.includes(process.env.ECHO_EXPORT_PASSWORD));
  if(process.env.ECHO_EXPORT_USER)assert(!raw.includes(process.env.ECHO_EXPORT_USER));
}
const browser=await chromium.launch({channel:'chrome',headless:true});
const context=await browser.newContext({viewport:{width:1440,height:1000},locale:'zh-CN',offline:!remote});
const page=await context.newPage();
page.setDefaultTimeout(30000);
page.setDefaultNavigationTimeout(navigationTimeout);
const errors=[],network=[];
page.on('pageerror',error=>errors.push(error.message));
page.on('request',request=>{if(/^https?:/.test(request.url())&&!(remote&&request.isNavigationRequest()&&new URL(request.url()).origin===new URL(url).origin))network.push(request.url());});
const checked=[];
const navigations=[];
async function navigate(label,action) {
  const started=performance.now();
  let completed=false;
  try {await action();completed=true;}
  finally {
    const record={label,completed,elapsedMs:Math.round(performance.now()-started),timeoutMs:navigationTimeout};
    navigations.push(record);
    console.log(JSON.stringify({navigation:record}));
  }
}
async function images() {await page.waitForFunction(()=>[...document.querySelectorAll('img[src]')].every(image=>image.complete&&image.naturalWidth>0));}
try {
  await navigate('首次加载',()=>page.goto(url));
  await page.getByRole('heading',{name:'临床工作台',exact:true}).waitFor();
  const info=await page.evaluate(()=>{
    const data=JSON.parse(document.getElementById('offline-data').textContent);
    const parser=new DOMParser();
    for(const snapshot of data.snapshots){
      const doc=parser.parseFromString(snapshot.html,'text/html');
      if(doc.querySelector('script,iframe,input[type=password]'))throw Error('快照含不允许的节点');
      for(const image of doc.querySelectorAll('img'))if(!data.assets[image.dataset.asset])throw Error('影像资源缺失');
      const player=doc.querySelector('[data-player]');
      if(snapshot.media&&player&&player.dataset.player!==snapshot.media)throw Error('实际影像与快照不一致');
      if(/正在读取分析记录|正在读取影像…|正在读取模型状态/.test(doc.body.textContent))throw Error('快照包含未完成的加载状态');
      if(snapshot.job&&doc.querySelector('[data-choose=job]')?.value!==snapshot.job)throw Error('实际分析任务与快照不一致');
      for(const link of doc.querySelectorAll('a[href]'))if(!link.getAttribute('href').startsWith('#'))throw Error('残留外部链接');
    }
    for(const [id,rows] of Object.entries(data.jobs))for(const [type,tab]of [['llm_quality','影像质量'],['llm_findings','异常征象']]){
      const expected=rows.filter(job=>job.model_type===type).map(job=>String(job.id)).sort();
      const actual=data.snapshots.filter(snapshot=>snapshot.route==='workspace'&&snapshot.media===id&&snapshot.tab===tab&&snapshot.job).map(snapshot=>snapshot.job).sort();
      if(JSON.stringify(expected)!==JSON.stringify(actual))throw Error('分析历史导出不完整');
    }
    return {cases:data.cases,media:Object.fromEntries(Object.entries(data.media).map(([id,item])=>[id,{frameCount:item.frameCount}])),jobs:data.jobs,snapshots:data.snapshots.length,images:Object.keys(data.assets).length};
  });
  for(const name of ['患儿管理','检查管理','影像管理','AI影像分析','多切面分析','AI 辅助诊断','专家协作','报告中心','系统管理','临床工作台']) {
    await page.locator('.sidebar nav').getByRole('link',{name,exact:true}).click();
    await images();checked.push('菜单:'+name);
  }
  await page.locator('.sidebar nav').getByRole('link',{name:'患儿管理',exact:true}).click();
  await page.getByRole('link',{name:'查看详情',exact:true}).first().click();
  for(const name of ['临床病史','检查记录','历史报告','基本资料']) {await page.getByRole('tab',{name,exact:true}).click();assert.equal(await page.getByRole('tab',{name,exact:true}).getAttribute('aria-selected'),'true');}
  checked.push('患儿详情四页');
  await page.locator('.sidebar nav').getByRole('link',{name:'检查管理',exact:true}).click();
  await page.getByRole('link',{name:'查看详情',exact:true}).first().click();
  for(const name of ['超声测量','影像资料','AI分析','审核记录','修改历史','临床参数']) {await page.getByRole('tab',{name,exact:true}).click();assert.equal(await page.getByRole('tab',{name,exact:true}).getAttribute('aria-selected'),'true');}
  checked.push('检查详情六页');
  await page.locator('.sidebar nav').getByRole('link',{name:'AI影像分析',exact:true}).click();
  for(const exam of info.cases) {
    await page.getByLabel('当前检查',{exact:true}).selectOption(String(exam.id));
    assert.equal(await page.getByLabel('当前检查',{exact:true}).inputValue(),String(exam.id));
    for(const mediaId of exam.media){
    await page.locator('.series-list button[data-media="'+mediaId+'"]').click();
    assert.equal(await page.locator('.cine[data-player]').first().getAttribute('data-player'),mediaId);
    for(const tab of ['切面识别','影像质量','心脏结构','异常征象']) {
      await page.getByRole('button',{name:tab,exact:true}).click();await images();
      if(tab==='影像质量'||tab==='异常征象') {
        const select=page.getByLabel('分析记录',{exact:true});
        if(await select.count()) {
          const options=await select.locator('option').evaluateAll(options=>options.map(option=>option.value));
          for(const option of options) {await select.selectOption(option);assert((await page.locator('body').textContent()).includes('任务 #'+option));}
          const saved=info.jobs[mediaId].find(job=>job.model_type===(tab==='影像质量'?'llm_quality':'llm_findings')&&job.state==='succeeded');
          if(saved){await select.selectOption(String(saved.id));assert((await page.locator('.medical-review').textContent()).includes(saved.output.summary));}
          const evidence=page.getByRole('button',{name:/^证据 · 第/});
          if(await evidence.count()){await evidence.first().click();await images();assert.equal(await page.locator('.offline-evidence img').count(),1);}
        }
      }
      if(tab==='心脏结构'){
        const select=page.getByLabel('历史分割结果',{exact:true});
        if(await select.count()){
          await select.locator('..').locator('..').locator('summary').click();
          const options=await select.locator('option').evaluateAll(options=>options.map(option=>option.value));
          for(const option of options){await select.selectOption(option);await images();assert((await select.locator('..').locator('..').textContent()).includes('任务 #'+option));}
        }
      }
    }
    await navigate('检查 #'+exam.id+' 影像 '+(exam.media.indexOf(mediaId)+1)+' 刷新',()=>page.reload());await images();
    assert.equal(await page.locator('.cine[data-player]').first().getAttribute('data-player'),mediaId);
    assert.equal(await page.getByLabel('当前检查',{exact:true}).inputValue(),String(exam.id));
    checked.push('检查 #'+exam.id+' 影像 '+(exam.media.indexOf(mediaId)+1)+' 模型结果/历史/刷新保持');
    }
  }
  const sample=info.cases.find(item=>item.media.some(id=>info.media[id].frameCount===53));
  assert(sample,'缺少53帧样例');
  await page.getByLabel('当前检查',{exact:true}).selectOption(String(sample.id));
  await page.getByRole('button',{name:'下一帧',exact:true}).click();
  assert.equal(await page.getByLabel('帧时间轴').inputValue(),'1');
  await page.getByRole('button',{name:'上一帧',exact:true}).click();
  assert.equal(await page.getByLabel('帧时间轴').inputValue(),'0');
  await page.getByLabel('帧时间轴').fill('52');
  assert((await page.locator('.cine-controls').textContent()).includes('53 / 53'));
  await page.getByRole('button',{name:'播放',exact:true}).click();
  await page.waitForFunction(()=>Number(document.querySelector('[aria-label="帧时间轴"]').value)>0);
  await page.getByRole('button',{name:'暂停',exact:true}).click();
  checked.push('53帧逐帧/时间轴/播放/暂停');
  await page.getByRole('button',{name:'心脏结构',exact:true}).click();
  await navigate('检查 #'+sample.id+' 四腔分割刷新',()=>page.reload());await images();
  assert.equal(await page.getByLabel('当前检查',{exact:true}).inputValue(),String(sample.id));
  checked.push('刷新后保留检查与面板');
  assert.deepEqual(errors,[]);assert.deepEqual(network,[]);
  console.log(JSON.stringify({ok:true,mode:remote?'public-url':'offline-file',checks:checked,snapshots:info.snapshots,images:info.images,externalRequests:network.length,pageErrors:errors.length,navigations}));
} catch(error) {
  console.error(JSON.stringify({completed:checked,pageErrors:errors,navigations,heading:await page.locator('h1').allTextContents(),selectLabels:await page.locator('select').evaluateAll(selects=>selects.map(select=>select.closest('label')?.childNodes[0]?.textContent))}));
  throw error;
} finally {await browser.close();}
