/*
 * DimOS 14 Mobile -- shared data.
 *
 * The phone edition keeps the same nine applications as the bare-metal
 * kernel (plus Weather and the Core bridge to the x86 build). Everything
 * here is plain data: the shell renders it, the apps consume it.
 */

/* ------------------------------------------------------------------ */
/* Wallpapers                                                          */
/* ------------------------------------------------------------------ */

export const WALLPAPERS = [
  {
    id: 'spectrum',
    name: 'Spectrum',
    dark: false,
    css:
      'radial-gradient(135% 100% at 12% 6%, #ffb648 0%, rgba(255,182,72,0) 46%),' +
      'radial-gradient(120% 95% at 92% 12%, #ff3d7f 0%, rgba(255,61,127,0) 52%),' +
      'radial-gradient(150% 120% at 78% 96%, #0a84ff 0%, rgba(10,132,255,0) 60%),' +
      'radial-gradient(130% 110% at 8% 88%, #5e5ce6 0%, rgba(94,92,230,0) 62%),' +
      'linear-gradient(160deg, #3a1e78 0%, #7b2d9d 48%, #12205e 100%)'
  },
  {
    id: 'aurora',
    name: 'Aurora',
    dark: true,
    css:
      'radial-gradient(120% 90% at 20% 10%, #20d5c8 0%, rgba(32,213,200,0) 52%),' +
      'radial-gradient(120% 110% at 88% 18%, #6a5cff 0%, rgba(106,92,255,0) 56%),' +
      'radial-gradient(140% 120% at 50% 100%, #00b3ff 0%, rgba(0,179,255,0) 62%),' +
      'linear-gradient(155deg, #04142c 0%, #0b2a4a 46%, #061a33 100%)'
  },
  {
    id: 'dune',
    name: 'Dune',
    dark: false,
    css:
      'radial-gradient(120% 100% at 18% 12%, #ffe0a3 0%, rgba(255,224,163,0) 55%),' +
      'radial-gradient(130% 110% at 88% 22%, #ff8a5c 0%, rgba(255,138,92,0) 58%),' +
      'radial-gradient(150% 120% at 50% 100%, #8a3450 0%, rgba(138,52,80,0) 70%),' +
      'linear-gradient(165deg, #ffca7a 0%, #f08a5d 40%, #7b2b56 100%)'
  },
  {
    id: 'midnight',
    name: 'Midnight',
    dark: true,
    css:
      'radial-gradient(130% 100% at 22% 8%, #2b3f7a 0%, rgba(43,63,122,0) 60%),' +
      'radial-gradient(130% 110% at 84% 30%, #4b1f6b 0%, rgba(75,31,107,0) 62%),' +
      'radial-gradient(150% 120% at 50% 100%, #0a1530 0%, rgba(10,21,48,0) 70%),' +
      'linear-gradient(160deg, #101a33 0%, #1a1430 55%, #05070f 100%)'
  }
];

/* ------------------------------------------------------------------ */
/* Applications                                                        */
/* ------------------------------------------------------------------ */

/* Each app icon is a gradient tile with a white glyph, like iOS 14.
 * `status` is the colour of the status-bar glyphs inside the app. */
