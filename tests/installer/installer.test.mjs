// Installs, upgrades and uninstalls the Windows installers of BeDD for the current user, and checks
// each step (Node 24, no dependencies):
//   node --test tests/installer/installer.test.mjs
// The installers come from env, built by `dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj`:
//   BEDD_MSI_OLD  an MSI of an older version (-p:ProductVersion=0.0.1)
//   BEDD_MSI      an MSI of a newer version (-p:ProductVersion=0.0.2)
//   BEDD_SETUP    the bedd-setup.exe of that newer version
// A scenario whose installer is not given is skipped. The tests refuse to run where BeDD is
// installed already: they would uninstall it. %LOCALAPPDATA%\BeDD (instances, worlds) is never
// touched, by the installers or by the tests.
import { describe, it, before, after, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const LOCAL = process.env.LOCALAPPDATA;
const INSTALL_DIR = path.join(LOCAL, 'Programs', 'BeDD');
const EXE = path.join(INSTALL_DIR, 'BeDD.exe');
const SHORTCUT = path.join(process.env.APPDATA, 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'BeDD.lnk');
const UNINSTALL_KEY = 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall';
const OUR_KEY = 'HKCU\\Software\\LiteLDev\\BeDD';
const LOG_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'bedd-installer-'));

const given = name => (process.env[name] ? path.resolve(process.env[name]) : undefined);
const msiOld = given('BEDD_MSI_OLD');
const msiNew = given('BEDD_MSI');
const setup = given('BEDD_SETUP');

function reg(...args) {
  const r = spawnSync('reg.exe', args, { encoding: 'utf8' });
  return r.status === 0 ? r.stdout : null;
}

// The user's PATH as stored (HKCU\Environment), split into its entries.
function userPath() {
  const out = reg('query', 'HKCU\\Environment', '/v', 'Path') ?? '';
  const m = out.match(/Path\s+REG_(?:EXPAND_)?SZ\s+(.*)/);
  return m ? m[1].trim().split(';').filter(Boolean) : [];
}

const onPath = () => userPath().some(p => path.resolve(p).toLowerCase() === path.resolve(INSTALL_DIR).toLowerCase());

// Where Windows Installer registers the products installed for this user (not HKCU\...\Uninstall,
// which holds the bundle's entry).
const SID = execFileSync('whoami.exe', ['/user', '/fo', 'csv', '/nh'], { encoding: 'utf8' }).trim().split(',').pop().replaceAll('"', '');
const MSI_PRODUCTS_KEY = `HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\${SID}\\Products`;

// Installed Apps entries named BeDD, the bundle's and the MSI's: [{ key, version, systemComponent, uninstall }].
function arpEntries() {
  const entries = [];
  for (const root of [UNINSTALL_KEY, MSI_PRODUCTS_KEY]) {
    const out = reg('query', root, '/s', '/f', 'BeDD', '/d', '/e') ?? '';
    for (const key of out.split(/\r?\n/).filter(l => l.startsWith('HKEY_'))) {
      const values = reg('query', key) ?? '';
      const get = n => values.match(new RegExp(`^\\s+${n}\\s+REG_\\w+\\s+(.*)$`, 'm'))?.[1].trim();
      if (get('DisplayName') !== 'BeDD') continue;
      entries.push({
        key,
        version: get('DisplayVersion'),
        systemComponent: get('SystemComponent') === '0x1',
        uninstall: get('QuietUninstallString') ?? get('UninstallString'),
      });
    }
  }
  return entries;
}

// After a failed scenario: uninstall what it left, the bundle first (it owns its MSI).
function removeLeftovers() {
  for (const e of arpEntries().sort((a, b) => a.systemComponent - b.systemComponent)) {
    if (!arpEntries().some(x => x.key === e.key)) continue;
    const code = e.uninstall?.match(/\{[0-9A-F-]{36}\}/i)?.[0];
    if (e.key.startsWith('HKEY_CURRENT_USER')) spawnSync(e.uninstall, { shell: true, timeout: 10 * 60 * 1000 });
    else if (code) spawnSync('msiexec.exe', ['/x', code, '/qn', '/norestart'], { timeout: 10 * 60 * 1000 });
  }
}

// What Installed Apps shows: entries not hidden as a part of a bundle.
const visibleEntries = () => arpEntries().filter(e => !e.systemComponent);

// msiexec with a verbose log; its exit code (1603 for a failure, 3010 when a reboot is needed).
function msi(name, args, expected = 0) {
  const log = path.join(LOG_DIR, `${name}.log`);
  const r = spawnSync('msiexec.exe', [...args, '/l*v', log], { encoding: 'utf8', timeout: 10 * 60 * 1000 });
  if (r.status !== expected) {
    const tail = fs.existsSync(log) ? fs.readFileSync(log, 'utf16le').split(/\r?\n/).slice(-60).join('\n') : '(no log)';
    assert.fail(`${name}: exit code ${r.status}, expected ${expected}\n${tail}`);
  }
}

// Burn writes its own log next to the one it is given, one per package.
const bundle = (name, args, expected = 0) => {
  const r = spawnSync(setup, [...args, '/log', path.join(LOG_DIR, `${name}.log`)], { encoding: 'utf8', timeout: 10 * 60 * 1000 });
  assert.equal(r.status, expected, `${name}: exit code ${r.status} (logs in ${LOG_DIR})`);
};

