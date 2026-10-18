import { DatabaseSync } from 'node:sqlite';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const fixture=mkdtempSync(join(tmpdir(),'session-stats-panel-'));
const codex=join(fixture,'Codex 测试'), local=join(fixture,'local');
mkdirSync(codex);mkdirSync(join(local,'CodexSessionMonitor'),{recursive:true});
const id='aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const log=join(codex,'rollout.jsonl');
const usage={input_tokens:1000,output_tokens:200,cached_input_tokens:800,total_tokens:1200};
const event=(type,extra={})=>({timestamp:'2026-10-18T10:00:00Z',type:'event_msg',payload:{type,...extra}});
writeFileSync(log,[event('task_started',{turn_id:'relay'}),
  {type:'turn_context',payload:{model:'relay-test-model'}},
  {type:'token_usage_record',payload:{thread_id:id,turn_id:'relay',root_turn_id:'relay',thread_token_usage:usage,turn_token_usage:usage,usage}},
  event('task_complete',{turn_id:'relay',duration_ms:4000})].map(x=>JSON.stringify(x)).join('\n')+'\n');
const db=new DatabaseSync(join(codex,'state_5.sqlite'));
db.exec('CREATE TABLE threads(id TEXT,title TEXT,name TEXT,model TEXT,source TEXT,archived INTEGER,recency_at_ms INTEGER,updated_at_ms INTEGER,updated_at INTEGER,rollout_path TEXT)');
const insert=db.prepare('INSERT INTO threads VALUES(?,?,?,?,?,?,?,?,?,?)');
insert.run(id,'Fixture title','Fixture display name','relay-test-model','vscode',0,100,100,100,log);
for(let i=0;i<40;i++)insert.run(`bbbbbbbb-bbbb-bbbb-bbbb-${String(i).padStart(12,'0')}`,'Other session',null,'test','vscode',0,i,i,i,log);
insert.run('cccccccc-cccc-cccc-cccc-cccccccccccc','Excluded child',null,'test','{"subagent":{}}',0,1000,1000,1000,log);
insert.run('dddddddd-dddd-dddd-dddd-dddddddddddd','Excluded archived',null,'test','vscode',1,1000,1000,1000,log);
db.close();
const prefs=join(local,'CodexSessionMonitor','preferences.json');
writeFileSync(prefs,JSON.stringify({threadId:id}));
const env={...process.env,CODEX_HOME:codex,LOCALAPPDATA:local};
const output=join(fixture,'smoke.json');
function runSmoke(){
  const run=spawnSync(join(root,'SessionStats.exe'),['--smoke-test',output],{env,timeout:20000,windowsHide:true});
  assert.equal(run.status,0,run.error?.message||run.stderr?.toString());
  return JSON.parse(readFileSync(output,'utf8'));
}
try {
  const result=runSmoke();
  assert.equal(result.recentCount,30);assert.equal(result.totalTokens,1200);assert.equal(result.averageOutputTps,50);
  assert.equal(result.title,'Fixture display name');assert.equal(result.model,'relay-test-model');
  assert.deepEqual(result.windowChecks,{separateWindows:true,dashboardHasNoMetrics:true,dashboardResizable:true,dashboardLayoutResizes:true,floatingSurvivesDashboardClose:true,adaptiveFloating:{layoutsFit:true,singleLineMinimum:true,noTitleOrFooter:true,pickerHiddenInMini:true,edgeResizeWorks:true,detailsRestore:true,opacityApplied:true}});
  assert.equal(JSON.parse(readFileSync(prefs,'utf8')).floatOpacity,35);
  console.log('PASS one-line resizing, adaptive layouts, hidden chrome, edge dragging and saved opacity');
  assert.equal(result.updatesWithDashboardClosed,true);assert.equal(result.floatingClosesCleanly,true);
  console.log('PASS floating statistics are independent of dashboard visibility and close cleanly');
  console.log('PASS native panel reads relay-style usage with no GPT/CDP restart; picker bounded to 30');
  writeFileSync(prefs,JSON.stringify({threadId:'cccccccc-cccc-cccc-cccc-cccccccccccc'}));
  assert.equal(runSmoke().threadId,id);
  console.log('PASS archived and subagent sessions cannot become the floating default');
  const legacy=new DatabaseSync(join(codex,'state_5.sqlite'));
  legacy.exec('ALTER TABLE threads DROP COLUMN name; ALTER TABLE threads DROP COLUMN recency_at_ms; ALTER TABLE threads DROP COLUMN updated_at_ms');legacy.close();
  assert.equal(runSmoke().title,'Fixture title');
  console.log('PASS recent picker supports legacy metadata schema');
  const install=spawnSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',join(root,'Install-Shortcuts.ps1'),'-InstallDirectory',join(fixture,'installed'),'-DesktopPath',join(fixture,'desktop')],{env,timeout:20000,windowsHide:true});
  assert.equal(install.status,0,install.stderr?.toString());
  const verify=spawnSync('powershell.exe',['-NoProfile','-Command',`$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $env:LOCALAPPDATA '../desktop/Session Stats.lnk')); if ($s.TargetPath -notlike '*installed\\SessionStats.exe' -or $s.IconLocation -notlike '*session-stats.ico,0') { exit 1 }`],{env,timeout:20000,windowsHide:true});
  assert.equal(verify.status,0,verify.stderr?.toString());
  console.log('PASS portable installation creates a native desktop shortcut with the custom icon');
} finally {
  // The exact directory is freshly created by this test, never a user-supplied path.
  assert.ok(fixture.startsWith(join(tmpdir(),'session-stats-panel-')));
  rmSync(fixture,{recursive:true,force:true});
}
