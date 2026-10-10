(() => {
  const KEY='__CODEX_SESSION_ONLY_MONITOR__';
  if(window[KEY])return window[KEY].health();
  const host=document.createElement('div');host.id='codex-session-only-monitor';
  host.style.cssText='position:fixed;z-index:1000;display:none;height:27px;pointer-events:auto;';
  document.body.append(host);
  const shadow=host.attachShadow({mode:'open'});
  shadow.innerHTML=`<style>
    :host{font-family:inherit;font-size:12px;color:var(--sm-fg,#303238);color-scheme:light dark}*{box-sizing:border-box}button,input{font:inherit}button{cursor:pointer}
    .bar{display:flex;align-items:center;gap:9px;width:100%;height:25px;padding:1px 9px;border:0;border-radius:7px;background:var(--sm-soft,#f3f4f6);color:inherit;text-align:left;white-space:nowrap;overflow:hidden}
    .bar:hover{background:var(--sm-hover,#e8eaf0)}.dot{height:7px;width:7px;background:#858b95;border-radius:50%;flex:none}.dot[data-level="normal"]{background:#25ba72}.dot[data-level="warning"]{background:#dfaa35}.dot[data-level="high"]{background:#eb6767}
    .summary{overflow:hidden;text-overflow:ellipsis}.activity{flex:none;font-size:10px;color:var(--sm-muted,#80858e)}.activity[hidden]{display:none}.chevron{margin-left:auto;font-size:10px;color:var(--sm-muted,#80858e)}
    .panel{position:fixed;display:none;width:338px;padding:14px;border:1px solid var(--sm-border,#dddfe5);border-radius:12px;background:var(--sm-bg,#fff);box-shadow:0 8px 28px #0002;color:inherit}.panel.open{display:block}
    .heading{display:flex;gap:8px;align-items:center;font-size:13px;font-weight:650;margin-bottom:10px}.close{margin-left:auto;border:0;background:transparent;color:var(--sm-muted,#80858e);font-size:18px;line-height:1}
    .title{color:var(--sm-muted,#80858e);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-bottom:10px;line-height:1.6}
    .row{display:flex;align-items:center;gap:8px;min-height:33px}.row input{accent-color:#737b87;width:14px;height:14px;margin:0;cursor:pointer}.label{flex:1;font-size:12px;color:var(--sm-muted,#80858e)}.value{font-size:12px;font-weight:600;white-space:nowrap;font-variant-numeric:tabular-nums}
    .foot{border-top:1px solid var(--sm-border,#dddfe5);margin-top:10px;padding-top:9px;color:var(--sm-muted,#80858e);font-size:10px;line-height:1.7}.context-warning{color:#d28520}.context-high{color:#eb6767}
  </style><button class="bar" title="Show session statistics" aria-expanded="false"><span class="dot"></span><span class="summary">Session · Loading</span><span class="activity" hidden>Running</span><span class="chevron">⌃</span></button><section class="panel" aria-label="Session statistics"><div class="heading"><span class="dot"></span>This Session<button class="close" title="Collapse">×</button></div><div class="title"></div><div class="rows"></div><div class="foot">Context: green &lt;70%, yellow 70–&lt;85%, red ≥85%.<br>Capacity only; total tokens are not current context length.<br>Selected metrics appear below. Speed includes thinking/tool waits.</div></section>`;
  const bar=shadow.querySelector('.bar'),panel=shadow.querySelector('.panel'),summary=shadow.querySelector('.summary');
  const definitions=[['totalTokens','Total session tokens','Total'],['lastTurnTokens','Last turn tokens','Last'],['cacheHitPercent','Cache hit rate','Cache'],['compactionCount','Context compactions','Compactions'],['status','Status',''],['contextPercent','Context usage','Context'],['lastTtftMs','Time to first token','TTFT'],['averageOutputTps','Average output speed','Avg output']];
  let saved;try{saved=JSON.parse(localStorage.getItem('codex-session-only-metrics-v1'));}catch{}
  const selected=new Set((Array.isArray(saved)?saved:['totalTokens','contextPercent']).filter(k=>definitions.some(d=>d[0]===k)));
  const rows=new Map();
  for(const [key,label] of definitions){
    const row=document.createElement('label');row.className='row';
    const check=document.createElement('input');check.type='checkbox';check.checked=selected.has(key);check.setAttribute('aria-label','Show '+label+' in the bar');
    const name=document.createElement('span');name.className='label';name.textContent=label;
    const value=document.createElement('span');value.className='value';value.textContent='--';row.append(check,name,value);shadow.querySelector('.rows').append(row);rows.set(key,value);
    check.addEventListener('change',()=>{check.checked?selected.add(key):selected.delete(key);try{localStorage.setItem('codex-session-only-metrics-v1',JSON.stringify([...selected]));}catch{}render();});
  }
  let current=null,active=null,reserved=null,opened=false,destroyed=false,timer=null,reason='starting',requestAt=0,nativeContext=null,observedMeter=null,renderedFingerprint=null;
  const uuid=/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i;
  const visible=n=>{const r=n.getBoundingClientRect();return r.width>0&&r.height>0&&!n.closest('[data-app-shell-active-page="false"],[aria-hidden="true"],[inert]');};
  function release(){if(reserved){const {node,value,priority}=reserved;if(value)node.style.setProperty('padding-bottom',value,priority);else node.style.removeProperty('padding-bottom');reserved=null;}geometryObserver.disconnect();contextObserver.disconnect();observedMeter=null;}
  function select(){
    const composers=[...document.querySelectorAll('.composer-surface-chrome,[data-testid="composer"],[class*="ComposerLayoutRoot"]')].filter(visible);
    const composer=composers.find(c=>[...c.querySelectorAll('textarea,[contenteditable="true"]')].some(visible));if(!composer)return {reason:'no-composer'};
    const scope=composer.closest('[data-app-shell-active-page="true"]')||document;
    for(const attr of ['data-above-composer-conversation-id','data-conversation-id','data-thread-id']){
      const nodes=[...scope.querySelectorAll('['+attr+']')].filter(n=>!n.closest('[data-app-shell-active-page="false"],[aria-hidden="true"],[inert]'));
      const options=nodes.map(n=>String(n.getAttribute(attr)||'')).filter(id=>!/^chatgpt\s*:/i.test(id)&&uuid.test(id));
      if(options.length)return {composer,threadId:options[0].match(uuid)[0].toLowerCase(),reason:'selected'};
      if(attr==='data-above-composer-conversation-id'&&nodes.some(n=>/^chatgpt\s*:/i.test(n.getAttribute(attr))))return {reason:'ordinary-chat'};
    }
    return {reason:'no-codex-session'};
  }
  function readNativeContext(composer){
    const meter=[...composer.querySelectorAll('[role="img"][aria-label],[role="progressbar"][aria-label]')].find(n=>visible(n)&&/context usage|上下文.*(?:占用|使用)|(?:占用|使用).*上下文/i.test(n.getAttribute('aria-label')||''));
    if(meter!==observedMeter){contextObserver.disconnect();observedMeter=meter||null;if(meter)contextObserver.observe(meter,{attributes:true,subtree:true,attributeFilter:['aria-label','stroke-dashoffset']});}
    if(!meter)return null;
    const labelMatch=(meter.getAttribute('aria-label')||'').match(/(\d+(?:[.,]\d+)?)\s*%/);
    const labelPercent=labelMatch?Number(labelMatch[1].replace(',','.')):null;
    const ring=meter.querySelector('circle[stroke-dasharray][stroke-dashoffset]');
    const circumference=ring?parseFloat(ring.getAttribute('stroke-dasharray')):NaN,offset=ring?parseFloat(ring.getAttribute('stroke-dashoffset')):NaN;
    const ringPercent=Number.isFinite(circumference)&&circumference>0&&Number.isFinite(offset)?(1-offset/circumference)*100:null;
    const fiberKey=Object.keys(meter).find(k=>k.startsWith('__reactFiber$'));
    let fiber=fiberKey?meter[fiberKey]:null;
    for(let depth=0;fiber&&depth<14;depth++,fiber=fiber.return){
      for(const props of [fiber.memoizedProps,fiber.alternate?.memoizedProps]){
        const usage=props?.contextUsage;
        const matchesRing=Number.isFinite(ringPercent)?Math.abs(usage?.percent-ringPercent)<0.0001:!Number.isFinite(labelPercent)||Math.round(usage?.percent)===labelPercent;
        if(usage&&Number.isFinite(usage.percent)&&usage.percent>=0&&matchesRing){return {percent:usage.percent,usedTokens:Number.isFinite(usage.usedTokens)?usage.usedTokens:null,contextWindow:Number.isFinite(usage.contextWindow)?usage.contextWindow:null,source:'codex-native'};}
      }
    }
    const percent=Number.isFinite(ringPercent)?ringPercent:labelPercent;
    return Number.isFinite(percent)?{percent,usedTokens:null,contextWindow:null,source:'codex-native-label'}:null;
  }
  const tokenFormatter=new Intl.NumberFormat('en-US',{notation:'compact',maximumFractionDigits:2});
  function tokens(n){return Number.isFinite(n)?tokenFormatter.format(Math.round(n)):'--';}
  function metric(key){
    if(key==='contextPercent')return nativeContext?[Number.isFinite(nativeContext.usedTokens)?tokens(nativeContext.usedTokens)+' tokens':null,nativeContext.percent.toFixed(1)+'%'].filter(Boolean).join(' · '):'--';
    if(!current)return '--';const n=current[key];
    if(key==='status')return ({running:'Running',completed:'Completed',paused:'Paused',idle:'Not started',unknown:'No data',unavailable:'No data'})[n]||n||'--';
    if(n==null)return '--';if(key==='cacheHitPercent')return Number(n).toFixed(1)+'%';if(key==='lastTtftMs')return (Number(n)/1000).toFixed(1)+' s';if(key==='averageOutputTps')return Number(n).toFixed(1)+' tok/s';if(key==='compactionCount')return String(n);return tokens(n);
  }
  function render(){
    const fingerprint=JSON.stringify([active,current,nativeContext,[...selected]]);if(fingerprint===renderedFingerprint)return;renderedFingerprint=fingerprint;
    const percent=nativeContext?.percent,level=Number.isFinite(percent)?percent>=85?'high':percent>=70?'warning':'normal':'unknown';host.dataset.contextLevel=level;
    for(const [key] of definitions){const value=rows.get(key);value.textContent=metric(key);value.classList.toggle('context-warning',key==='contextPercent'&&level==='warning');value.classList.toggle('context-high',key==='contextPercent'&&level==='high');}
    const values=definitions.filter(d=>selected.has(d[0])).map(d=>[d[2],metric(d[0])].filter(Boolean).join(' '));summary.textContent=current?.error?'Session · Data unavailable':values.join(' · ')||'Session statistics';
    shadow.querySelector('.activity').hidden=current?.status!=='running'||selected.has('status')||!!current?.error;
    const title=shadow.querySelector('.title');title.textContent=current?[current.title||'Local chat',current.model].filter(Boolean).join(' · '):'Reading current session';title.title=active||'';
    const contextHint=nativeContext?`Native Codex context usage: ${nativeContext.percent.toLocaleString('en-US',{maximumFractionDigits:4})}%. Green below 70%, yellow from 70% to below 85%, red from 85%. Capacity indicator, not an ability score.`:'Native Codex context data is unavailable. Total token usage is not used as a substitute.';
    for(const dot of shadow.querySelectorAll('.dot')){dot.dataset.level=level;dot.title=contextHint;}
    rows.get('totalTokens').title=Number.isFinite(current?.totalTokens)?current.totalTokens.toLocaleString('en-US')+' tokens. Cumulative usage across requests, including repeated input context.':'';
    rows.get('contextPercent').title=nativeContext?`Native Codex tokens used: ${Number.isFinite(nativeContext.usedTokens)?nativeContext.usedTokens.toLocaleString('en-US'):'--'} / Model context window: ${Number.isFinite(nativeContext.contextWindow)?nativeContext.contextWindow.toLocaleString('en-US'):'--'} tokens. ${contextHint}`:contextHint;
    rows.get('averageOutputTps').title='Total output tokens in completed turns / total turn duration. Includes thinking and tool waits; not pure generation TPS.';
    rows.get('lastTtftMs').title='Recorded time from turn start to the first model token, which may be a reasoning token.';
  }
  function positionPanel(){if(!opened)return;const r=host.getBoundingClientRect(),w=Math.min(338,innerWidth-24);panel.style.width=w+'px';panel.style.left=Math.max(12,Math.min(r.left,innerWidth-w-12))+'px';panel.style.top=Math.max(12,r.top-panel.offsetHeight-8)+'px';}
  function setOpen(value){opened=value;panel.classList.toggle('open',value);bar.setAttribute('aria-expanded',String(value));positionPanel();}
  bar.addEventListener('click',()=>setOpen(!opened));shadow.querySelector('.close').addEventListener('click',()=>setOpen(false));
  const outside=e=>{if(!e.composedPath().includes(host))setOpen(false);},escape=e=>{if(e.key==='Escape')setOpen(false);};document.addEventListener('pointerdown',outside);document.addEventListener('keydown',escape);
  function schedule(ms){clearTimeout(timer);timer=setTimeout(tick,ms);}
  const contextObserver=new MutationObserver(()=>{if(reserved&&!document.hidden){nativeContext=readNativeContext(reserved.node);render();}});
  const geometryObserver=new ResizeObserver(()=>schedule(100));
  function tick(){
    if(destroyed)return;if(document.hidden){host.style.display='none';setOpen(false);schedule(15000);return;}
    const choice=select();reason=choice.reason;
    if(!choice.threadId){host.style.display='none';release();current=null;active=null;nativeContext=null;setOpen(false);schedule(5000);return;}
    const composer=choice.composer;
    if(!reserved||reserved.node!==composer){release();reserved={node:composer,value:composer.style.getPropertyValue('padding-bottom'),priority:composer.style.getPropertyPriority('padding-bottom')};composer.style.setProperty('padding-bottom',((parseFloat(getComputedStyle(composer).paddingBottom)||0)+29)+'px');geometryObserver.observe(composer);}
    const r=composer.getBoundingClientRect();host.style.display='block';host.style.left=(r.left+8)+'px';host.style.top=(r.bottom-27)+'px';host.style.width=Math.max(80,r.width-16)+'px';
    const dark=document.documentElement.classList.contains('dark')||document.documentElement.dataset.theme==='dark'||getComputedStyle(document.documentElement).colorScheme==='dark';
    for(const [k,v] of Object.entries({'--sm-bg':dark?'#242529':'#fff','--sm-fg':dark?'#e8e9ec':'#303238','--sm-muted':dark?'#a4a7af':'#80858e','--sm-soft':dark?'#303137':'#f3f4f6','--sm-hover':dark?'#3b3d43':'#e8eaf0','--sm-border':dark?'#41434b':'#dddfe5'}))host.style.setProperty(k,v);
    if(active!==choice.threadId){active=choice.threadId;current=null;requestAt=0;}
    nativeContext=readNativeContext(composer);render();positionPanel();
    const delay=current?.status==='running'?2500:10000;
    if(Date.now()-requestAt>=delay&&typeof window.__codexSessionMonitorRequest==='function'){requestAt=Date.now();window.__codexSessionMonitorRequest(JSON.stringify({threadId:active,needsSnapshot:!current}));}
    schedule(delay);
  }
  const identityObserver=new MutationObserver(()=>schedule(150));identityObserver.observe(document.body,{attributes:true,subtree:true,attributeFilter:['data-app-shell-active-page','data-above-composer-conversation-id','data-conversation-id','data-thread-id']});
  const wake=()=>schedule(100);document.addEventListener('visibilitychange',wake);window.addEventListener('resize',wake);
  window[KEY]={update(value){if(value?.threadId!==active)return false;const oldStatus=current?.status;current=value;render();if(oldStatus!==current.status)schedule(document.hidden?15000:current.status==='running'?2500:10000);return true;},health(){return {installed:host.isConnected,visible:host.style.display!=='none',threadId:active,metrics:8,reason,contextLevel:host.dataset.contextLevel||'unknown',contextSource:nativeContext?.source||'unavailable',contextPercent:nativeContext?.percent??null};},remove(){destroyed=true;clearTimeout(timer);identityObserver.disconnect();release();document.removeEventListener('pointerdown',outside);document.removeEventListener('keydown',escape);document.removeEventListener('visibilitychange',wake);window.removeEventListener('resize',wake);host.remove();delete window[KEY];return true;}};
  tick();return window[KEY].health();
})()