export const APPS = [
  {
    id: 'files',
    name: 'Files',
    tint: 'linear-gradient(180deg, #5ec2ff 0%, #0a84ff 100%)',
    glyph: '<path d="M3.2 6.4h5.1l1.9 2.1h10.6c.7 0 1.2.6 1.2 1.3v8.8c0 .7-.5 1.3-1.2 1.3H3.2c-.7 0-1.2-.6-1.2-1.3V7.7c0-.7.5-1.3 1.2-1.3z" fill="#fff"/>',
    status: 'light',
    title: 'Files',
    category: 'Utilities'
  },
  {
    id: 'music',
    name: 'Music',
    tint: 'linear-gradient(180deg, #ff6a8b 0%, #ff2d55 100%)',
    glyph:
      '<path d="M17.6 3.5c0-.6-.5-1-1.1-.9l-7.1 1.6c-.5.1-.8.5-.8 1v9.5a2.9 2.9 0 1 0 1.9 2.7V9.1l5.2-1.2v5.2a2.9 2.9 0 1 0 1.9 2.7V3.5z" fill="#fff"/>',
    status: 'light',
    title: 'Now Playing',
    category: 'Music'
  },
  {
    id: 'calculator',
    name: 'Calculator',
    tint: 'linear-gradient(180deg, #3a3a3c 0%, #161618 100%)',
    glyph:
      '<rect x="4" y="2.4" width="16" height="19.2" rx="3" fill="none" stroke="#fff" stroke-width="1.6"/>' +
      '<rect x="6.6" y="5.2" width="10.8" height="3.2" rx="1" fill="#fff" opacity=".9"/>' +
      '<circle cx="7.6" cy="12.2" r="1.35" fill="#ff9f0a"/><circle cx="12" cy="12.2" r="1.35" fill="#fff"/>' +
      '<circle cx="16.4" cy="12.2" r="1.35" fill="#fff"/><circle cx="7.6" cy="16.6" r="1.35" fill="#fff"/>' +
      '<circle cx="12" cy="16.6" r="1.35" fill="#fff"/><circle cx="16.4" cy="16.6" r="1.35" fill="#fff"/>',
    status: 'light',
    title: 'Calculator',
    category: 'Utilities'
  },
  {
    id: 'paint',
    name: 'Paint',
    tint: 'linear-gradient(180deg, #d17bff 0%, #9747ff 100%)',
    glyph:
      '<path d="M19.8 4.2a2.3 2.3 0 0 0-3.3 0l-8.1 8.1 3.3 3.3 8.1-8.1a2.3 2.3 0 0 0 0-3.3z" fill="#fff"/>' +
      '<path d="M7 13.6c-1.4.4-2.4 1.7-2.4 3.2 0 1.2-.9 2.2-2.1 2.4 1.1 1.2 2.6 1.9 4.2 1.9 2.4 0 4.4-2 4.4-4.4L7 13.6z" fill="#fff"/>',
    status: 'light',
    title: 'Paint',
    category: 'Creativity'
  },
  {
    id: 'snake',
    name: 'Snake',
    tint: 'linear-gradient(180deg, #b6f06b 0%, #34c759 100%)',
    glyph:
      '<path d="M6 6.6h6.4a3.4 3.4 0 0 1 0 6.8H9.6a3.4 3.4 0 0 0 0 6.8H18" fill="none" stroke="#fff" stroke-width="2.6" stroke-linecap="round"/>' +
      '<circle cx="6.2" cy="6.6" r="2.1" fill="#fff"/>',
    status: 'light',
    title: 'Snake',
    category: 'Games'
  },
  {
    id: 'mines',
    name: 'Mines',
    tint: 'linear-gradient(180deg, #8e8e93 0%, #48484a 100%)',
    glyph:
      '<circle cx="12" cy="12.4" r="5.6" fill="#fff"/>' +
      '<path d="M12 3.2v3M12 18.6v3M3.2 12.4h3M17.8 12.4h3M5.8 6.2l2.1 2.1M16.1 16.5l2.1 2.1M18.2 6.2l-2.1 2.1M7.9 16.5l-2.1 2.1" stroke="#fff" stroke-width="1.7" stroke-linecap="round"/>',
    status: 'light',
    title: 'Mines',
    category: 'Games'
  },
  {
    id: 'cheesy',
    name: 'Cheesy',
    tint: 'linear-gradient(180deg, #ffe066 0%, #ffb300 100%)',
    glyph:
      '<circle cx="12" cy="12.6" r="7.4" fill="#fff"/>' +
      '<circle cx="9.4" cy="10.4" r="1.35" fill="#ffb300"/><circle cx="14.6" cy="9.6" r="1.05" fill="#ffb300"/>' +
      '<circle cx="13.9" cy="14.4" r="1.5" fill="#ffb300"/><circle cx="9.2" cy="15.2" r=".95" fill="#ffb300"/>' +
      '<path d="M8.6 4.2c.6-.9 1.9-.6 2.1.5M12.8 3.6c.9-.4 1.8.5 1.5 1.4" stroke="#fff" stroke-width="1.3" stroke-linecap="round" fill="none"/>',
    status: 'dark',
    title: 'Cheesy Balls',
    category: 'Games'
  },
  {
    id: 'terminal',
    name: 'Terminal',
    tint: 'linear-gradient(180deg, #2c2c2e 0%, #0b0b0d 100%)',
    glyph:
      '<path d="M4.6 7.2l4.6 4.3-4.6 4.3" fill="none" stroke="#30d158" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round"/>' +
      '<path d="M11.4 16.6h8" stroke="#fff" stroke-width="2.1" stroke-linecap="round"/>',
    status: 'light',
    title: 'Terminal',
    category: 'Utilities'
  },
  {
    id: 'settings',
    name: 'Settings',
    tint: 'linear-gradient(180deg, #c7c7cc 0%, #8e8e93 100%)',
    glyph:
      '<circle cx="12" cy="12" r="3.1" fill="#fff"/>' +
      '<path d="M12 2.6l1.2 2.6 2.8-.7 1.1 2.7 2.8.8-.5 2.9 2 2.1-2 2.1.5 2.9-2.8.8-1.1 2.7-2.8-.7L12 21.4l-1.2-2.6-2.8.7-1.1-2.7-2.8-.8.5-2.9-2-2.1 2-2.1-.5-2.9 2.8-.8 1.1-2.7 2.8.7L12 2.6z" fill="none" stroke="#fff" stroke-width="1.5" stroke-linejoin="round"/>',
    status: 'dark',
    title: 'Settings',
    category: 'Utilities'
  },
  {
    id: 'weather',
    name: 'Weather',
    tint: 'linear-gradient(180deg, #63b3ff 0%, #0a6cff 100%)',
    glyph:
      '<circle cx="9" cy="9.4" r="3.5" fill="#ffe066"/>' +
      '<path d="M10.6 17.8h7.1a3.1 3.1 0 0 0 .3-6.2 4.4 4.4 0 0 0-8.5-1 3.6 3.6 0 0 0 1.1 7.2z" fill="#fff"/>',
    status: 'light',
    title: 'Weather',
    category: 'Information'
  },
  {
    id: 'core',
    name: 'Core',
    tint: 'linear-gradient(180deg, #7d7aff 0%, #4b45d6 100%)',
    glyph:
      '<rect x="6.4" y="6.4" width="11.2" height="11.2" rx="2.2" fill="none" stroke="#fff" stroke-width="1.7"/>' +
      '<rect x="9.6" y="9.6" width="4.8" height="4.8" rx="1" fill="#fff"/>' +
      '<path d="M9.4 2.8v2.4M14.6 2.8v2.4M9.4 18.8v2.4M14.6 18.8v2.4M2.8 9.4h2.4M2.8 14.6h2.4M18.8 9.4h2.4M18.8 14.6h2.4" stroke="#fff" stroke-width="1.7" stroke-linecap="round"/>',
    status: 'light',
    title: 'Core',
    category: 'Information'
  }
];

