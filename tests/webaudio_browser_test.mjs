import { chromium } from 'playwright-core';

const baseUrl = process.env.RTAUDIO_WEBAUDIO_URL ||
  'http://127.0.0.1:8765/webaudio_playsaw.html';

const browser = await chromium.launch({
  channel: 'chrome',
  headless: true,
  args: [
    '--autoplay-policy=no-user-gesture-required',
    '--disable-dev-shm-usage',
  ],
});

const page = await browser.newPage();
const pageErrors = [];
page.on('pageerror', error => pageErrors.push(String(error)));
page.on('console', message => {
  if (message.type() !== 'error') return;

  const text = message.text();
  // Chromium reports resource 404s once through console.error and once through
  // the HTTP response event. Keep the URL-aware response check below as the
  // authority so real missing application resources still fail the test.
  if (text.startsWith('Failed to load resource: the server responded with a status of 404'))
    return;

  pageErrors.push(`console.error: ${text}`);
});
page.on('response', response => {
  if (response.status() !== 404) return;

  const url = new URL(response.url());
  // Browser metadata probes are unrelated to RtAudio and are not part of the
  // generated example bundle. Any other missing resource remains fatal.
  if (url.pathname === '/favicon.ico' ||
      url.pathname === '/.well-known/appspecific/com.chrome.devtools.json')
    return;

  pageErrors.push(`HTTP 404: ${response.url()}`);
});

async function diagnostics() {
  return page.evaluate(() => ({
    moduleDefined: typeof Module !== 'undefined',
    apiDefined: typeof Module !== 'undefined' && !!Module.RtAudioWeb,
    initializeType:
      typeof Module !== 'undefined' && Module.RtAudioWeb
        ? typeof Module.RtAudioWeb.initialize
        : 'missing',
    state:
      typeof Module !== 'undefined' && Module.RtAudioWeb
        ? Module.RtAudioWeb._state
        : 'missing',
    ready:
      typeof Module !== 'undefined' && Module.RtAudioWeb &&
      typeof Module.RtAudioWeb.isReady === 'function'
        ? Module.RtAudioWeb.isReady()
        : false,
    hasHostContext: window.__rtaudioHostContext instanceof AudioContext,
    contextState: window.__rtaudioHostContext?.state || 'missing',
    streamRunning:
      typeof Module !== 'undefined' &&
      typeof Module._rtaudio_webaudio_playsaw_is_running === 'function'
        ? Module._rtaudio_webaudio_playsaw_is_running()
        : -1,
    hasNode:
      typeof Module !== 'undefined' && Module.RtAudioWeb
        ? !!Module.RtAudioWeb.getNode('rtaudio-playsaw')
        : false,
    status: document.getElementById('status')?.textContent || '',
    details: document.getElementById('details')?.textContent || '',
  }));
}

try {
  await page.goto(baseUrl, { waitUntil: 'load' });

  try {
    await page.waitForFunction(() =>
      typeof Module !== 'undefined' &&
      Module.RtAudioWeb &&
      typeof Module.RtAudioWeb.initialize === 'function' &&
      typeof Module.RtAudioWeb.isReady === 'function' &&
      Module.RtAudioWeb.isReady() &&
      window.__rtaudioHostContext instanceof AudioContext,
      undefined,
      { timeout: 10000 });
  } catch (error) {
    throw new Error(
      `RtAudio WebAudio bootstrap did not become ready: ${error.message}\n` +
      `diagnostics=${JSON.stringify(await diagnostics())}\n` +
      `browserErrors=${JSON.stringify(pageErrors)}`);
  }

  const bootstrap = await page.evaluate(() => ({
    isolated: crossOriginIsolated,
    ready: Module.RtAudioWeb.isReady(),
    sameContext:
      Module.RtAudioWeb.getContext() === window.__rtaudioHostContext,
    state: window.__rtaudioHostContext.state,
  }));

  if (!bootstrap.isolated)
    throw new Error('Page is not cross-origin isolated');
  if (!bootstrap.ready)
    throw new Error('RtAudio WebAudio runtime did not become ready');
  if (!bootstrap.sameContext)
    throw new Error('RtAudio did not adopt the host-owned AudioContext');

  await page.click('#start');
  try {
    await page.waitForFunction(() =>
      Module._rtaudio_webaudio_playsaw_is_running() !== 0,
      undefined,
      { timeout: 5000 });
  } catch (error) {
    throw new Error(
      `RtAudio stream did not start: ${error.message}\n` +
      `diagnostics=${JSON.stringify(await diagnostics())}\n` +
      `browserErrors=${JSON.stringify(pageErrors)}`);
  }

  const firstNodeOk = await page.evaluate(() => {
    const api = Module.RtAudioWeb;
    const node = api.getNode('rtaudio-playsaw');
    window.__rtaudioFirstNode = node;
    return !!node &&
      node instanceof AudioWorkletNode &&
      api.getContext() === window.__rtaudioHostContext &&
      api.playsawGraph &&
      api.playsawGraph.source === node;
  });
  if (!firstNodeOk)
    throw new Error('RtAudio AudioWorkletNode was not published into the host graph');

  await page.evaluate(() => {
    const frequency = document.getElementById('frequency');
    const gain = document.getElementById('gain');
    frequency.value = '440';
    gain.value = '0.5';
    frequency.dispatchEvent(new Event('input', { bubbles: true }));
    gain.dispatchEvent(new Event('input', { bubbles: true }));
  });

  await page.waitForFunction(() =>
    Math.abs(Module._rtaudio_webaudio_playsaw_frequency() - 440) < 1 &&
    Math.abs(Module._rtaudio_webaudio_playsaw_gain() - 0.5) < 0.01);

  await page.waitForFunction(() =>
    Module._rtaudio_webaudio_playsaw_meter_left() > 0 ||
    Module._rtaudio_webaudio_playsaw_meter_right() > 0,
    undefined,
    { timeout: 5000 });

  await page.click('#stop');
  await page.waitForFunction(() =>
    Module._rtaudio_webaudio_playsaw_is_running() === 0);

  await page.click('#start');
  await page.waitForFunction(() =>
    Module._rtaudio_webaudio_playsaw_is_running() !== 0);

  const reusedNode = await page.evaluate(() =>
    Module.RtAudioWeb.getNode('rtaudio-playsaw') === window.__rtaudioFirstNode);
  if (!reusedNode)
    throw new Error('Stop/Start did not reuse the AudioWorkletNode');

  if (pageErrors.length)
    throw new Error(`Browser errors:\n${pageErrors.join('\n')}`);
} finally {
  await browser.close();
}
