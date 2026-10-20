/*
 * DimOS 14 Mobile -- the applications.
 *
 * Every app is a function that builds its DOM and returns
 *   { node, mount?, destroy? }
 * The shell inserts `node` into the app window and calls `mount` once it
 * is laid out, so canvases can measure themselves.
 */

import {
  APPS, APP_BY_ID, TRACKS, WEATHER, FILES, WALLPAPERS
} from './data.js';
import {
  icon, appIconHTML, el, toast, haptic, click, settings, saveSettings,
  durationText, clockText, WEEKDAYS_SHORT, MONTHS, sunArt, batteryRing
} from './ui.js';

/* ------------------------------------------------------------------ */
/* Music player (shared by the app, widget, Control Centre and lock)   */
/* ------------------------------------------------------------------ */

const playerListeners = new Set();

export const player = {
  index: 0,
  playing: false,
  progress: 0,
  get track() {
    return TRACKS[this.index];
  },
  play() {
    this.playing = true;
    emitPlayer();
  },
  pause() {
    this.playing = false;
    emitPlayer();
  },
  toggle() {
    this.playing = !this.playing;
    haptic(10);
    emitPlayer();
  },
  next() {
    this.index = (this.index + 1) % TRACKS.length;
    this.progress = 0;
    haptic(12);
    emitPlayer();
  },
  prev() {
    if (this.progress > 4) {
      this.progress = 0;
    } else {
      this.index = (this.index + TRACKS.length - 1) % TRACKS.length;
    }
    haptic(12);
    emitPlayer();
  },
  seek(fraction) {
    this.progress = Math.max(0, Math.min(1, fraction)) * this.track.length;
    emitPlayer();
  },
  subscribe(fn) {
    playerListeners.add(fn);
    fn(this);
    return () => playerListeners.delete(fn);
  }
};

function emitPlayer() {
  playerListeners.forEach((fn) => fn(player));
}

setInterval(() => {
  if (!player.playing || document.hidden) return;
  player.progress += 1;
  if (player.progress >= player.track.length) {
    player.index = (player.index + 1) % TRACKS.length;
    player.progress = 0;
  }
  emitPlayer();
}, 1000);

/* ------------------------------------------------------------------ */
/* Shared chrome                                                       */
/* ------------------------------------------------------------------ */

const chevron = () => `<svg class="chevron" viewBox="0 0 24 24" aria-hidden="true"><path d="M8.5 4.2L16.3 12l-7.8 7.8" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg>`;

function appShell(ctx, { app, title, content, className = '', right = '', back = 'Back' }) {
  const node = el('div', `appview ${className}`);
  node.dataset.app = app;
  node.innerHTML = `
    <header class="navbar">
      <button class="nav-back" data-back aria-label="${back}">
        ${icon('chevron-left')}<span>${back}</span>
      </button>
      <span class="nav-title">${title}</span>
      ${right}
    </header>
    <div class="content">${content}</div>`;
  node.querySelector('[data-back]').addEventListener('click', () => {
    haptic(6);
    ctx.goHome();
  });
  return node;
}

function switchRow({ label, iconName, tint = '#30d158', on, value = '', onTap }) {
  return `
    <button class="row" data-row>
      ${iconName ? `<span class="row-icon" style="background:${tint}">${icon(iconName)}</span>` : ''}
      <span class="row-label">${label}</span>
      ${value ? `<span class="row-value">${value}</span>` : ''}
      <span class="switch ${on ? 'on' : ''}" role="switch" aria-checked="${on}"></span>
    </button>`;
}

/* ------------------------------------------------------------------ */
/* Calculator                                                          */
/* ------------------------------------------------------------------ */

function calculatorApp(ctx) {
  const node = el('div', 'appview calc');
  node.dataset.app = 'calculator';
  node.innerHTML = `
    <div class="calc-display">
      <div class="calc-history"></div>
      <div class="calc-value">0</div>
    </div>
    <div class="calc-pad">
      <button class="calc-key fn" data-k="clear">AC</button>
      <button class="calc-key fn" data-k="sign">±</button>
      <button class="calc-key fn" data-k="percent">%</button>
      <button class="calc-key op" data-k="/">÷</button>
      <button class="calc-key" data-k="7">7</button>
      <button class="calc-key" data-k="8">8</button>
      <button class="calc-key" data-k="9">9</button>
      <button class="calc-key op" data-k="*">×</button>
      <button class="calc-key" data-k="4">4</button>
      <button class="calc-key" data-k="5">5</button>
      <button class="calc-key" data-k="6">6</button>
      <button class="calc-key op" data-k="-">−</button>
      <button class="calc-key" data-k="1">1</button>
      <button class="calc-key" data-k="2">2</button>
      <button class="calc-key" data-k="3">3</button>
      <button class="calc-key op" data-k="+">+</button>
      <button class="calc-key zero" data-k="0">0</button>
      <button class="calc-key" data-k=".">.</button>
      <button class="calc-key op" data-k="=">=</button>
    </div>`;

  const valueNode = node.querySelector('.calc-value');
  const historyNode = node.querySelector('.calc-history');
  const clearKey = node.querySelector('[data-k="clear"]');

  let display = '0';
  let accumulator = null;
  let pending = null;
  let fresh = true;
  let lastOp = null;
  let lastOperand = null;

  const ops = {
    '+': (a, b) => a + b,
    '-': (a, b) => a - b,
    '*': (a, b) => a * b,
    '/': (a, b) => (b === 0 ? NaN : a / b)
  };
  const symbols = { '+': '+', '-': '−', '*': '×', '/': '÷' };

  function render() {
    let text = display;
    if (text.length > 9 && !text.includes('e')) {
      const n = Number(text);
      text = Math.abs(n) >= 1e10 || (Math.abs(n) < 1e-6 && n !== 0)
        ? n.toExponential(4)
        : String(Number(n.toPrecision(10)));
    }
    valueNode.textContent = text;
    valueNode.classList.toggle('small', text.length > 7);
    valueNode.classList.toggle('tiny', text.length > 10);
    clearKey.textContent = display === '0' && !pending ? 'AC' : 'C';
    historyNode.textContent = pending
      ? `${format(accumulator)} ${symbols[pending]}`
      : (lastOp ? `${format(lastOperand)} ${symbols[lastOp]}` : '');
    node.querySelectorAll('.calc-key.op').forEach((key) => {
      key.classList.toggle('active', pending === key.dataset.k);
    });
  }

  function format(n) {
    if (!Number.isFinite(n)) return 'Error';
    return Number(n.toPrecision(12)).toString();
  }

  function press(key) {
    click();
    haptic(6);
    if (/^[0-9]$/.test(key)) {
      display = fresh || display === '0' ? key : display + key;
      fresh = false;
      lastOp = null;
    } else if (key === '.') {
      if (fresh) {
        display = '0.';
        fresh = false;
      } else if (!display.includes('.')) {
        display += '.';
      }
    } else if (key === 'clear') {
      if (display !== '0' && !pending) {
        display = '0';
      } else {
        display = '0';
        accumulator = null;
        pending = null;
        lastOp = null;
        lastOperand = null;
      }
      fresh = true;
    } else if (key === 'sign') {
      display = display.startsWith('-') ? display.slice(1) : (display === '0' ? '0' : '-' + display);
    } else if (key === 'percent') {
      display = format(Number(display) / 100);
    } else if (['+', '-', '*', '/'].includes(key)) {
      const current = Number(display);
      if (pending && !fresh) {
        accumulator = ops[pending](accumulator, current);
        display = format(accumulator);
      } else if (accumulator === null || lastOp) {
        accumulator = current;
      }
      pending = key;
      fresh = true;
      lastOp = null;
    } else if (key === '=') {
      const current = Number(display);
      if (pending) {
        lastOperand = current;
        lastOp = pending;
        const result = ops[pending](accumulator, current);
        display = format(result);
        accumulator = Number.isFinite(result) ? result : null;
        pending = null;
        fresh = true;
      }
    }
    render();
  }

  node.querySelectorAll('[data-k]').forEach((key) => {
    key.addEventListener('click', () => press(key.dataset.k));
  });

  const onKey = (event) => {
    const map = { Enter: '=', '=': '=', Escape: 'clear', Backspace: 'clear' };
    const key = map[event.key] || event.key;
    if (/^[0-9.+\-*/]$/.test(key)) {
      event.preventDefault();
      press(key);
    } else if (['=', 'Enter'].includes(key)) {
      event.preventDefault();
      press('=');
    }
  };
  window.addEventListener('keydown', onKey);
  render();
  return { node, destroy: () => window.removeEventListener('keydown', onKey) };
}