export const APP_BY_ID = Object.fromEntries(APPS.map((app) => [app.id, app]));

export const DOCK_APPS = ['files', 'music', 'calculator', 'settings'];

/* The first home page: a big weather widget, a clock widget and the app
 * grid. Widgets use the same four-column rhythm as the icons. */
export const HOME_PAGES = [
  [
    { type: 'weather-widget', span: 4, rows: 2 },
    { type: 'clock-widget', span: 2, rows: 2 },
    { type: 'calendar-widget', span: 2, rows: 2 },
    { type: 'app', id: 'files' },
    { type: 'app', id: 'music' },
    { type: 'app', id: 'calculator' },
    { type: 'app', id: 'paint' },
    { type: 'app', id: 'snake' },
    { type: 'app', id: 'mines' },
    { type: 'app', id: 'cheesy' },
    { type: 'app', id: 'terminal' },
    { type: 'app', id: 'settings' },
    { type: 'app', id: 'weather' },
    { type: 'app', id: 'core' }
  ],
  [
    { type: 'music-widget', span: 4, rows: 2 },
    { type: 'app', id: 'files' },
    { type: 'app', id: 'terminal' },
    { type: 'app', id: 'core' },
    { type: 'app', id: 'paint' },
    { type: 'battery-widget', span: 2, rows: 2 },
    { type: 'app', id: 'calculator' },
    { type: 'app', id: 'settings' }
  ]
];

/* ------------------------------------------------------------------ */
/* Music                                                               */
/* ------------------------------------------------------------------ */

