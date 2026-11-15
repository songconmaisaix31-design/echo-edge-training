(() => {
  const data = JSON.parse(document.getElementById('offline-data').textContent);
  const root = document.getElementById('root');
  let current;
  const timers = new Set();
  let selectedExam = String(data.cases[0]?.id || '');
  let selectedMedia = data.cases[0]?.media[0] || '';
  function show(criteria) {
    const candidates = data.snapshots.filter(snapshot => snapshot.route === criteria.route && (!criteria.exam || snapshot.exam === String(criteria.exam)) && (!criteria.media || snapshot.media === criteria.media));
    const snapshot = candidates.find(snapshot => (!criteria.tab || snapshot.tab === criteria.tab) && (!criteria.job || snapshot.job === criteria.job));
    if (!snapshot) throw new Error('离线快照没有此页面：' + criteria.route);
    timers.forEach(clearTimeout); timers.clear();
    current = snapshot;
    if (snapshot.exam) selectedExam = snapshot.exam;
    if (snapshot.media) selectedMedia = snapshot.media;
    root.innerHTML = snapshot.html;
    root.querySelectorAll('img[data-asset]').forEach(image => {
      const source = data.assets[image.dataset.asset];
      if (!source) throw new Error('快照影像资源缺失');
      image.src = source;
    });
    const banner = document.createElement('div');
    banner.className = 'offline-banner';
    const title = document.createElement('strong'); title.textContent = '离线数据快照 · 只读展示';
    const note = document.createElement('span'); note.textContent = new Date(data.created).toLocaleString('zh-CN') + ' 导出。可切换检查、播放已保存影像和查看分析结果；新增分析、上传及修改需使用在线工作站。';
    banner.append(title,note);
    root.querySelector('.shell-header').after(banner);
    root.querySelectorAll('.cine[data-player]').forEach(setupPlayer);
    if (current.media && (current.tab === '心脏结构' || current.tab === '诊断证据')) setupStructureHistory();
    root.querySelectorAll('form').forEach(form=>form.addEventListener('submit',event=>event.preventDefault()));
    document.title = 'Echo Edge · ' + (root.querySelector('h1')?.textContent || '数据展示');
    history.replaceState(null,'','#offline/'+encodeURIComponent(current.route)+'/'+(current.exam||'')+'/'+encodeURIComponent(current.tab||'')+'/'+(current.job||'')+'/'+(current.media||''));
    window.scrollTo(0,0);
  }
  function setupStructureHistory() {
    const rows=data.jobs[current.media].filter(job=>job.model_type==='cardiacudc_fourchamber');
    const article=[...root.querySelectorAll('article')].find(article=>article.querySelector('h2')?.textContent==='四腔结构分割');
    if (!article || !rows.length) return;
    const details=document.createElement('details');
    const summary=document.createElement('summary');summary.textContent='已保存的分割记录 · '+rows.length;
    const label=document.createElement('label');label.textContent='历史分割结果';
    const select=document.createElement('select');select.setAttribute('aria-label','历史分割结果');
    const content=document.createElement('div');content.className='record';
    rows.forEach(job=>{const option=document.createElement('option');option.value=String(job.id);option.textContent='#'+job.id+' · '+new Date(job.created).toLocaleString('zh-CN')+' · '+({succeeded:'成功',failed:'失败',interrupted:'已中断',running:'运行中',pending:'待处理'}[job.state]||job.state);select.append(option);});
    function display() {
      const job=rows.find(job=>String(job.id)===select.value);content.replaceChildren();
      const text=document.createElement('p');
      text.textContent='任务 #'+job.id+(job.output?' · 第 '+(job.output.frame.frame_index+1)+' 帧 · '+job.output.frame.timestamp_ms+' ms · 模型第 '+job.output.epoch+' 轮':' · '+(job.error||job.state));
      content.append(text);
      if(job.state==='succeeded') {const image=document.createElement('img');image.src=data.assets['/api/v1/analysis-jobs/'+job.id+'/overlay'];image.alt='历史四腔分割叠图 · 任务 '+job.id;image.style.maxWidth='100%';content.append(image);}
    }
    select.addEventListener('change',display);label.append(select);details.append(summary,label,content);article.append(details);display();
  }
  function setupPlayer(cine) {
    const item = data.media[cine.dataset.player];
    const image = cine.querySelector('.cine-image img');
    const range = cine.querySelector('input[type=range]');
    const status = cine.querySelector('.cine-controls span');
    const buttons = [...cine.querySelectorAll('button')];
    const previous=buttons.find(button=>button.dataset.playback==='上一帧');
    const next=buttons.find(button=>button.dataset.playback==='下一帧');
    const play=buttons.find(button=>['播放','暂停'].includes(button.dataset.playback));
    let index=0, playing=false, timer;
    function draw() {
      image.src=data.assets['/api/v1/media/'+item.id+'/frames/'+index];image.alt='源帧 '+(index+1);
      range.value=String(index);status.textContent=(index+1)+' / '+item.frameCount+' · '+(item.frames[index].offsetMs/1000).toFixed(3)+' s';
      previous.disabled=index===0;next.disabled=index===item.frameCount-1;play.textContent=playing?'暂停':'播放';
    }
    function stop() {playing=false;clearTimeout(timer);timers.delete(timer);draw();}
    function tick() {
      if (!playing) return;
      if (index>=item.frameCount-1) {stop();return;}
      timer=setTimeout(()=>{timers.delete(timer);index++;draw();tick();},Math.max(1,item.frames[index+1].offsetMs-item.frames[index].offsetMs));
      timers.add(timer);
    }
    previous.onclick=()=>{stop();index=Math.max(0,index-1);draw();};
    next.onclick=()=>{stop();index=Math.min(item.frameCount-1,index+1);draw();};
    play.onclick=()=>{if(playing){stop();return;}if(index===item.frameCount-1)index=0;playing=true;draw();tick();};
    range.oninput=()=>{const chosen=Number(range.value);stop();index=chosen;draw();};
    draw();
  }
  function route(href, exam=selectedExam) {
    const path=href.replace(/^#/,'');
    if (path.startsWith('diagnosis')) {
      const parts=path.split('/'); const id=parts[1]||exam;
      const chosen=data.cases.find(item=>String(item.id)===id);
      show({route:'diagnosis/'+id,exam:id,media:parts[3]||chosen?.media[0]||'',tab:parts[2]==='structure'?'诊断证据':undefined});
    } else if (['workspace','media','multi','experts','reports'].includes(path)) {
      const chosen=data.cases.find(item=>String(item.id)===exam);
      show({route:path,exam,media:chosen?.media.includes(selectedMedia)?selectedMedia:chosen?.media[0]||''});
    } else show({route:path});
  }
  root.addEventListener('click',event=>{
    const target=event.target.closest('a,button');if(!target||target.disabled)return;
    if(target.dataset.playback)return;
    if(target.dataset.media){show({...current,media:target.dataset.media,job:undefined});return;}
    if(target.dataset.tab){show({...current,tab:target.dataset.tab,job:undefined});return;}
    if(target.dataset.route){route(target.dataset.route,target.dataset.exam||selectedExam);return;}
    if(target.dataset.evidence!==undefined){
      const path='/api/v1/media/'+current.media+'/frames/'+target.dataset.evidence;
      if(!data.assets[path])throw new Error('证据帧不存在');
      const article=target.closest('.medical-review')||target.parentElement;
      let figure=article.querySelector('.offline-evidence');
      if(!figure){figure=document.createElement('figure');figure.className='offline-evidence';article.append(figure);}
      figure.replaceChildren();const image=document.createElement('img');image.src=data.assets[path];image.alt=target.textContent;
      const caption=document.createElement('figcaption');caption.textContent=target.textContent;figure.append(image,caption);figure.scrollIntoView({block:'nearest'});return;
    }
    if(target.tagName==='A'){
      event.preventDefault();const href=target.getAttribute('href');if(href)route(href);
    }
  });
  root.addEventListener('change',event=>{
    const select=event.target;
    if(select.dataset.choose==='exam'){
      if(!select.value){select.value=selectedExam;return;}
      const item=data.cases.find(item=>String(item.id)===select.value);
      show({...current,route:current.route.startsWith('diagnosis')?'diagnosis/'+select.value:current.route,exam:select.value,media:item.media[0]||'',job:undefined});
    }
    if(select.dataset.choose==='media')show({...current,media:select.value,job:undefined});
    if(select.dataset.choose==='job')show({...current,job:select.value});
  });
  const locationParts=location.hash.startsWith('#offline/')?location.hash.slice(9).split('/'):[];
  if(locationParts.length){show({route:decodeURIComponent(locationParts[0]),exam:locationParts[1],tab:decodeURIComponent(locationParts[2]||''),job:locationParts[3],media:locationParts[4]});}
  else show({route:'home'});
  const icon=document.createElement('link');icon.rel='icon';
  icon.href='data:image/svg+xml,'+encodeURIComponent(root.querySelector('.wordmark svg').outerHTML);
  document.head.append(icon);
})();