/* ------------------------------------------------------------------ */
/* Music                                                               */
/* ------------------------------------------------------------------ */

function musicApp(ctx) {
  const node = el('div', 'appview music-app');
  node.dataset.app = 'music';
  node.innerHTML = `
    <div class="bg"></div>
    <div class="bg-veil"></div>
    <header class="navbar" style="padding-left:12px">
      <button class="nav-back" data-back aria-label="Back">${icon('chevron-left')}</button>
      <span class="nav-title">Now Playing</span>
    </header>
    <div class="m-body">
      <div class="art">${icon('music')}</div>
      <div class="meta">
        <div>
          <div class="title">—</div>
          <div class="artist">—</div>
        </div>
        <button class="nav-action" data-more aria-label="More">•••</button>
      </div>
      <div class="track-progress">
        <div class="bar"><i></i></div>
        <div class="times"><span class="elapsed">0:00</span><span class="remain">-0:00</span></div>
      </div>
      <div class="controls">
        <button data-shuffle aria-label="Shuffle">${icon('shuffle')}</button>
        <button data-prev aria-label="Previous">${icon('prev')}</button>
        <button data-play class="play" aria-label="Play">${icon('play')}</button>
        <button data-next aria-label="Next">${icon('next')}</button>
        <button data-repeat aria-label="Repeat">${icon('repeat')}</button>
      </div>
      <div class="up-next">
        <i></i>
        <div>
          <div style="font-size:11px;text-transform:uppercase;letter-spacing:.5px;opacity:.6">Up next</div>
          <div class="next-title" style="color:#fff">—</div>
        </div>
      </div>
    </div>`;

  node.querySelector('[data-back]').addEventListener('click', () => ctx.goHome());
  node.querySelector('[data-more]').addEventListener('click', () =>
    ctx.showSheet({
      title: 'DimOS Sound',
      body: '<p style="font-size:14px;line-height:1.5;opacity:.7;margin:0">The bare-metal kernel plays these three melodies through the PC speaker. On a phone this edition is a silent demo: the timeline runs, the notes do not.</p>'
    }));

  const art = node.querySelector('.art');
  const bg = node.querySelector('.bg');
  const title = node.querySelector('.title');
  const artist = node.querySelector('.artist');
  const bar = node.querySelector('.bar');
  const fill = bar.querySelector('i');
  const elapsed = node.querySelector('.elapsed');
  const remain = node.querySelector('.remain');
  const playBtn = node.querySelector('[data-play]');
  const nextTitle = node.querySelector('.next-title');

  const unsubscribe = player.subscribe((state) => {
    const track = state.track;
    art.style.setProperty('--art', track.art);
    bg.style.setProperty('--art', track.art);
    art.style.background = track.art;
    title.textContent = track.title;
    artist.textContent = track.artist;
    playBtn.innerHTML = state.playing ? icon('pause') : icon('play');
    playBtn.setAttribute('aria-label', state.playing ? 'Pause' : 'Play');
    art.classList.toggle('paused', !state.playing);
    const fraction = Math.min(1, state.progress / track.length);
    fill.style.width = `${fraction * 100}%`;
    elapsed.textContent = durationText(state.progress);
    remain.textContent = `-${durationText(track.length - state.progress)}`;
    const upcoming = TRACKS[(state.index + 1) % TRACKS.length];
    nextTitle.textContent = `${upcoming.title} · ${upcoming.artist}`;
    node.querySelector('.up-next i').style.background = upcoming.art;
  });

  playBtn.addEventListener('click', () => player.toggle());
  node.querySelector('[data-next]').addEventListener('click', () => player.next());
  node.querySelector('[data-prev]').addEventListener('click', () => player.prev());
  node.querySelector('[data-shuffle]').addEventListener('click', () => toast('Shuffle is not available in this demo'));
  node.querySelector('[data-repeat]').addEventListener('click', () => toast('Repeat One'));

  bar.addEventListener('pointerdown', (event) => {
    const rect = bar.getBoundingClientRect();
    const seek = (clientX) => player.seek((clientX - rect.left) / rect.width);
    seek(event.clientX);
    const move = (moveEvent) => seek(moveEvent.clientX);
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  });

  return { node, destroy: unsubscribe };
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

function fileIcon(kind) {
  return icon(kind === 'folder' ? 'folder' : 'file');
}

function filesApp(ctx) {
  const node = appShell(ctx, {
    app: 'files',
    title: 'Files',
    className: 'files',
    right: '<button class="nav-action" data-edit>Edit</button>',
    content: `
      <div class="large-title">Files</div>
      <div class="storage">
        <div class="storage-bar">
          <i style="width:3%;background:#5ac8fa"></i>
          <i style="width:12%;background:#ff9f0a"></i>
          <i style="width:6%;background:#30d158"></i>
        </div>
        <div class="storage-legend"><span>1.4 GB used</span><span>64 GB available</span></div>
      </div>
      <div class="group-title">On My Phone</div>
      <div class="group" id="file-list"></div>
      <div class="group-title">DimOS Core</div>
      <div class="group">
        <button class="file-row" data-core>
          <span class="f-icon" style="color:#bf5af2">${icon('folder')}</span>
          <span class="f-name">Kernel sources</span>
          <span class="f-size">x86</span>
        </button>
        <button class="file-row" data-pc>
          <span class="f-icon" style="color:#0a84ff">${icon('download')}</span>
          <span class="f-name">PC edition</span>
          <span class="f-size">.img</span>
        </button>
      </div>`
  });

  const list = node.querySelector('#file-list');
  let editing = false;

  function renderList() {
    list.innerHTML = FILES.map((file) => `
      <button class="file-row" data-file="${file.name}">
        <span class="f-icon">${fileIcon(file.kind)}</span>
        <span class="f-name">${file.name}</span>
        ${editing
          ? `<span class="f-size" style="color:#ff453a">Delete</span>`
          : `<span class="f-size">${file.size}</span>`}
      </button>`).join('');
  }
  renderList();

  list.addEventListener('click', (event) => {
    const row = event.target.closest('[data-file]');
    if (!row) return;
    const file = FILES.find((item) => item.name === row.dataset.file);
    if (!file) return;
    if (editing) {
      if (file.name === 'KERNEL.BIN') {
        toast('KERNEL.BIN is protected');
        return;
      }
      FILES.splice(FILES.indexOf(file), 1);
      renderList();
      haptic(12);
      return;
    }
    haptic(6);
    if (file.body) {
      ctx.showSheet({
        title: file.name,
        body: `<div class="file-meta"><span>${file.size}</span><span>Today, 12:35</span><span>Plain text</span></div>
               <div class="file-view">${escapeHTML(file.body)}</div>`
      });
    } else {
      ctx.showSheet({
        title: file.name,
        body: `<div class="file-meta"><span>${file.size}</span><span>Binary</span></div>
               <p style="font-size:14px;line-height:1.5;opacity:.65;padding:0 4px">
                 This file belongs to the DimOS Core image. It is listed here so the phone
                 and the bare-metal build share the same file table.
               </p>`
      });
    }
  });

  node.querySelector('[data-edit]').addEventListener('click', (event) => {
    editing = !editing;
    event.currentTarget.textContent = editing ? 'Done' : 'Edit';
    renderList();
  });
  node.querySelector('[data-pc]').addEventListener('click', () => window.open('./pc.html', '_blank'));
  node.querySelector('[data-core]').addEventListener('click', () => ctx.openApp('core'));

  return { node };
}

function escapeHTML(text) {
  return String(text).replace(/[&<>"']/g, (char) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[char]));
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

const PALETTE = [
  '#000000', '#ffffff', '#ff3b30', '#ff9500', '#ffcc00', '#34c759',
  '#00c7be', '#007aff', '#5856d6', '#af52de', '#ff2d55', '#a2845e'
];

function paintApp(ctx) {
  const node = el('div', 'appview paint');
  node.dataset.app = 'paint';
  node.innerHTML = `
    <header class="navbar">
      <button class="nav-back" data-back aria-label="Back">${icon('chevron-left')}<span>Back</span></button>
      <span class="nav-title">Paint</span>
      <button class="nav-action" data-save>Save</button>
    </header>
    <canvas></canvas>
    <div class="paint-tools">
      <div class="paint-colors">
        ${PALETTE.map((color, index) => `<button class="paint-color ${index === 7 ? 'active' : ''}" style="background:${color}" data-color="${color}" aria-label="Colour ${index + 1}"></button>`).join('')}
      </div>
      <div class="paint-row">
        <div class="paint-sizes">
          ${[2, 5, 9].map((size, index) => `
            <button class="paint-size ${index === 1 ? 'active' : ''}" data-size="${size}">
              <i style="width:${size + 3}px;height:${size + 3}px"></i>
            </button>`).join('')}
        </div>
        <div class="paint-spacer"></div>
        <button class="paint-btn" data-undo>Undo</button>
        <button class="paint-btn" data-clear>Clear</button>
      </div>
    </div>`;

  const canvas = node.querySelector('canvas');
  const context = canvas.getContext('2d');
  let color = '#007aff';
  let size = 5;
  let drawing = false;
  let dpr = 1;
  const undoStack = [];

  function resize() {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const snapshot = undoStack.length ? canvas.toDataURL() : null;
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.round(rect.width * dpr);
    canvas.height = Math.round(rect.height * dpr);
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    context.fillStyle = '#fff';
    context.fillRect(0, 0, rect.width, rect.height);
    context.lineJoin = 'round';
    context.lineCap = 'round';
    if (undoStack.length === 1 && snapshot) {
      const image = new Image();
      image.onload = () => context.drawImage(image, 0, 0, rect.width, rect.height);
      image.src = snapshot;
    }
  }

  function position(event) {
    const rect = canvas.getBoundingClientRect();
    return { x: event.clientX - rect.left, y: event.clientY - rect.top };
  }

  function pushUndo() {
    undoStack.push(canvas.toDataURL());
    if (undoStack.length > 12) undoStack.shift();
  }

  canvas.addEventListener('pointerdown', (event) => {
    if (event.button !== 0) return;
    drawing = true;
    canvas.setPointerCapture(event.pointerId);
    pushUndo();
    const point = position(event);
    context.strokeStyle = color;
    context.fillStyle = color;
    context.lineWidth = size;
    context.beginPath();
    context.moveTo(point.x, point.y);
    context.lineTo(point.x + 0.01, point.y);
    context.stroke();
  });

  canvas.addEventListener('pointermove', (event) => {
    if (!drawing) return;
    const point = position(event);
    context.lineTo(point.x, point.y);
    context.stroke();
  });

  const stop = () => { drawing = false; };
  canvas.addEventListener('pointerup', stop);
  canvas.addEventListener('pointercancel', stop);
  canvas.addEventListener('pointerleave', stop);

  node.querySelectorAll('[data-color]').forEach((button) => {
    button.addEventListener('click', () => {
      color = button.dataset.color;
      node.querySelectorAll('[data-color]').forEach((other) => other.classList.toggle('active', other === button));
      haptic(5);
    });
  });

  node.querySelectorAll('[data-size]').forEach((button) => {
    button.addEventListener('click', () => {
      size = Number(button.dataset.size);
      node.querySelectorAll('[data-size]').forEach((other) => other.classList.toggle('active', other === button));
      haptic(5);
    });
  });

  node.querySelector('[data-clear]').addEventListener('click', () => {
    pushUndo();
    const rect = canvas.getBoundingClientRect();
    context.fillStyle = '#fff';
    context.fillRect(0, 0, rect.width, rect.height);
    haptic(8);
  });

  node.querySelector('[data-undo]').addEventListener('click', () => {
    const previous = undoStack.pop();
    if (!previous) {
      toast('Nothing to undo');
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const image = new Image();
    image.onload = () => {
      context.fillStyle = '#fff';
      context.fillRect(0, 0, rect.width, rect.height);
      context.drawImage(image, 0, 0, rect.width, rect.height);
    };
    image.src = previous;
  });

  node.querySelector('[data-save]').addEventListener('click', () => {
    const link = document.createElement('a');
    link.download = 'dimos-paint.png';
    link.href = canvas.toDataURL('image/png');
    link.click();
    toast('Saved to your downloads');
  });

  node.querySelector('[data-back]').addEventListener('click', () => ctx.goHome());

  let resizeTimer = 0;
  const observer = new ResizeObserver(() => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(resize, 60);
  });

  return {
    node,
    mount() {
      resize();
      observer.observe(canvas);
    },
    destroy() {
      clearTimeout(resizeTimer);
      observer.disconnect();
    }
  };
}

/* ------------------------------------------------------------------ */
/* Snake                                                               */
/* ------------------------------------------------------------------ */

function snakeApp(ctx) {
  const node = el('div', 'appview snake');
  node.dataset.app = 'snake';
  node.innerHTML = `
    <div class="snake-head">
      <button class="nav-back" data-back style="color:#30d158;font-size:16px">${icon('chevron-left')}<span>Home</span></button>
      <span class="snake-score">Score <b>0</b></span>
    </div>
    <div class="snake-field">
      <canvas></canvas>
      <div class="snake-overlay" data-overlay>
        <h2>Snake</h2>
        <p>Swipe the field or use the pad.<br>Walls are solid.</p>
        <button class="btn-primary" data-start>Play</button>
      </div>
    </div>
    <div class="snake-dpad">
      <button class="up" data-dir="up" aria-label="Up">${icon('chevron-up')}</button>
      <button class="left" data-dir="left" aria-label="Left">${icon('chevron-left')}</button>
      <button class="right" data-dir="right" aria-label="Right">${icon('chevron-right')}</button>
      <button class="down" data-dir="down" aria-label="Down">${icon('chevron-down')}</button>
    </div>`;

  const canvas = node.querySelector('canvas');
  const overlay = node.querySelector('[data-overlay]');
  const scoreNode = node.querySelector('.snake-score b');
  const context = canvas.getContext('2d');

  const COLS = 18;
  const ROWS = 30;
  let cell = 10;
  let dpr = 1;
  let snake = [];
  let direction = { x: 0, y: 0 };
  let queued = null;
  let food = { x: 9, y: 8 };
  let score = 0;
  let running = false;
  let timer = 0;
  let lastStep = 0;

  function layout() {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.round(rect.width * dpr);
    canvas.height = Math.round(rect.height * dpr);
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    cell = Math.floor(Math.min(rect.width / COLS, rect.height / ROWS));
    draw();
  }

  function reset() {
    snake = [
      { x: 9, y: 18 }, { x: 9, y: 19 }, { x: 9, y: 20 }, { x: 9, y: 21 }
    ];
    direction = { x: 0, y: -1 };
    queued = null;
    score = 0;
    scoreNode.textContent = '0';
    dropFood();
    draw();
  }

  function dropFood() {
    for (let attempt = 0; attempt < 300; attempt += 1) {
      const candidate = {
        x: 1 + Math.floor(Math.random() * (COLS - 2)),
        y: 1 + Math.floor(Math.random() * (ROWS - 2))
      };
      if (!snake.some((part) => part.x === candidate.x && part.y === candidate.y)) {
        food = candidate;
        return;
      }
    }
  }

  function draw() {
    const rect = canvas.getBoundingClientRect();
    context.clearRect(0, 0, rect.width, rect.height);
    const offsetX = (rect.width - cell * COLS) / 2;
    const offsetY = (rect.height - cell * ROWS) / 2;

    context.fillStyle = 'rgba(48,209,88,0.08)';
    for (let y = 0; y < ROWS; y += 1) {
      for (let x = 0; x < COLS; x += 1) {
        if ((x + y) % 2 === 0) {
          context.fillRect(offsetX + x * cell, offsetY + y * cell, cell, cell);
        }
      }
    }

    context.fillStyle = '#ff453a';
    context.beginPath();
    context.arc(offsetX + (food.x + 0.5) * cell, offsetY + (food.y + 0.5) * cell, cell * 0.34, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = 'rgba(255,255,255,.55)';
    context.beginPath();
    context.arc(offsetX + (food.x + 0.38) * cell, offsetY + (food.y + 0.36) * cell, cell * 0.1, 0, Math.PI * 2);
    context.fill();

    snake.forEach((part, index) => {
      const head = index === 0;
      context.fillStyle = head ? '#b6f06b' : `rgba(48,209,88,${1 - index / (snake.length + 4) * 0.55})`;
      const pad = cell * 0.11;
      const x = offsetX + part.x * cell + pad;
      const y = offsetY + part.y * cell + pad;
      const size = cell - pad * 2;
      const radius = head ? size * 0.36 : size * 0.28;
      context.beginPath();
      if (context.roundRect) {
        context.roundRect(x, y, size, size, radius);
      } else {
        context.rect(x, y, size, size);
      }
      context.fill();
    });
  }

  function step() {
    if (!running) return;
    if (queued) {
      direction = queued;
      queued = null;
    }
    const head = { x: snake[0].x + direction.x, y: snake[0].y + direction.y };
    const hitsWall = head.x < 0 || head.y < 0 || head.x >= COLS || head.y >= ROWS;
    const hitsSelf = snake.some((part) => part.x === head.x && part.y === head.y);
    if (hitsWall || hitsSelf) {
      end('Game Over', `You scored ${score}.`);
      return;
    }
    snake.unshift(head);
    if (head.x === food.x && head.y === food.y) {
      score += 1;
      scoreNode.textContent = String(score);
      haptic(10);
      dropFood();
    } else {
      snake.pop();
    }
    draw();
  }

  function end(title, text) {
    running = false;
    overlay.hidden = false;
    overlay.innerHTML = `<h2>${title}</h2><p>${text}</p><button class="btn-primary" data-start>Play again</button>`;
    overlay.querySelector('[data-start]').addEventListener('click', start);
  }

  function start() {
    reset();
    overlay.hidden = true;
    running = true;
    lastStep = performance.now();
    timer = requestAnimationFrame(loop);
    haptic(12);
  }

  function loop(now) {
    if (!running) return;
    if (now - lastStep > 132) {
      lastStep = now;
      step();
    }
    draw();
    timer = requestAnimationFrame(loop);
  }

  function turn(next) {
    const opposites = { up: 'down', down: 'up', left: 'right', right: 'left' };
    const current = direction.y === -1 ? 'up' : direction.y === 1 ? 'down'
      : direction.x === -1 ? 'left' : 'right';
    if (opposites[next] === current) return;
    queued = {
      up: { x: 0, y: -1 },
      down: { x: 0, y: 1 },
      left: { x: -1, y: 0 },
      right: { x: 1, y: 0 }
    }[next] || direction;
    haptic(4);
  }

  overlay.querySelector('[data-start]').addEventListener('click', start);
  node.querySelectorAll('[data-dir]').forEach((button) => {
    button.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      turn(button.dataset.dir);
    });
  });
  node.querySelector('[data-back]').addEventListener('click', () => ctx.goHome());

  let touchStart = null;
  canvas.addEventListener('pointerdown', (event) => {
    touchStart = { x: event.clientX, y: event.clientY };
  });
  canvas.addEventListener('pointerup', (event) => {
    if (!touchStart) return;
    const dx = event.clientX - touchStart.x;
    const dy = event.clientY - touchStart.y;
    if (Math.abs(dx) < 14 && Math.abs(dy) < 14) return;
    turn(Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'right' : 'left') : (dy > 0 ? 'down' : 'up'));
    touchStart = null;
  });

  const onKey = (event) => {
    const map = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', w: 'up', s: 'down', a: 'left', d: 'right' };
    if (map[event.key]) {
      event.preventDefault();
      turn(map[event.key]);
    }
  };
  window.addEventListener('keydown', onKey);

  reset();
  return {
    node,
    mount() {
      layout();
      const observer = new ResizeObserver(layout);
      observer.observe(canvas);
      node.__observer = observer;
    },
    destroy() {
      running = false;
      cancelAnimationFrame(timer);
      window.removeEventListener('keydown', onKey);
      if (node.__observer) node.__observer.disconnect();
    }
  };
}

/* ------------------------------------------------------------------ */
/* Mines                                                               */
/* ------------------------------------------------------------------ */

function minesApp(ctx) {
  const node = el('div', 'appview mines');
  node.dataset.app = 'mines';
  node.innerHTML = `
    <div class="mines-head">
      <div class="mines-stat"><span>Mines</span><b class="m-left">10</b></div>
      <button class="mines-reset" data-reset aria-label="New game">${icon('repeat')}</button>
      <div class="mines-stat"><span>Time</span><b class="m-time">0:00</b></div>
    </div>
    <div class="mines-board" data-board></div>
    <div class="mines-status" data-status>Tap a tile. Long press to flag.</div>
    <div class="mines-foot">
      <button data-flag>Flag mode: off</button>
      <button data-back>Back</button>
    </div>`;

  const boardNode = node.querySelector('[data-board]');
  const statusNode = node.querySelector('[data-status]');
  const leftNode = node.querySelector('.m-left');
  const timeNode = node.querySelector('.m-time');
  const flagButton = node.querySelector('[data-flag]');

  const COLS = 9;
  const ROWS = 9;
  const MINES = 10;
  let cells = [];
  let flagMode = false;
  let started = false;
  let finished = false;
  let timer = 0;
  let seconds = 0;
  let pressTimer = 0;
  let longPressed = false;

  function reset() {
    clearInterval(timer);
    seconds = 0;
    started = false;
    finished = false;
    timeNode.textContent = '0:00';
    leftNode.textContent = String(MINES);
    statusNode.textContent = 'Tap a tile. Long press to flag.';
    flagMode = false;
    flagButton.classList.remove('on');
    flagButton.textContent = 'Flag mode: off';
    cells = Array.from({ length: COLS * ROWS }, (_, index) => ({
      index,
      mine: false,
      open: false,
      flag: false,
      count: 0
    }));
    boardNode.innerHTML = cells.map((cell) =>
      `<button class="mine-cell" data-cell="${cell.index}"></button>`).join('');
  }

  function neighbours(index) {
    const x = index % COLS;
    const y = Math.floor(index / COLS);
    const result = [];
    for (let dy = -1; dy <= 1; dy += 1) {
      for (let dx = -1; dx <= 1; dx += 1) {
        if (!dx && !dy) continue;
        const nx = x + dx;
        const ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < COLS && ny < ROWS) result.push(ny * COLS + nx);
      }
    }
    return result;
  }

  function plant(safeIndex) {
    const banned = new Set([safeIndex, ...neighbours(safeIndex)]);
    let placed = 0;
    while (placed < MINES) {
      const index = Math.floor(Math.random() * cells.length);
      if (cells[index].mine || banned.has(index)) continue;
      cells[index].mine = true;
      placed += 1;
    }
    cells.forEach((cell) => {
      cell.count = neighbours(cell.index).filter((index) => cells[index].mine).length;
    });
  }

  function paint() {
    cells.forEach((cell) => {
      const button = boardNode.children[cell.index];
      if (!button) return;
      button.className = 'mine-cell';
      button.textContent = '';
      delete button.dataset.n;
      if (cell.open) {
        button.classList.add('revealed');
        if (cell.mine) {
          button.classList.add('mine');
          button.textContent = '✳';
        } else if (cell.count) {
          button.textContent = String(cell.count);
          button.dataset.n = String(cell.count);
        }
      } else if (cell.flag) {
        button.classList.add('flag');
        button.textContent = '⚑';
      }
    });
    const flags = cells.filter((cell) => cell.flag).length;
    leftNode.textContent = String(Math.max(0, MINES - flags));
  }

  function openCell(index) {
    const cell = cells[index];
    if (cell.open || cell.flag) return;
    cell.open = true;
    if (cell.mine) {
      lose(index);
      return;
    }
    if (cell.count === 0) {
      neighbours(index).forEach((neighbour) => openCell(neighbour));
    }
    paint();
    checkWin();
  }

  function lose(hitIndex) {
    finished = true;
    clearInterval(timer);
    cells.forEach((cell) => { if (cell.mine) cell.open = true; });
    paint();
    boardNode.children[hitIndex].style.background = '#ff453a';
    statusNode.textContent = 'Boom. Better luck next time.';
    haptic([30, 40, 30]);
  }

  function checkWin() {
    const closed = cells.filter((cell) => !cell.open && !cell.mine).length;
    if (closed === 0) {
      finished = true;
      clearInterval(timer);
      statusNode.textContent = `Cleared in ${durationText(seconds)}.`;
      haptic([12, 30, 12]);
    }
  }

  function toggleFlag(index) {
    const cell = cells[index];
    if (cell.open || finished) return;
    cell.flag = !cell.flag;
    haptic(10);
    paint();
  }

  function begin() {
    if (started) return;
    started = true;
    timer = setInterval(() => {
      seconds += 1;
      timeNode.textContent = durationText(seconds);
    }, 1000);
  }

  boardNode.addEventListener('pointerdown', (event) => {
    const button = event.target.closest('[data-cell]');
    if (!button || finished) return;
    longPressed = false;
    clearTimeout(pressTimer);
    const index = Number(button.dataset.cell);
    pressTimer = setTimeout(() => {
      longPressed = true;
      toggleFlag(index);
    }, 420);
  });

  const clearPress = () => clearTimeout(pressTimer);
  boardNode.addEventListener('pointerup', clearPress);
  boardNode.addEventListener('pointerleave', clearPress);

  boardNode.addEventListener('click', (event) => {
    const button = event.target.closest('[data-cell]');
    if (!button || finished) return;
    if (longPressed) {
      longPressed = false;
      return;
    }
    const index = Number(button.dataset.cell);
    if (flagMode) {
      toggleFlag(index);
      return;
    }
    if (cells[index].flag) return;
    if (!started) {
      plant(index);
      begin();
    }
    openCell(index);
    haptic(5);
  });

  boardNode.addEventListener('contextmenu', (event) => event.preventDefault());

  flagButton.addEventListener('click', () => {
    flagMode = !flagMode;
    flagButton.classList.toggle('on', flagMode);
    flagButton.textContent = `Flag mode: ${flagMode ? 'on' : 'off'}`;
    haptic(6);
  });

  node.querySelector('[data-reset]').addEventListener('click', () => {
    reset();
    haptic(6);
  });
  node.querySelector('[data-back]').addEventListener('click', () => ctx.goHome());

  reset();
  return { node, destroy: () => clearInterval(timer) };
}

/* ------------------------------------------------------------------ */
/* Cheesy Balls                                                        */
/* ------------------------------------------------------------------ */

function cheesyApp(ctx) {
  const node = el('div', 'appview cheesy');
  node.dataset.app = 'cheesy';
  node.innerHTML = `
    <div class="c-head">
      <div>
        <div class="c-title">Cheesy Balls</div>
        <div class="c-sub">fresh from the bare-metal oven</div>
      </div>
      <div class="c-score">NYAM<b>0</b></div>
    </div>
    <div class="cheesy-field" data-field></div>
    <div class="c-foot">
      <button data-cook>Cook more</button>
      <button class="primary" data-burp>Burp</button>
    </div>`;

  const field = node.querySelector('[data-field]');
  const scoreNode = node.querySelector('.c-score b');
  let score = 0;
  let balls = 0;

  function place(ball) {
    const width = field.clientWidth;
    const height = field.clientHeight;
    const size = Math.min(64, Math.max(36, width * 0.12));
    const x = 8 + Math.random() * Math.max(1, width - size - 16);
    const y = 8 + Math.random() * Math.max(1, height - size - 16);
    ball.style.width = `${size}px`;
    ball.style.height = `${size}px`;
    ball.style.left = `${x}px`;
    ball.style.top = `${y}px`;
  }

  function cook() {
    field.innerHTML = '';
    score = 0;
    scoreNode.textContent = '0';
    balls = 9;
    for (let i = 0; i < balls; i += 1) {
      const ball = el('button', 'cheese-ball');
      ball.setAttribute('aria-label', 'Cheese ball');
      ball.addEventListener('click', () => eat(ball), { once: true });
      field.appendChild(ball);
      place(ball);
    }
    haptic(10);
  }

  function eat(ball) {
    if (ball.classList.contains('eaten')) return;
    ball.classList.add('eaten');
    score += 1;
    scoreNode.textContent = String(score);
    haptic(8);
    const fieldRect = field.getBoundingClientRect();
    const ballRect = ball.getBoundingClientRect();
    const nyam = el('div', 'nyam', score % 3 === 0 ? 'NYAM!' : 'nyam');
    nyam.style.left = `${ballRect.left - fieldRect.left + ballRect.width / 2}px`;
    nyam.style.top = `${ballRect.top - fieldRect.top}px`;
    field.appendChild(nyam);
    setTimeout(() => nyam.remove(), 700);
    setTimeout(() => ball.remove(), 340);
    if (score === 12) {
      const burp = el('div', 'burp', 'BURRRP!');
      node.appendChild(burp);
      setTimeout(() => burp.remove(), 950);
      haptic([20, 30, 20]);
    }
    if (field.querySelectorAll('.cheese-ball:not(.eaten)').length === 0) {
      setTimeout(() => {
        if (score >= 12) toast('Legendary. 12 balls, one burp.');
        else toast('Plate empty. Cook more?');
      }, 500);
    }
  }

  node.querySelector('[data-cook]').addEventListener('click', cook);
  node.querySelector('[data-burp]').addEventListener('click', () => {
    const burp = el('div', 'burp', 'BURRRP!');
    node.appendChild(burp);
    setTimeout(() => burp.remove(), 950);
    haptic(30);
  });

  let observer = null;
  return {
    node,
    mount() {
      cook();
      observer = new ResizeObserver(() => {
        field.querySelectorAll('.cheese-ball').forEach(place);
      });
      observer.observe(field);
    },
    destroy() {
      if (observer) observer.disconnect();
    }
  };
}

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

function terminalApp(ctx) {
  const node = el('div', 'appview terminal');
  node.dataset.app = 'terminal';
  node.innerHTML = `
    <div class="term-log" data-log></div>
    <div class="term-chips">
      ${['HELP', 'DIR', 'TYPE README.TXT', 'THEME AURORA', 'OPEN SNAKE'].map((cmd) =>
        `<button class="term-chip" data-cmd="${cmd}">${cmd}</button>`).join('')}
    </div>
    <div class="term-input-row">
      <span class="prompt">dim@phone:~$</span>
      <input class="term-input" data-input autocomplete="off" autocapitalize="characters"
             autocorrect="off" spellcheck="false" aria-label="Terminal input">
    </div>
    <div class="term-keys">
      <button data-key="Tab">TAB</button>
      <button data-key="up">↑</button>
      <button data-key="down">↓</button>
      <button data-key="clear">CLS</button>
      <button data-key="dismiss">HIDE KEYBOARD</button>
    </div>`;

  const log = node.querySelector('[data-log]');
  const input = node.querySelector('[data-input]');
  const history = [];
  let historyIndex = -1;

  function print(text, className = '') {
    const line = el('div', `term-line ${className}`, escapeHTML(text).replace(/\n/g, '<br>'));
    log.appendChild(line);
    log.scrollTop = log.scrollHeight;
  }

  function run(raw) {
    const command = raw.trim();
    if (!command) return;
    print(command, 'cmd');
    history.push(command);
    historyIndex = history.length;
    const [name, ...args] = command.split(/\s+/);
    const arg = args.join(' ');

    switch (name.toUpperCase()) {
      case 'HELP':
      case '?':
        print('Commands: HELP DIR TYPE <file> DEL <file> THEME <name> APPEARANCE <mode>');
        print('          DATE TIME ABOUT CLS LOCK HOME OPEN <app> PC ECHO <text>');
        print('Try: THEME AURORA · OPEN SNAKE · TYPE README.TXT');
        break;
      case 'DIR':
      case 'LS':
        FILES.forEach((file) => print(`${file.name.padEnd(14)} ${file.size.padStart(8)}`));
        break;
      case 'TYPE':
      case 'CAT': {
        const file = FILES.find((item) => item.name.toLowerCase() === arg.toLowerCase());
        if (!file) print(`type: ${arg || 'missing file'} not found`, 'err');
        else if (file.body) print(file.body);
        else print(`${file.name} is binary (${file.size}).`);
        break;
      }
      case 'DEL': {
        const index = FILES.findIndex((item) => item.name.toLowerCase() === arg.toLowerCase());
        if (index < 0) print(`del: ${arg} not found`, 'err');
        else if (FILES[index].name === 'KERNEL.BIN') print('del: KERNEL.BIN is protected', 'err');
        else { print(`removed ${FILES[index].name}`); FILES.splice(index, 1); }
        break;
      }
      case 'THEME': {
        const wall = WALLPAPERS.find((item) => item.id === arg.toLowerCase())
          || WALLPAPERS.find((item) => item.name.toLowerCase() === arg.toLowerCase());
        if (!wall) print(`theme: choose ${WALLPAPERS.map((item) => item.id).join(', ')}`, 'err');
        else { ctx.setWallpaper(wall.id); print(`wallpaper -> ${wall.name}`); }
        break;
      }
      case 'APPEARANCE': {
        const mode = (arg || '').toLowerCase();
        if (!['dark', 'light', 'auto'].includes(mode)) print('appearance: dark | light | auto', 'err');
        else { ctx.setAppearance(mode); print(`appearance -> ${mode}`); }
        break;
      }
      case 'DATE':
        print(new Date().toString());
        break;
      case 'TIME':
        print(clockText());
        break;
      case 'ABOUT':
        print('DimOS 14 Mobile 14.0.1 (phone edition)');
        print('Shell: iOS 14 inspired. Kernel edition: i686 bare metal, 55.7 KB.');
        print('Display: 390 x 844 logical points, safe-area aware.');
        break;
      case 'CLS':
      case 'CLEAR':
        log.innerHTML = '';
        break;
      case 'LOCK':
        ctx.lock();
        break;
      case 'HOME':
        ctx.goHome();
        break;
      case 'OPEN': {
        const target = APP_BY_ID[arg.toLowerCase()];
        if (!target) print(`open: no app called ${arg}`, 'err');
        else { print(`opening ${target.name}...`); setTimeout(() => ctx.openApp(target.id), 180); }
        break;
      }
      case 'PC':
        print('opening the x86 edition in a new tab...');
        window.open('./pc.html', '_blank');
        break;
      case 'ECHO':
        print(arg);
        break;
      case 'NYAM':
        print('NYAM! The terminal ate a cheese ball.');
        break;
      default:
        print(`dim: command not found: ${name}`, 'err');
    }
  }

  print('DimOS 14 Mobile 14.0 (tty1)');
  print('Type HELP for a list of commands.', 'dim');

  input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      run(input.value);
      input.value = '';
      event.preventDefault();
    } else if (event.key === 'ArrowUp') {
      if (historyIndex > 0) {
        historyIndex -= 1;
        input.value = history[historyIndex];
      }
      event.preventDefault();
    } else if (event.key === 'ArrowDown') {
      if (historyIndex < history.length - 1) {
        historyIndex += 1;
        input.value = history[historyIndex];
      } else {
        historyIndex = history.length;
        input.value = '';
      }
      event.preventDefault();
    }
  });

  node.querySelectorAll('[data-cmd]').forEach((chip) => {
    chip.addEventListener('click', () => {
      run(chip.dataset.cmd);
      input.focus();
    });
  });

  node.querySelectorAll('[data-key]').forEach((button) => {
    button.addEventListener('click', () => {
      const key = button.dataset.key;
      if (key === 'clear') {
        log.innerHTML = '';
      } else if (key === 'dismiss') {
        input.blur();
      } else if (key === 'up' || key === 'down') {
        input.dispatchEvent(new KeyboardEvent('keydown', { key: key === 'up' ? 'ArrowUp' : 'ArrowDown', bubbles: true }));
      } else if (key === 'Tab') {
        input.value = 'HELP';
      }
      input.focus();
    });
  });

  log.addEventListener('click', () => input.focus({ preventScroll: true }));
  if (window.matchMedia('(pointer: fine)').matches) {
    setTimeout(() => input.focus({ preventScroll: true }), 350);
  }

  return { node };
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

