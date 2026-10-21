'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require(path.join(process.cwd(), '.pio/ui-test/node_modules/playwright-core'));
const base = (process.argv[2] || 'http://192.168.0.122').replace(/\/$/, '');
const output = path.join(process.cwd(), 'docs/screenshots');
async function api(route, values) {
  const response = await fetch(base + route, values ? {method:'POST',body:new URLSearchParams(values)} : {});
  assert.equal(response.status, 200, await response.clone().text());
  return response.json();
}
(async () => {
  const original = (await api('/api/status')).control;
  fs.mkdirSync(output, {recursive:true});
  let browser;
  try {
    const executablePath = ['C:/Program Files/Google/Chrome/Application/chrome.exe','C:/Program Files (x86)/Google/Chrome/Application/chrome.exe'].find(fs.existsSync);
    assert.ok(executablePath, 'Chrome executable missing');
    browser = await chromium.launch({executablePath,headless:true,args:['--no-proxy-server']});
    const page = await browser.newPage({viewport:{width:1440,height:1100},deviceScaleFactor:1});
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await api('/api/control',{auto_enabled:'1',strategy:'threshold'});
    await page.goto(base,{waitUntil:'networkidle'});
    await page.waitForFunction(() => document.getElementById('temperature').textContent !== '--');
    await page.waitForFunction(() => document.getElementById('plot-note').textContent.includes('samples'));
    await page.screenshot({path:path.join(output,'dashboard-desktop.png'),fullPage:true});
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:path.join(output,'dashboard-mobile.png')});
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),false);
    await page.setViewportSize({width:1440,height:1100});
    const switched = page.waitForResponse(r => r.url().endsWith('/api/control') && r.request().method()==='POST');
    await page.locator('#mode-select').selectOption('curve');
    assert.equal((await switched).status(),200);
    await page.waitForFunction(() => document.getElementById('mode').textContent==='Curve' && document.getElementById('curve-chart').dataset.temperature !== undefined);
    await page.locator('#curve-card').screenshot({path:path.join(output,'curve-editor.png')});
    await page.locator('#curve-chart').screenshot({path:path.join(output,'curve-live-point.png')});
    await page.goto(base+'/wifi',{waitUntil:'networkidle'});
    assert.equal(await page.locator('#ssid').inputValue(),'');
    assert.equal(await page.locator('#password').inputValue(),'');
    await page.locator('main').screenshot({path:path.join(output,'wifi-setup.png')});
    assert.deepEqual(errors,[]);
    console.log('Captured desktop, mobile, curve editor/live point, and blank Wi-Fi setup from the physical ESP32.');
  } finally {
    if(browser) await browser.close();
    const restored = {};
    for(const key of ['stop_c','start_c','full_c','offset_c','min_percent','max_percent'])restored[key]=String(original[key]);
    restored.auto_enabled=original.auto_enabled?'1':'0';
    restored.strategy=original.strategy;
    restored.curve_hysteresis_c=String(original.curve_hysteresis_c);
    restored.curve=original.curve_points.map(p=>p.temperature_c+':'+p.percent).join(',');
    await api('/api/control',restored);
    console.log('Original control configuration restored.');
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
