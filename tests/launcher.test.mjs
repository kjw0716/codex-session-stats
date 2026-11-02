import assert from 'node:assert/strict';
import { copyFileSync, existsSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = process.env.SESSION_STATS_PACKAGE_ROOT || resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fixture = mkdtempSync(join(tmpdir(), 'session-stats-launcher-'));
const launcherDir = join(fixture, "启动器 with spaces & apostrophe's");
const local = join(fixture, 'local');
const installed = join(fixture, 'installed');
const desktop = join(fixture, 'desktop');
mkdirSync(launcherDir);
mkdirSync(join(local, 'CodexSessionMonitor'), { recursive: true });
mkdirSync(desktop);
const env = { ...process.env, LOCALAPPDATA: local, LAUNCHER_FIXTURE: fixture, LAUNCHER_SOURCE: root };
const ps = text => {
  const result = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', text], { env, windowsHide: true, timeout: 20000 });
  assert.equal(result.status, 0, result.error?.message || result.stderr?.toString());
  return result.stdout?.toString();
};
const launcher = join(launcherDir, 'Open-ChatGPT.exe');
const script = join(launcherDir, 'Open-Codex.ps1');
const output = join(launcherDir, 'result.json');
const prefs = join(local, 'CodexSessionMonitor', 'preferences.json');
const run = expected => {
  // Do not ask the test runner to hide the launcher. The executable itself
  // must prevent allocation of a console for its PowerShell child.
  const result = spawnSync(launcher, [], { env, timeout: 20000, windowsHide: false });
  assert.equal(result.status, expected, result.error?.message || result.stderr?.toString());
};
try {
  copyFileSync(join(root, 'Open-ChatGPT.exe'), launcher);
  const pe = readFileSync(launcher), header = pe.readUInt32LE(60);
  assert.equal(pe.readUInt16LE(header + 4), 0x8664, 'launcher must be x64');
  assert.equal(pe.readUInt16LE(header + 24 + 68), 2, 'launcher must use the Windows GUI subsystem');
  writeFileSync(script, `Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class ConsoleProbe { [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow(); }'
@{console=[ConsoleProbe]::GetConsoleWindow().ToInt64(); argsCount=$args.Count} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'result.json')
exit 37
`);
  run(37);
  const result = JSON.parse(readFileSync(output, 'utf8').replace(/^\uFEFF/, ''));
  assert.equal(result.console, 0, 'PowerShell child must not own or inherit a console');
  assert.equal(result.argsCount, 0);
  console.log('PASS GUI launcher starts PowerShell without a console, handles Unicode/spaces/punctuation, and returns child exit status');

  copyFileSync(join(root, 'Open-Codex.ps1'), script);
  writeFileSync(join(launcherDir, 'Start-EmbeddedStats.ps1'), `param([switch]$AppOnly)
@{appOnly=[bool]$AppOnly} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'result.json')
`);
  for (const [value, appOnly] of [[null, true], ['{"autoStart":false}', true], ['{"autoStart":true}', false], ['invalid json', true]]) {
    if (value === null) { if (existsSync(prefs)) rmSync(prefs); }
    else writeFileSync(prefs, value);
    run(0);
    assert.equal(JSON.parse(readFileSync(output, 'utf8').replace(/^\uFEFF/, '')).appOnly, appOnly);
  }
  console.log('PASS real startup script preserves automatic stats on/off and missing/invalid preference behavior');

  // Mock only Store-package discovery, so CI never needs ChatGPT installed.
  writeFileSync(prefs, JSON.stringify({ autoStart: true, floatWidth: 634 }));
  const configure = `function Get-AppxPackage { [pscustomobject]@{Version='1.0'; PackageFamilyName='OpenAI.Codex_fixture'; InstallLocation=$env:LAUNCHER_FIXTURE} }
& (Join-Path $env:LAUNCHER_SOURCE 'Install-Shortcuts.ps1') -ConfigureCodexEntry -InstallDirectory (Join-Path $env:LAUNCHER_FIXTURE 'installed') -DesktopPath (Join-Path $env:LAUNCHER_FIXTURE 'desktop')`;
  ps(configure);
  assert.deepEqual(JSON.parse(readFileSync(prefs, 'utf8').replace(/^\uFEFF/, '')), { autoStart: true, floatWidth: 634 });
  const verify = `$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $env:LAUNCHER_FIXTURE 'desktop/ChatGPT.lnk')); if($s.TargetPath -ne (Join-Path $env:LAUNCHER_FIXTURE 'installed/Open-ChatGPT.exe') -or $s.Arguments -ne ''){throw 'Incorrect ChatGPT shortcut'}`;
  ps(verify);
  assert.ok(existsSync(join(installed, 'Open-ChatGPT.exe')));
  ps(configure);
  ps(verify);
  assert.equal(existsSync(join(desktop, 'ChatGPT (Session Ready).lnk')), false);
  console.log('PASS clean install and repeat install keep the ChatGPT shortcut on the packaged native launcher');
  ps(configure + ' -AutoStart 0');
  assert.equal(JSON.parse(readFileSync(prefs, 'utf8').replace(/^\uFEFF/, '')).autoStart, false);
  assert.equal(JSON.parse(readFileSync(prefs, 'utf8').replace(/^\uFEFF/, '')).floatWidth, 634);
  console.log('PASS reinstall preserves preferences and explicit automatic-start changes still apply');

  ps(`$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $env:LAUNCHER_FIXTURE 'desktop/ChatGPT.lnk')); $s.TargetPath=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'; $s.Arguments='-NoProfile -File "'+(Join-Path $env:LAUNCHER_FIXTURE 'installed/Open-Codex.ps1')+'"'; $s.Save()`);
  const legacy = readFileSync(join(desktop, 'ChatGPT.lnk'));
  ps(configure);
  ps(verify);
  ps(`$found=@(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'CodexSessionMonitor/shortcut-backups') -Filter ChatGPT.lnk -Recurse); if($found.Count -lt 1){throw 'No shortcut backups'}`);
  // Locate an exact backup through PowerShell so no user directory is scanned.
  const legacyHex = legacy.toString('hex');
  const backupHexes = ps(`@(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'CodexSessionMonitor/shortcut-backups') -Filter ChatGPT.lnk -Recurse) | ForEach-Object { ([BitConverter]::ToString([IO.File]::ReadAllBytes($_.FullName))).Replace('-','').ToLowerInvariant() }`);
  assert.ok(backupHexes.includes(legacyHex), 'legacy shortcut must be backed up exactly');
  console.log('PASS v0.2 PowerShell shortcut upgrades to the native launcher with an exact backup');

  ps(`$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $env:LAUNCHER_FIXTURE 'desktop/ChatGPT.lnk')); $s.TargetPath=Join-Path $env:WINDIR 'System32/notepad.exe'; $s.Arguments=''; $s.Save()`);
  const unknown = readFileSync(join(desktop, 'ChatGPT.lnk'));
  ps(configure);
  assert.deepEqual(readFileSync(join(desktop, 'ChatGPT.lnk')), unknown);
  assert.ok(existsSync(join(desktop, 'ChatGPT (Session Ready).lnk')));
  console.log('PASS unrelated shortcuts are preserved and a separate Session Ready shortcut is created');
} finally {
  assert.ok(fixture.startsWith(join(tmpdir(), 'session-stats-launcher-')));
  rmSync(fixture, { recursive: true, force: true });
}