function assertInstalled(version) {
  for (const f of ['BeDD.exe', 'COPYING', 'COPYING.LESSER', 'mod/BeDD/BeDD.dll', 'mod/BeDD/manifest.json',
    'mod/BeDD-server/BeDD-server.dll', 'mod/BeDD-server/manifest.json', 'spec/bedd.asyncapi.yaml']) {
    assert.ok(fs.existsSync(path.join(INSTALL_DIR, f)), `${f} is installed`);
  }
  assert.ok(fs.existsSync(SHORTCUT), 'the Start menu shortcut exists');
  assert.ok(onPath(), `${INSTALL_DIR} is on the user's PATH`);
  const visible = visibleEntries();
  assert.equal(visible.length, 1, `one BeDD in Installed Apps (${JSON.stringify(arpEntries())})`);
  assert.equal(visible[0].version, version);
  // The installed BeDD.exe runs, and finds the mod installed next to it.
  const v = JSON.parse(execFileSync(EXE, ['version', '--json'], { encoding: 'utf8' }));
  assert.match(v.version ?? v.app ?? JSON.stringify(v), /\d+\.\d+\.\d+/);
}

function assertRemoved() {
  assert.ok(!fs.existsSync(INSTALL_DIR), `${INSTALL_DIR} is removed`);
  assert.ok(!fs.existsSync(SHORTCUT), 'the Start menu shortcut is removed');
  assert.ok(!onPath(), 'the PATH entry is removed');
  assert.equal(arpEntries().length, 0, 'no BeDD in Installed Apps');
  assert.equal(reg('query', OUR_KEY), null, `${OUR_KEY} is removed`);
}

function managerProcesses() {
  const out = spawnSync('tasklist.exe', ['/fi', 'IMAGENAME eq BeDD.exe', '/fo', 'csv', '/nh', '/v'], { encoding: 'utf8' }).stdout ?? '';
  return out.split(/\r?\n/).filter(l => l.startsWith('"BeDD.exe"'));
}

describe('Windows installers (per user)', { concurrency: false }, () => {
  let marker;

  before(() => {
    assert.ok(!fs.existsSync(EXE) && arpEntries().length === 0 && reg('query', OUR_KEY) === null,
      `BeDD is installed for this user already (${INSTALL_DIR}): these tests would uninstall it`);
    // The user's data must outlive every scenario.
    marker = path.join(LOCAL, 'BeDD', `installer-test-${process.pid}`);
    fs.mkdirSync(path.dirname(marker), { recursive: true });
    fs.writeFileSync(marker, 'kept');
  });

  // A failed scenario must not fail the next ones: each starts with nothing installed.
  afterEach(() => removeLeftovers());

  after(() => {
    assert.equal(fs.readFileSync(marker, 'utf8'), 'kept', '%LOCALAPPDATA%\\BeDD is left alone');
    fs.rmSync(marker);
  });

  it('the MSI installs and uninstalls without elevation', { skip: !msiNew && 'BEDD_MSI not given' }, () => {
    msi('msi-install', ['/i', msiNew, '/qn', '/norestart']);
    assertInstalled('0.0.2');
    msi('msi-uninstall', ['/x', msiNew, '/qn', '/norestart']);
    assertRemoved();
  });

  it('a newer MSI replaces an older one, closing the manager window that runs it',
    { skip: !(msiOld && msiNew) && 'BEDD_MSI_OLD and BEDD_MSI not given' }, async () => {
      msi('upgrade-old', ['/i', msiOld, '/qn', '/norestart']);
      assertInstalled('0.0.1');

      // The manager window, as the Start menu shortcut opens it (BeDD.exe without arguments).
      const manager = spawn(EXE, [], { detached: true, stdio: 'ignore' });
      manager.unref();
      for (let i = 0; i < 50 && managerProcesses().length === 0; i++) await new Promise(r => setTimeout(r, 200));
      const hasManager = managerProcesses().length > 0;

      // Restart Manager closes it (ENDSESSION_CLOSEAPP) instead of leaving the old BeDD.exe for a reboot.
      msi('upgrade-new', ['/i', msiNew, '/qn', '/norestart']);
      assertInstalled('0.0.2');
      if (hasManager) {
        for (let i = 0; i < 50 && managerProcesses().length > 0; i++) await new Promise(r => setTimeout(r, 200));
        assert.equal(managerProcesses().length, 0, 'the manager window was closed for the upgrade');
      }

      msi('upgrade-uninstall', ['/x', msiNew, '/qn', '/norestart']);
      assertRemoved();
    });

  it('an older MSI does not replace a newer one', { skip: !(msiOld && msiNew) && 'BEDD_MSI_OLD and BEDD_MSI not given' }, () => {
    msi('downgrade-new', ['/i', msiNew, '/qn', '/norestart']);
    msi('downgrade-old', ['/i', msiOld, '/qn', '/norestart'], 1603);
    assertInstalled('0.0.2');
    msi('downgrade-uninstall', ['/x', msiNew, '/qn', '/norestart']);
    assertRemoved();
  });

  it('setup.exe installs the MSI as its one entry in Installed Apps and uninstalls it',
    { skip: !setup && 'BEDD_SETUP not given' }, () => {
      bundle('bundle-install', ['/quiet', '/norestart']);
      assertInstalled('0.0.2');
      bundle('bundle-uninstall', ['/uninstall', '/quiet', '/norestart']);
      assertRemoved();
    });
});