function settingsApp(ctx) {
  const node = appShell(ctx, {
    app: 'settings',
    title: 'Settings',
    className: 'settings',
    content: `
      <div class="settings-head">
        <span class="avatar">D</span>
        <div>
          <div class="name">DimOS User</div>
          <div class="sub">DimOS Account, Core, Cloud</div>
        </div>
      </div>
      <div data-settings></div>
      <div class="app-version">DimOS 14.0.1 · phone edition</div>`
  });

  node.classList.toggle('light', ctx.effectiveAppearance() === 'light');
  const host = node.querySelector('[data-settings]');

  function render() {
    const installed = ctx.isInstalled();
    host.innerHTML = `
      <div class="group-title">Network</div>
      <div class="group">
        ${switchRow({ label: 'Airplane Mode', iconName: 'airplane', tint: '#ff9500', on: settings.airplane, onTap: 'airplane' })}
        ${switchRow({ label: 'Wi-Fi', iconName: 'wifi', tint: '#0a84ff', on: settings.wifi, value: settings.wifi ? settings.wifiName : 'Off', onTap: 'wifi' })}
        ${switchRow({ label: 'Bluetooth', iconName: 'bluetooth', tint: '#0a84ff', on: settings.bluetooth, value: 'On', onTap: 'bluetooth' })}
        ${switchRow({ label: 'Cellular', iconName: 'cellular', tint: '#30d158', on: settings.cellular, value: '5G', onTap: 'cellular' })}
      </div>

      <div class="group-title">Appearance</div>
      <div class="group">
        <div class="row" style="flex-wrap:wrap">
          <span class="row-label">Appearance</span>
          <div class="segmented" style="width:170px;flex:none" data-appearance>
            ${['dark', 'light', 'auto'].map((mode) =>
              `<button data-mode="${mode}" class="${settings.appearance === mode ? 'active' : ''}">${mode[0].toUpperCase() + mode.slice(1)}</button>`).join('')}
          </div>
        </div>
        <button class="row" data-wallpaper>
          <span class="row-label">Wallpaper</span>
          <span class="row-value">${WALLPAPERS.find((item) => item.id === settings.wallpaper).name}</span>
          ${chevron()}
        </button>
        <div class="row">
          <span class="row-label">Brightness</span>
          <input type="range" min="20" max="100" value="${Math.round(settings.brightness * 100)}" data-brightness
                 style="flex:1;max-width:170px;accent-color:#0a84ff">
        </div>
        <div class="row">
          <span class="row-label">24-Hour Time</span>
          <button class="switch ${settings.hour24 === true ? 'on' : ''}" data-hour24 role="switch"></button>
        </div>
      </div>

      <div class="group-title">Sounds &amp; Haptics</div>
      <div class="group">
        <div class="row">
          <span class="row-label">Haptics</span>
          <button class="switch ${settings.haptics ? 'on' : ''}" data-haptics role="switch"></button>
        </div>
        <div class="row">
          <span class="row-label">Key Clicks</span>
          <button class="switch ${settings.clicks ? 'on' : ''}" data-clicks role="switch"></button>
        </div>
        <div class="row">
          <span class="row-label">Volume</span>
          <input type="range" min="0" max="100" value="${Math.round(settings.volume * 100)}" data-volume
                 style="flex:1;max-width:170px;accent-color:#0a84ff">
        </div>
      </div>

      <div class="group-title">General</div>
      <div class="group">
        <button class="row" data-about>
          <span class="row-label">About</span>
          <span class="row-value">DimOS 14.0.1</span>
          ${chevron()}
        </button>
        <div class="row">
          <span class="row-label">Storage</span>
          <span class="row-value">1.4 GB of 64 GB</span>
        </div>
        <button class="row" data-pc>
          <span class="row-label">DimOS Core (x86)</span>
          <span class="row-value">PC edition</span>
          ${chevron()}
        </button>
        ${installed
          ? `<div class="row"><span class="row-label">Installed to Home Screen</span><span class="row-value" style="color:#30d158">Yes</span></div>`
          : `<button class="row" data-install><span class="row-label">Add to Home Screen</span><span class="row-value" style="color:#0a84ff">Install</span></button>`}
      </div>

      <div class="group-title">Power</div>
      <div class="group">
        <button class="row" data-lock><span class="row-label" style="color:#ff453a">Lock</span></button>
        <button class="row" data-restart><span class="row-label" style="color:#ff453a">Restart</span></button>
      </div>`;

    host.querySelectorAll('[data-row]').forEach((row) => {
      row.addEventListener('click', () => {
        const key = row.querySelector('.switch');
        const name = row.querySelector('.row-label').textContent.toLowerCase();
        const map = { 'airplane mode': 'airplane', 'wi-fi': 'wifi', 'bluetooth': 'bluetooth', cellular: 'cellular' };
        const setting = map[name];
        if (!setting) return;
        settings[setting] = !settings[setting];
        if (setting === 'airplane' && settings.airplane) {
          settings.wifi = false;
          settings.bluetooth = false;
          settings.cellular = false;
        }
        saveSettings();
        key.classList.toggle('on', settings[setting]);
        key.setAttribute('aria-checked', String(settings[setting]));
        ctx.refreshNetwork();
        render();
        haptic(8);
      });
    });

    host.querySelectorAll('[data-appearance] button').forEach((button) => {
      button.addEventListener('click', () => {
        ctx.setAppearance(button.dataset.mode);
        render();
      });
    });

    host.querySelector('[data-wallpaper]').addEventListener('click', () => {
      ctx.showSheet({
        title: 'Wallpaper',
        body: `<div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:4px 0 8px">
          ${WALLPAPERS.map((wall) => `
            <button data-pick="${wall.id}" style="border-radius:16px;overflow:hidden;padding:0;position:relative;aspect-ratio:9/16;background:${wall.css};background-size:cover;box-shadow:inset 0 0 0 0.5px rgba(255,255,255,.2)">
              ${wall.id === settings.wallpaper ? '<span style="position:absolute;right:8px;bottom:8px;width:24px;height:24px;border-radius:50%;background:#0a84ff;display:grid;place-items:center;color:#fff">✓</span>' : ''}
              <span style="position:absolute;left:8px;bottom:8px;font-size:11px;font-weight:600;color:#fff;text-shadow:0 1px 4px rgba(0,0,0,.6)">${wall.name}</span>
            </button>`).join('')}
        </div>`
      });
      setTimeout(() => {
        document.querySelectorAll('[data-pick]').forEach((button) => {
          button.addEventListener('click', () => {
            ctx.setWallpaper(button.dataset.pick);
            ctx.hideSheet();
            render();
          });
        });
      }, 30);
    });

    host.querySelector('[data-about]').addEventListener('click', () => {
      const info = ctx.deviceInfo();
      ctx.showSheet({
        title: 'About',
        body: `<div style="font-size:14px;line-height:1.6">
          ${Object.entries(info).map(([key, value]) =>
            `<div style="display:flex;justify-content:space-between;gap:16px;padding:7px 0;border-bottom:0.5px solid rgba(255,255,255,.1)">
               <span style="opacity:.6">${key}</span><span style="text-align:right">${value}</span>
             </div>`).join('')}
        </div>`
      });
    });

    host.querySelector('[data-pc]').addEventListener('click', () => window.open('./pc.html', '_blank'));
    const installRow = host.querySelector('[data-install]');
    if (installRow) installRow.addEventListener('click', () => ctx.promptInstall());
    host.querySelector('[data-lock]').addEventListener('click', () => ctx.lock());
    host.querySelector('[data-restart]').addEventListener('click', () => {
      ctx.showSheet({
        title: 'Restart DimOS?',
        body: `<p style="font-size:14px;line-height:1.5;opacity:.7;margin:0 0 6px">The shell will reload. Your settings stay on this device.</p>
               <button class="sheet-action" data-yes style="background:#ff453a;color:#fff">Restart</button>
               <button class="sheet-action" data-no>Cancel</button>`
      });
      setTimeout(() => {
        const yes = document.querySelector('[data-yes]');
        const no = document.querySelector('[data-no]');
        if (yes) yes.addEventListener('click', () => location.reload());
        if (no) no.addEventListener('click', () => ctx.hideSheet());
      }, 30);
    });

    host.querySelector('[data-haptics]').addEventListener('click', (event) => {
      settings.haptics = !settings.haptics;
      saveSettings();
      event.currentTarget.classList.toggle('on', settings.haptics);
      haptic(10);
    });

    host.querySelector('[data-clicks]').addEventListener('click', (event) => {
      settings.clicks = !settings.clicks;
      saveSettings();
      event.currentTarget.classList.toggle('on', settings.clicks);
      click();
    });

    host.querySelector('[data-hour24]').addEventListener('click', (event) => {
      settings.hour24 = settings.hour24 === null ? !/^en-US/i.test(navigator.language) : !settings.hour24;
      saveSettings();
      event.currentTarget.classList.toggle('on', settings.hour24);
      ctx.refreshClock();
    });

    host.querySelector('[data-brightness]').addEventListener('input', (event) => {
      settings.brightness = Number(event.target.value) / 100;
      saveSettings();
      ctx.refreshBrightness();
    });
    host.querySelector('[data-volume]').addEventListener('input', (event) => {
      settings.volume = Number(event.target.value) / 100;
      saveSettings();
    });
  }

  render();
  return { node };
}

