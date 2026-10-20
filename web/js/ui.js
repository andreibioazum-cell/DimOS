/*
 * DimOS 14 Mobile -- small shared helpers: icons, DOM, settings, clock,
 * haptics, sound and the toast. No framework, no build step.
 */

/* ------------------------------------------------------------------ */
/* DOM                                                                 */
/* ------------------------------------------------------------------ */

export const $ = (selector, root = document) => root.querySelector(selector);
export const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

export function el(tag, className, markup) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (markup !== undefined) node.innerHTML = markup;
  return node;
}

/* ------------------------------------------------------------------ */
/* Icons                                                               */
/* ------------------------------------------------------------------ */

const GLYPHS = {
  'chevron-left': '<path d="M15.5 4.2L7.7 12l7.8 7.8" stroke="currentColor" stroke-width="2.1" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  'chevron-right': '<path d="M8.5 4.2L16.3 12l-7.8 7.8" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  'chevron-down': '<path d="M4.2 8.5L12 16.3l7.8-7.8" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  'chevron-up': '<path d="M4.2 15.5L12 7.7l7.8 7.8" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  'arrow-up': '<path d="M12 19V5M6 11l6-6 6 6" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  search: '<circle cx="10.7" cy="10.7" r="6" stroke="currentColor" stroke-width="2" fill="none"/><path d="M15.2 15.2l4.6 4.6" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
  play: '<path d="M8 5.2c0-.9 1-1.4 1.7-.9l9 6.8c.6.4.6 1.4 0 1.8l-9 6.8c-.7.5-1.7 0-1.7-.9V5.2z" fill="currentColor"/>',
  pause: '<rect x="6.4" y="4.6" width="4.2" height="14.8" rx="1.4" fill="currentColor"/><rect x="13.4" y="4.6" width="4.2" height="14.8" rx="1.4" fill="currentColor"/>',
  next: '<path d="M5.2 5.6c0-.9 1-1.4 1.7-.9l8 6.4c.6.5.6 1.3 0 1.8l-8 6.4c-.7.5-1.7 0-1.7-.9V5.6z" fill="currentColor"/><rect x="16.8" y="4.4" width="2.6" height="15.2" rx="1.2" fill="currentColor"/>',
  prev: '<path d="M18.8 5.6c0-.9-1-1.4-1.7-.9l-8 6.4c-.6.5-.6 1.3 0 1.8l8 6.4c.7.5 1.7 0 1.7-.9V5.6z" fill="currentColor"/><rect x="4.6" y="4.4" width="2.6" height="15.2" rx="1.2" fill="currentColor"/>',
  shuffle: '<path d="M3.5 6.5h3.2c1.4 0 2.6.7 3.4 1.8l4.6 7.4c.7 1.1 1.9 1.8 3.3 1.8h2.5M17.5 14.5l3 3-3 3" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/><path d="M3.5 17.5h3.2c1.4 0 2.6-.7 3.4-1.8l1.2-1.9M13.9 8.5l1-1.6c.7-1.1 1.9-1.8 3.3-1.8h2.5M17.5 2.5l3 3-3 3" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  repeat: '<path d="M6.5 4.5h11a3 3 0 0 1 3 3v4M17.5 2.5l2.5 2.5-2.5 2.5M17.5 19.5h-11a3 3 0 0 1-3-3v-4M6.5 21.5L4 19l2.5-2.5" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  volume: '<path d="M4 9.5h3.4L12 5.6v12.8L7.4 14.5H4z" fill="currentColor"/><path d="M15.6 8.4a5 5 0 0 1 0 7.2M18.2 5.8a8.7 8.7 0 0 1 0 12.4" stroke="currentColor" stroke-width="1.7" fill="none" stroke-linecap="round"/>',
  sun: '<circle cx="12" cy="12" r="4.1" fill="currentColor"/><path d="M12 2.6v2.6M12 18.8v2.6M2.6 12h2.6M18.8 12h2.6M5.4 5.4l1.8 1.8M16.8 16.8l1.8 1.8M18.6 5.4l-1.8 1.8M7.2 16.8l-1.8 1.8" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
  moon: '<path d="M20 14.6A8.6 8.6 0 0 1 9.4 4a8.7 8.7 0 1 0 10.6 10.6z" fill="currentColor"/>',
  wifi: '<path d="M12 18.2l3-3.5a4.6 4.6 0 0 0-6 0l3 3.5z" fill="currentColor"/><path d="M4.8 8.3a10.8 10.8 0 0 1 14.4 0M7.8 11.6a6.8 6.8 0 0 1 8.4 0" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round"/>',
  bluetooth: '<path d="M7 7.4L17 16.6 12 21V3l5 4.4L7 16.6" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  airplane: '<path d="M12 2.8c.9 0 1.6.8 1.6 1.8v4.7l6.8 3.9v2.1l-6.8-2v4.4l2.4 1.7v1.7L12 20l-4 1.1v-1.7l2.4-1.7v-4.4l-6.8 2v-2.1l6.8-3.9V4.6c0-1 .7-1.8 1.6-1.8z" fill="currentColor"/>',
  cellular: '<rect x="3.2" y="13.6" width="3.4" height="7.2" rx="1" fill="currentColor"/><rect x="8.2" y="10.2" width="3.4" height="10.6" rx="1" fill="currentColor"/><rect x="13.2" y="6.8" width="3.4" height="14" rx="1" fill="currentColor"/><rect x="18.2" y="3.4" width="3.4" height="17.4" rx="1" fill="currentColor"/>',
  flashlight: '<path d="M8.8 3h6.4l.7 4.4-1.8 2.8v9.4c0 .8-.6 1.4-1.4 1.4h-1.4c-.8 0-1.4-.6-1.4-1.4v-9.4L8.1 7.4 8.8 3z" fill="currentColor"/>',
  timer: '<circle cx="12" cy="13.2" r="7.8" stroke="currentColor" stroke-width="1.9" fill="none"/><path d="M12 8.8v4.4l2.9 1.7M9.6 2.6h4.8" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round"/>',
  camera: '<path d="M4.4 8.1h3.1l1.5-2.1h6l1.5 2.1h3.1c.8 0 1.4.6 1.4 1.4v7.7c0 .8-.6 1.4-1.4 1.4H4.4c-.8 0-1.4-.6-1.4-1.4V9.5c0-.8.6-1.4 1.4-1.4z" stroke="currentColor" stroke-width="1.7" fill="none"/><circle cx="12" cy="13.4" r="3.2" stroke="currentColor" stroke-width="1.7" fill="none"/>',
  calculator: '<rect x="5" y="2.8" width="14" height="18.4" rx="2.6" stroke="currentColor" stroke-width="1.7" fill="none"/><rect x="7.6" y="5.4" width="8.8" height="3.2" rx="1" fill="currentColor"/><circle cx="8.4" cy="12.4" r="1.05" fill="currentColor"/><circle cx="12" cy="12.4" r="1.05" fill="currentColor"/><circle cx="15.6" cy="12.4" r="1.05" fill="currentColor"/><circle cx="8.4" cy="16.2" r="1.05" fill="currentColor"/><circle cx="12" cy="16.2" r="1.05" fill="currentColor"/><circle cx="15.6" cy="16.2" r="1.05" fill="currentColor"/>',
  lock: '<path d="M7.2 10.4V8a4.8 4.8 0 0 1 9.6 0v2.4" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round"/><rect x="5.3" y="10.2" width="13.4" height="9.6" rx="2.6" fill="currentColor"/>',
  x: '<path d="M6 6l12 12M18 6L6 18" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
  plus: '<path d="M12 5v14M5 12h14" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
  trash: '<path d="M5.5 6.8h13M9.4 6.6V4.8c0-.7.6-1.3 1.3-1.3h2.6c.7 0 1.3.6 1.3 1.3v1.8M7.2 6.8l.8 12.2c0 .8.7 1.4 1.5 1.4h5c.8 0 1.5-.6 1.5-1.4l.8-12.2" stroke="currentColor" stroke-width="1.7" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  folder: '<path d="M3.4 6.6h5l1.8 2h10.4c.6 0 1 .5 1 1v9c0 .6-.4 1-1 1H3.4c-.6 0-1-.4-1-1V7.6c0-.6.4-1 1-1z" fill="currentColor"/>',
  file: '<path d="M6.4 2.8h7.2l4.4 4.4v14H6.4z" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"/><path d="M13.4 3v4.6h4.4" fill="none" stroke="currentColor" stroke-width="1.7"/>',
  download: '<path d="M12 3.4v11.2M7.6 10.6L12 15l4.4-4.4M4.6 19.4h14.8" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  share: '<path d="M12 15.4V3.2M7.8 7.2L12 3l4.2 4.2" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round" stroke-linejoin="round"/><path d="M5.4 11.4v8.4c0 .6.4 1 1 1h11.2c.6 0 1-.4 1-1v-8.4" stroke="currentColor" stroke-width="1.9" fill="none" stroke-linecap="round"/>',
  check: '<path d="M4.8 12.4l4.8 4.8L19.4 7" stroke="currentColor" stroke-width="2.2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
  info: '<circle cx="12" cy="12" r="9.2" stroke="currentColor" stroke-width="1.7" fill="none"/><path d="M12 10.6v6.2M12 7.4v.4" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
  power: '<path d="M12 3.2v8.4M7.2 5.6a8.2 8.2 0 1 0 9.6 0" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round"/>',
  home: '<path d="M4.4 10.8L12 4l7.6 6.8v8.4c0 .6-.4 1-1 1h-4.4v-5.6H9.8v5.6H5.4c-.6 0-1-.4-1-1v-8.4z" fill="currentColor"/>',
  music: '<path d="M17.6 3.5c0-.6-.5-1-1.1-.9l-7.1 1.6c-.5.1-.8.5-.8 1v9.5a2.9 2.9 0 1 0 1.9 2.7V9.1l5.2-1.2v5.2a2.9 2.9 0 1 0 1.9 2.7V3.5z" fill="currentColor"/>',
  cloud: '<path d="M7.4 18.4h9.8a3.7 3.7 0 0 0 .3-7.4 5.2 5.2 0 0 0-10-1.2 4.3 4.3 0 0 0-.1 8.6z" fill="currentColor"/>',
  'cloud-sun': '<circle cx="8.2" cy="8.4" r="3.1" fill="currentColor" opacity=".9"/><path d="M10.6 19h7.6a3.1 3.1 0 0 0 .2-6.2 4.6 4.6 0 0 0-8.8-1 3.6 3.6 0 0 0 1 7.2z" fill="currentColor"/>',
  'cloud-moon': '<path d="M8.8 5.2a4.8 4.8 0 0 1-1 6.9 4.8 4.8 0 1 0 6.4-6.5 5.8 5.8 0 0 1-5.4-.4z" fill="currentColor" opacity=".85"/><path d="M11 19.4h7.4a3 3 0 0 0 .2-6 4.5 4.5 0 0 0-8.6-1 3.5 3.5 0 0 0 1 7z" fill="currentColor"/>',
  rain: '<path d="M7.6 15.4h9.4a3.4 3.4 0 0 0 .3-6.8 5 5 0 0 0-9.6-1.1 4 4 0 0 0-.1 7.9z" fill="currentColor"/><path d="M8.6 17.8l-1 2.4M12 17.8l-1 2.4M15.4 17.8l-1 2.4" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
  gear: '<circle cx="12" cy="12" r="3.2" stroke="currentColor" stroke-width="1.8" fill="none"/><path d="M19.4 14.8a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.9 2.9l-.1-.1a1.7 1.7 0 0 0-2.9 1.2v.2a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.6 1.7 1.7 0 0 0-1.9.4l-.1.1a2 2 0 1 1-2.9-2.9l.1-.1a1.7 1.7 0 0 0-1.2-2.9h-.2a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.6-1.1 1.7 1.7 0 0 0-.4-1.9l-.1-.1a2 2 0 1 1 2.9-2.9l.1.1a1.7 1.7 0 0 0 1.9.3H11a1.7 1.7 0 0 0 1-1.5v-.2a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 2.9 1.2l.1-.1a2 2 0 1 1 2.9 2.9l-.1.1a1.7 1.7 0 0 0-.3 1.9V11a1.7 1.7 0 0 0 1.5 1h.2a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.6 1z" stroke="currentColor" stroke-width="1.3" fill="none"/>',
  heart: '<path d="M12 20.4S3.8 15.9 3.8 9.9A4.4 4.4 0 0 1 12 7.3a4.4 4.4 0 0 1 8.2 2.6c0 6-8.2 10.5-8.2 10.5z" fill="currentColor"/>',
  battery: '<rect x="2.8" y="7.4" width="16.4" height="9.2" rx="2.6" stroke="currentColor" stroke-width="1.6" fill="none"/><rect x="4.8" y="9.4" width="10" height="5.2" rx="1.2" fill="currentColor"/><path d="M21 10.4v3.2" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>'
};