export const TRACKS = [
  {
    title: 'Chime',
    artist: 'DimOS Sound',
    album: 'Pocket Sessions',
    length: 168,
    art: 'linear-gradient(150deg, #ffd166 0%, #ff6b6b 48%, #845ef7 100%)'
  },
  {
    title: 'March',
    artist: 'DimOS Sound',
    album: 'Pocket Sessions',
    length: 204,
    art: 'linear-gradient(150deg, #4facfe 0%, #00f2fe 52%, #0a6cff 100%)'
  },
  {
    title: 'Waltz',
    artist: 'DimOS Sound',
    album: 'Pocket Sessions',
    length: 186,
    art: 'linear-gradient(150deg, #ff9a9e 0%, #fecfef 46%, #a18cd1 100%)'
  },
  {
    title: 'Night Drive',
    artist: 'Core Trio',
    album: 'Bare Metal',
    length: 231,
    art: 'linear-gradient(150deg, #0f2027 0%, #2c5364 52%, #ff2d55 100%)'
  }
];

/* ------------------------------------------------------------------ */
/* Weather                                                             */
/* ------------------------------------------------------------------ */

export const WEATHER = {
  city: 'Frankfurt',
  temp: 18,
  condition: 'Mostly Sunny',
  high: 21,
  low: 12,
  feels: 17,
  humidity: 58,
  wind: 11,
  uv: 3,
  rain: 10,
  hourly: [
    { h: 'Now', t: 18, icon: 'sun' },
    { h: '14', t: 19, icon: 'sun' },
    { h: '15', t: 20, icon: 'sun' },
    { h: '16', t: 21, icon: 'cloud-sun' },
    { h: '17', t: 20, icon: 'cloud-sun' },
    { h: '18', t: 19, icon: 'cloud' },
    { h: '19', t: 17, icon: 'cloud' },
    { h: '20', t: 16, icon: 'cloud' },
    { h: '21', t: 15, icon: 'cloud-moon' },
    { h: '22', t: 14, icon: 'cloud-moon' }
  ],
  daily: [
    { day: 'Today', icon: 'sun', low: 12, high: 21 },
    { day: 'Tue', icon: 'cloud-sun', low: 13, high: 22 },
    { day: 'Wed', icon: 'cloud', low: 12, high: 19 },
    { day: 'Thu', icon: 'rain', low: 11, high: 17 },
    { day: 'Fri', icon: 'sun', low: 10, high: 20 },
    { day: 'Sat', icon: 'sun', low: 11, high: 23 },
    { day: 'Sun', icon: 'cloud-sun', low: 12, high: 21 },
    { day: 'Mon', icon: 'cloud', low: 13, high: 19 }
  ]
};

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

export const FILES = [
  { name: 'README.TXT', size: '1.8 KB', kind: 'text', body:
    'DimOS 14 Mobile\n' +
    '===============\n\n' +
    'This is the phone edition: the same shell, apps and iOS 14 look as\n' +
    'the bare-metal kernel, running where real phones live.\n\n' +
    'Install it from Settings > Add to Home Screen, then it opens\n' +
    'full screen, offline, with no browser chrome.\n\n' +
    'The x86 edition still boots under v86 from the Core app.\n' },
  { name: 'BOOT.BIN', size: '512 B', kind: 'bin' },
  { name: 'KERNEL.BIN', size: '55.7 KB', kind: 'bin' },
  { name: 'DIMOS.IMG', size: '1.44 MB', kind: 'disk' },
  { name: 'PAINT.IMG', size: '640 B', kind: 'image' },
  { name: 'CHEESY.LOG', size: '2.4 KB', kind: 'text', body:
    '12:35  NYAM! ball #1\n12:35  NYAM! ball #2\n12:36  BURRRP!\n' +
    '12:36  cooked a fresh batch\n12:37  ball #3 escaped the plate\n' }
];

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */

export const NOTIFICATIONS = [
  {
    id: 'n1',
    app: 'music',
    title: 'DimOS Sound',
    time: 'now',
    body: 'Now playing: Chime',
    accent: '#ff2d55'
  },
  {
    id: 'n2',
    app: 'cheesy',
    title: 'Cheesy Balls',
    time: '9m ago',
    body: '12 balls are getting cold. NYAM!',
    accent: '#ffb300'
  },
  {
    id: 'n3',
    app: 'core',
    title: 'DimOS Core',
    time: '1h ago',
    body: 'The x86 kernel image is ready to boot.',
    accent: '#5e5ce6'
  }
];