/* ------------------------------------------------------------------ */
/* Weather                                                             */
/* ------------------------------------------------------------------ */

function weatherApp(ctx) {
  const node = el('div', 'appview weather-app');
  node.dataset.app = 'weather';

  const dayBars = WEATHER.daily.map((day) => {
    const left = ((day.low - 8) / 18) * 100;
    const width = ((day.high - day.low) / 18) * 100;
    return `<div class="day">
      <span class="d-name">${day.day}</span>
      ${icon(day.icon)}
      <span class="d-low">${day.low}°</span>
      <span class="d-bar"><i style="width:${width}%;margin-left:${left}%"></i></span>
      <span class="d-high">${day.high}°</span>
    </div>`;
  }).join('');

  node.innerHTML = `
    <div class="wa-top">
      <div class="wa-city">${WEATHER.city}</div>
      <div class="wa-temp">${WEATHER.temp}°</div>
      <div class="wa-cond">${WEATHER.condition}</div>
      <div class="wa-range">H:${WEATHER.high}°  L:${WEATHER.low}°</div>
      <div class="wa-sun">${sunArt()}</div>
    </div>
    <div class="weather-card">
      <div class="card-title">Hourly forecast</div>
      <div class="hours">
        ${WEATHER.hourly.map((hour) => `
          <div class="hour"><span>${hour.h}</span>${icon(hour.icon)}<b>${hour.t}°</b></div>`).join('')}
      </div>
    </div>
    <div class="weather-card">
      <div class="card-title">8-day forecast</div>
      <div class="days">${dayBars}</div>
    </div>
    <div class="weather-stats">
      <div class="weather-stat"><div class="s-label">UV Index</div><div class="s-value">${WEATHER.uv}</div><div class="s-note">Moderate</div></div>
      <div class="weather-stat"><div class="s-label">Wind</div><div class="s-value">${WEATHER.wind} km/h</div><div class="s-note">From the west</div></div>
      <div class="weather-stat"><div class="s-label">Humidity</div><div class="s-value">${WEATHER.humidity}%</div><div class="s-note">Feels like ${WEATHER.feels}°</div></div>
      <div class="weather-stat"><div class="s-label">Rain</div><div class="s-value">${WEATHER.rain}%</div><div class="s-note">Next 24 hours</div></div>
    </div>
    <div style="text-align:center;font-size:11px;opacity:.5;margin-top:16px">Demo forecast · Frankfurt</div>`;

  return { node };
}

