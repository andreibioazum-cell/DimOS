/*
 * DimOS 14 Mobile -- entry point.
 *
 * Boots the shell, keeps the phone frame scaled on desktop and registers
 * the service worker so the whole OS keeps working offline once installed.
 */

import { createShell } from './shell.js';
import { toast } from './ui.js';

const shell = createShell();
shell.init();

/* Handy for the curious (and for the screenshot harness): the shell is a
 * plain object, no globals hidden away. */
window.dimos = shell;

/* A tiny first-run greeting, once the boot animation has finished. */
setTimeout(() => {
  if (shell.state.locked) {
    const hint = document.getElementById('lock-hint');
    if (hint) hint.classList.add('ready');
  }
}, 1500);

/* Offline + installable. Service workers need http(s), so file:// and the
 * bare-metal disk stay quiet. */
if ('serviceWorker' in navigator && /^https?:$/.test(location.protocol)) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').then((registration) => {
      registration.addEventListener('updatefound', () => {
        const worker = registration.installing;
        if (!worker) return;
        worker.addEventListener('statechange', () => {
          if (worker.state === 'installed' && navigator.serviceWorker.controller) {
            toast('A new DimOS is ready — restart to update', 3200);
          }
        });
      });
    }).catch(() => { /* offline support is optional */ });
  });
}

/* Never let a double tap zoom the phone UI. */
document.addEventListener('gesturestart', (event) => event.preventDefault());
document.addEventListener('dblclick', (event) => event.preventDefault(), { passive: false });