export function icon(name) {
  return `<svg viewBox="0 0 24 24" aria-hidden="true">${GLYPHS[name] || ''}</svg>`;
}

export function appIconHTML(app, className = 'app-icon') {
  return `<span class="${className}" style="background:${app.tint}"><svg viewBox="0 0 24 24" aria-hidden="true">${app.glyph}</svg></span>`;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

const STORAGE_KEY = 'dimos14.settings.v1';

const DEFAULTS = {
  wallpaper: 'spectrum',
  appearance: 'dark',       /* dark | light | auto */
  brightness: 1,
  volume: 0.62,
  haptics: true,
  clicks: false,
  wifi: true,
  bluetooth: true,
  cellular: true,
  airplane: false,
  wifiName: 'DimOS-5G',
  hour24: null,             /* null = follow the locale */
  installDismissed: false,
  lowPower: false,
  focus: false
};

function readSettings() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return Object.assign({}, DEFAULTS, JSON.parse(raw));
  } catch (_) { /* private mode */ }
  return Object.assign({}, DEFAULTS);
}

export const settings = readSettings();

export function saveSettings() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
  } catch (_) { /* storage is optional */ }
}

export function use24Hour() {
  if (settings.hour24 !== null) return !!settings.hour24;
  return !/^en-US/i.test(navigator.language || 'en-US');
}