/* ------------------------------------------------------------------ */
/* Core (bridge to the bare-metal x86 edition)                         */
/* ------------------------------------------------------------------ */

function coreApp(ctx) {
  const node = el('div', 'appview core-app');
  node.dataset.app = 'core';
  node.innerHTML = `
    <button class="nav-back core-back" data-back aria-label="Back">${icon('chevron-left')}</button>
    <div class="core-scroll">
    <div class="core-mark"><svg viewBox="0 0 24 24" aria-hidden="true">${APP_BY_ID.core.glyph}</svg></div>
    <h2>DimOS Core</h2>
    <div class="core-tag">The same operating system,<br>written for bare metal.</div>

    <div class="fact-grid">
      <div class="fact"><span>Kernel</span><b>i686, 55.7 KB</b></div>
      <div class="fact"><span>Graphics</span><b>VBE linear</b></div>
      <div class="fact"><span>Canvas</span><b>844 × 390</b></div>
      <div class="fact"><span>Libraries</span><b>None</b></div>
      <div class="fact"><span>Boot</span><b>512-byte sector</b></div>
      <div class="fact"><span>Apps</span><b>9 native</b></div>
    </div>

    <p class="core-note">
      This phone edition is the portable face of DimOS. The original build boots
      straight from a floppy or disk image into protected mode, draws its own
      iOS 14 shell into a VBE framebuffer and runs the same apps without an OS
      underneath. It runs on real PCs, in QEMU and in the browser through v86.
    </p>

    <div class="core-actions">
      <a class="primary" href="./pc.html" target="_blank" rel="noopener">${icon('power')} Boot the x86 edition</a>
      <button data-about>${icon('info')} Machine facts</button>
      <button data-install>${icon('download')} Install this phone edition</button>
    </div>

    <p class="core-note" style="font-size:11.5px;opacity:.45;text-align:center;margin-top:18px">
      The x86 edition loads v86 from a CDN and needs a network for its first boot.
    </p>
    </div>`;

  node.querySelector('[data-about]').addEventListener('click', () => {
    const info = ctx.deviceInfo();
    ctx.showSheet({
      title: 'Machine facts',
      body: `<div style="font-size:14px;line-height:1.6">
        ${Object.entries(info).map(([key, value]) =>
          `<div style="display:flex;justify-content:space-between;gap:16px;padding:7px 0;border-bottom:0.5px solid rgba(255,255,255,.1)">
             <span style="opacity:.6">${key}</span><span style="text-align:right">${value}</span>
           </div>`).join('')}
      </div>`
    });
  });

  node.querySelector('[data-install]').addEventListener('click', () => ctx.promptInstall());
  node.querySelector('[data-back]').addEventListener('click', () => ctx.goHome());

  return { node };
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

export const APP_VIEWS = {
  calculator: calculatorApp,
  music: musicApp,
  files: filesApp,
  paint: paintApp,
  snake: snakeApp,
  mines: minesApp,
  cheesy: cheesyApp,
  terminal: terminalApp,
  settings: settingsApp,
  weather: weatherApp,
  core: coreApp
};

export function buildApp(ctx, id) {
  const builder = APP_VIEWS[id];
  if (!builder) return null;
  return builder(ctx);
}

export function appMeta(id) {
  return APP_BY_ID[id];
}

export const APP_LIST = APPS;