/* ------------------------------------------------------------------ */
/* Time and dates                                                      */
/* ------------------------------------------------------------------ */

export const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December'];
export const WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday',
  'Thursday', 'Friday', 'Saturday'];
export const WEEKDAYS_SHORT = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

export function clockText(date = new Date()) {
  const h = date.getHours();
  const m = String(date.getMinutes()).padStart(2, '0');
  if (use24Hour()) return `${String(h).padStart(2, '0')}:${m}`;
  const hour = h % 12 === 0 ? 12 : h % 12;
  return `${hour}:${m}`;
}

export function dateText(date = new Date()) {
  return `${WEEKDAYS[date.getDay()]}, ${MONTHS[date.getMonth()]} ${date.getDate()}`;
}

export function shortDate(date = new Date()) {
  return `${WEEKDAYS_SHORT[date.getDay()]}, ${MONTHS[date.getMonth()].slice(0, 3)} ${date.getDate()}`;
}

export function durationText(seconds) {
  const s = Math.max(0, Math.round(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

/* ------------------------------------------------------------------ */
/* Haptics, sound, toast                                               */
/* ------------------------------------------------------------------ */

let audioContext = null;

export function click() {
  if (!settings.clicks) return;
  try {
    audioContext = audioContext || new (window.AudioContext || window.webkitAudioContext)();
    const osc = audioContext.createOscillator();
    const gain = audioContext.createGain();
    osc.type = 'sine';
    osc.frequency.value = 1180;
    gain.gain.setValueAtTime(0.035, audioContext.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.0001, audioContext.currentTime + 0.05);
    osc.connect(gain).connect(audioContext.destination);
    osc.start();
    osc.stop(audioContext.currentTime + 0.06);
  } catch (_) { /* audio is a bonus */ }
}

export function haptic(pattern = 8) {
  if (!settings.haptics) return;
  try {
    if (navigator.vibrate) navigator.vibrate(pattern);
  } catch (_) { /* ignored */ }
}

let toastTimer = 0;
export function toast(message, ms = 2100) {
  const node = document.getElementById('toast');
  if (!node) return;
  node.textContent = message;
  node.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => node.classList.remove('show'), ms);
}

/* ------------------------------------------------------------------ */
/* Tiny event emitter                                                  */
/* ------------------------------------------------------------------ */

export function emitter() {
  const listeners = new Map();
  return {
    on(name, fn) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name).add(fn);
      return () => listeners.get(name).delete(fn);
    },
    emit(name, payload) {
      const set = listeners.get(name);
      if (set) set.forEach((fn) => fn(payload));
    }
  };
}

export const bus = emitter();

/* ------------------------------------------------------------------ */
/* SVG artwork helpers                                                 */
/* ------------------------------------------------------------------ */

export function sunArt() {
  return `<svg viewBox="0 0 64 64" fill="none" aria-hidden="true">
    <circle cx="32" cy="32" r="12" fill="currentColor"/>
    <g stroke="currentColor" stroke-width="4" stroke-linecap="round">
      <path d="M32 6v7M32 51v7M6 32h7M51 32h7M13.6 13.6l5 5M45.4 45.4l5 5M50.4 13.6l-5 5M18.6 45.4l-5 5"/>
    </g>
  </svg>`;
}

export function batteryRing(level) {
  const radius = 44;
  const circumference = 2 * Math.PI * radius;
  return `<svg viewBox="0 0 100 100" aria-hidden="true">
    <circle cx="50" cy="50" r="${radius}" stroke="rgba(255,255,255,.18)" stroke-width="9" fill="none"/>
    <circle cx="50" cy="50" r="${radius}" stroke="#30d158" stroke-width="9" fill="none"
      stroke-linecap="round" stroke-dasharray="${circumference}"
      stroke-dashoffset="${circumference * (1 - level)}"/>
  </svg>`;
}
