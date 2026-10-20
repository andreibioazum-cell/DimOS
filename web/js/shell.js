/*
 * DimOS 14 Mobile -- the shell.
 *
 * Lock screen, home screen with widgets, pages and App Library, Control
 * Centre, Notification Centre, the app window, the switcher and the
 * gestures that connect them.
 */

import {
  APPS, APP_BY_ID, DOCK_APPS, HOME_PAGES, WALLPAPERS, NOTIFICATIONS,
  WEATHER
} from './data.js';
import {
  $, $$, el, icon, appIconHTML, settings, saveSettings, toast, haptic,
  click, clockText, dateText, shortDate, durationText, WEEKDAYS_SHORT,
  MONTHS, batteryRing, sunArt
} from './ui.js';
import { player, buildApp } from './apps.js';

const WEEKDAY_LONG = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];

export function createShell() {
  /* ---------------------------------------------------------------- */
  /* Elements and state                                               */
  /* ---------------------------------------------------------------- */

  const device = $('#device');
  const stage = $('#stage');
  const lockScreen = $('#lock');
  const lockTime = $('#lock-time');
  const lockDate = $('#lock-date');
  const lockNotifications = $('#lock-notifications');
  const home = $('#home');
  const pagesNode = $('#pages');
  const dotsNode = $('#dots');
  const dockNode = $('#dock');
  const statusbar = $('#statusbar');
  const homebar = $('#homebar');
  const sbTime = $('#sb-time');
  const sbNetwork = $('#sb-network');
  const sbSignal = $('#sb-signal');
  const sbWifi = $('.sb-wifi');
  const sbBatteryFill = $('#sb-battery-fill');
  const appHost = $('#app-host');
  const appWindow = $('#app-window');
  const ccNode = $('#cc');
  const ncNode = $('#nc');
  const switcherNode = $('#switcher');
  const sheetLayer = $('#sheet-layer');
  const contextLayer = $('#context-layer');
  const boot = $('#boot');

  const state = {
    page: 0,
    locked: true,
    appId: null,
    appInstance: null,
    recents: [],
    ccOpen: false,
    ncOpen: false,
    switcherOpen: false,
    sheetOpen: false,
    contextOpen: false,
    editing: false,
    notifications: NOTIFICATIONS.map((item) => ({ ...item })),
    timer: null
  };

  let installPrompt = null;
  let pageWidth = 390;
  let suppressClickUntil = 0;
  let drag = null;
  let closing = false;

  /* Brightness veil: iOS dims the display, it does not fade the UI. */
  const dim = el('div', 'dim-layer');
  device.appendChild(dim);

  /* ---------------------------------------------------------------- */
  /* Context handed to apps                                           */
  /* ---------------------------------------------------------------- */

  const ctx = {
    settings,
    saveSettings,
    toast,
    haptic,
    openApp,
    goHome,
    lock,
    showSheet,
    hideSheet,
    setWallpaper,
    setAppearance,
    refreshClock,
    refreshBrightness: applyBrightness,
    refreshNetwork,
    isInstalled,
    promptInstall,
    deviceInfo,
    notify,
    effectiveAppearance,
    openControlCentre,
    closeControlCentre,
    openNotificationCentre,
    closeNotificationCentre,
    openSwitcher,
    closeSwitcher,
    goToPage,
    closeContextMenu,
    stopEditing
  };

  /* ---------------------------------------------------------------- */
  /* Appearance                                                       */
  /* ---------------------------------------------------------------- */

  function effectiveAppearance() {
    if (settings.appearance === 'auto') {
      const hour = new Date().getHours();
      return hour >= 7 && hour < 19 ? 'light' : 'dark';
    }
    return settings.appearance;
  }

  function applyAppearance() {
    const light = effectiveAppearance() === 'light';
    device.classList.toggle('light-ui', light);
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = light ? '#f2f2f7' : '#000000';
    applyChromeTheme();
    document.documentElement.style.colorScheme = light ? 'light' : 'dark';
  }

  function applyWallpaper() {
    const wallpaper = WALLPAPERS.find((item) => item.id === settings.wallpaper) || WALLPAPERS[0];
    $('#wallpaper').style.background = wallpaper.css;
    $('#wallpaper').style.backgroundSize = 'cover';
  }

  function applyBrightness() {
    const dark = Math.max(0, Math.min(0.45, (1 - settings.brightness) * 0.7));
    dim.style.opacity = String(dark);
  }

  /* ---------------------------------------------------------------- */
  /* Chrome colours                                                   */
  /* ---------------------------------------------------------------- */

  function applyChromeTheme() {
    const light = effectiveAppearance() === 'light';
    let darkGlyphs = false;

    if (state.appId) {
      if (state.appId === 'cheesy') darkGlyphs = true;
      else if (state.appId === 'settings') darkGlyphs = light;
      else darkGlyphs = false;
    } else if (state.locked) {
      darkGlyphs = false;
    } else if (state.sheetOpen) {
      darkGlyphs = light;
    } else if (state.ccOpen || state.ncOpen) {
      darkGlyphs = light;
    }

    statusbar.classList.toggle('dark', darkGlyphs);
    statusbar.classList.toggle('light', !darkGlyphs);
    homebar.classList.toggle('dark', darkGlyphs);
    device.classList.toggle('app-open', !!state.appId);
  }

  /* ---------------------------------------------------------------- */
  /* Clock, battery, network                                          */
  /* ---------------------------------------------------------------- */

  function refreshClock() {
    const now = new Date();
    const time = clockText(now);
    if (lockTime) lockTime.textContent = time;
    if (lockDate) lockDate.textContent = dateText(now);
    if (sbTime) sbTime.textContent = time;

    const ncTime = $('#nc-time');
    if (ncTime) ncTime.textContent = time;
    const ncDate = $('#nc-date');
    if (ncDate) ncDate.textContent = dateText(now);

    const calDay = $('.cal-day');
    if (calDay) calDay.textContent = String(now.getDate());
    const calWeek = $('.cal-week');
    if (calWeek) calWeek.textContent = `${MONTHS[now.getMonth()]} ${now.getFullYear()}`;
    const calWeekday = $('.calendar-widget .w-app');
    if (calWeekday) calWeekday.textContent = WEEKDAY_LONG[now.getDay()].toUpperCase();

    updateAnalog(now);

    const musicProgress = $('.music-widget .mw-progress i');
    if (musicProgress) {
      musicProgress.style.width = `${(player.progress / player.track.length) * 100}%`;
    }
  }

  function updateAnalog(now) {
    const analog = $('.analog');
    if (!analog) return;
    const seconds = now.getSeconds();
    const minutes = now.getMinutes() + seconds / 60;
    const hours = (now.getHours() % 12) + minutes / 60;
    const set = (selector, degrees) => {
      const hand = analog.querySelector(selector);
      if (hand) hand.style.transform = `rotate(${degrees}deg)`;
    };
    set('.h', hours * 30);
    set('.m', minutes * 6);
    set('.s', seconds * 6);
  }

  async function initBattery() {
    const render = (level) => {
      if (sbBatteryFill) sbBatteryFill.style.width = `${Math.max(4, Math.round(level * 100))}%`;
      const widget = $('.battery-widget .batt-ring span');
      if (widget) widget.textContent = `${Math.round(level * 100)}%`;
      const ring = $('.battery-widget .batt-ring');
      if (ring) {
        const levelCircle = ring.querySelectorAll('circle')[1];
        if (levelCircle) {
          const radius = 44;
          const circumference = 2 * Math.PI * radius;
          levelCircle.setAttribute('stroke-dashoffset', String(circumference * (1 - level)));
        }
      }
    };
    render(0.8);
    try {
      if (navigator.getBattery) {
        const battery = await navigator.getBattery();
        const update = () => render(battery.level);
        update();
        battery.addEventListener('levelchange', update);
      }
    } catch (_) { /* no battery API */ }
  }

  function refreshNetwork() {
    if (!sbNetwork) return;
    sbNetwork.textContent = settings.airplane ? '—' : (settings.cellular ? '5G' : 'Wi-Fi');
    sbSignal.querySelectorAll('i').forEach((bar, index) => {
      bar.classList.toggle('dim', settings.airplane || !settings.cellular || index > 2);
    });
    if (sbWifi) sbWifi.style.opacity = settings.wifi && !settings.airplane ? '1' : '0.35';
  }

  /* ---------------------------------------------------------------- */
  /* Widgets                                                          */
  /* ---------------------------------------------------------------- */

  function weatherWidget() {
    return `
      <button class="widget full weather-widget" data-open="weather">
        <div class="w-app">${icon('cloud-sun')} Weather · ${WEATHER.city}</div>
        <div class="w-main">
          <div>
            <div class="w-temp">${WEATHER.temp}°</div>
            <div class="w-cond">${WEATHER.condition}</div>
            <div class="w-cond" style="opacity:.7">H:${WEATHER.high}°  L:${WEATHER.low}°</div>
          </div>
          <div class="w-sun">${sunArt()}</div>
        </div>
        <div class="w-hours">
          ${WEATHER.hourly.slice(0, 5).map((hour) => `
            <div class="w-hour"><span>${hour.h}</span>${icon(hour.icon)}<b>${hour.t}°</b></div>`).join('')}
        </div>
      </button>`;
  }

  function clockWidget() {
    return `
      <button class="widget half clock-widget" data-open="none">
        <div class="w-app">${icon('timer')} Clock</div>
        <div class="analog"><i class="h"></i><i class="m"></i><i class="s"></i></div>
        <div class="w-city-label">Cupertino</div>
      </button>`;
  }

  function calendarWidget() {
    const now = new Date();
    return `
      <button class="widget half calendar-widget" data-open="none">
        <div class="w-app">${WEEKDAY_LONG[now.getDay()].toUpperCase()}</div>
        <div class="cal-day">${now.getDate()}</div>
        <div class="cal-week">${MONTHS[now.getMonth()]} ${now.getFullYear()}</div>
        <div class="cal-events">
          <div class="cal-event"><i></i>Boot the x86 kernel</div>
          <div class="cal-event"><i style="background:#0a84ff"></i>NYAM! 12 balls</div>
        </div>
      </button>`;
  }

  function musicWidget() {
    return `
      <button class="widget full music-widget" data-open="music">
        <div class="w-app">${icon('music')} Music</div>
        <div class="mw-body">
          <div class="mw-art" style="background:${player.track.art}"></div>
          <div class="mw-meta">
            <div class="mw-title">${player.track.title}</div>
            <div class="mw-artist">${player.track.artist}</div>
            <div class="mw-progress"><i style="width:${(player.progress / player.track.length) * 100}%"></i></div>
            <div class="mw-controls">
              <span>${icon('prev')}</span>
              <span>${icon('play')}</span>
              <span>${icon('next')}</span>
            </div>
          </div>
        </div>
      </button>`;
  }

  function batteryWidget() {
    return `
      <button class="widget half battery-widget" data-open="settings">
        <div class="w-app">${icon('battery')} Battery</div>
        <div class="batt-ring">${batteryRing(0.8)}<span>80%</span></div>
        <div class="batt-sub">DimOS Phone · 5G</div>
      </button>`;
  }

  const WIDGETS = {
    'weather-widget': weatherWidget,
    'clock-widget': clockWidget,
    'calendar-widget': calendarWidget,
    'music-widget': musicWidget,
    'battery-widget': batteryWidget
  };

  /* ---------------------------------------------------------------- */
  /* Home screen                                                      */
  /* ---------------------------------------------------------------- */

  function appCell(id, extra = '') {
    const app = APP_BY_ID[id];
    if (!app) return '';
    return `
      <button class="app-cell ${extra}" data-open="${id}" aria-label="${app.name}">
        ${appIconHTML(app)}
        <span class="app-label">${app.name}</span>
      </button>`;
  }

  function renderPage(items) {
    let markup = '';
    const widgetRow = [];
    const appCells = [];

    const flushWidgets = () => {
      if (!widgetRow.length) return;
      markup += `<div class="widget-row">${widgetRow.join('')}</div>`;
      widgetRow.length = 0;
    };
    const flushApps = () => {
      if (!appCells.length) return;
      markup += `<div class="app-grid">${appCells.join('')}</div>`;
      appCells.length = 0;
    };

    items.forEach((item) => {
      if (item.type === 'app') {
        flushWidgets();
        appCells.push(appCell(item.id));
        return;
      }
      const builder = WIDGETS[item.type];
      if (!builder) return;
      if (item.span === 4) {
        flushApps();
        flushWidgets();
        markup += `<div class="widget-row">${builder()}</div>`;
      } else {
        flushApps();
        widgetRow.push(builder());
      }
    });

    flushWidgets();
    flushApps();
    return markup;
  }

  function libraryPage() {
    return `
      <div class="page library" data-page="2">
        <h2 class="library-title">App Library</h2>
        <label class="library-search">
          ${icon('search')}
          <input id="library-search" placeholder="App Library" autocomplete="off" spellcheck="false">
        </label>
        <div class="library-grid" id="library-grid"></div>
      </div>`;
  }

  function renderLibrary(filter = '') {
    const grid = $('#library-grid');
    if (!grid) return;
    const query = filter.trim().toLowerCase();
    const categories = new Map();
    APPS.filter((app) => !query || app.name.toLowerCase().includes(query))
      .forEach((app) => {
        if (!categories.has(app.category)) categories.set(app.category, []);
        categories.get(app.category).push(app);
      });

    if (!categories.size) {
      grid.innerHTML = `<div class="library-empty">No apps match “${filter}”.</div>`;
      return;
    }

    grid.innerHTML = Array.from(categories.entries()).map(([name, apps]) => `
        <div class="library-card">
          <h4>${name}</h4>
          <div class="library-icons">
            ${apps.slice(0, 4).map((app) => `
              <button class="app-cell" data-open="${app.id}" aria-label="${app.name}">
                ${appIconHTML(app)}
              </button>`).join('')}
          </div>
        </div>`).join('');
  }

  function renderHome() {
    const pages = HOME_PAGES.map((items, index) => `
      <div class="page" data-page="${index}">${renderPage(items)}</div>`).join('');
    pagesNode.innerHTML = pages + libraryPage();
    renderLibrary();
    dockNode.innerHTML = DOCK_APPS.map((id) => appCell(id)).join('');
    renderDots();
    pagesNode.style.transform = 'translateX(0)';
    refreshClock();
    initBattery();
  }

  function renderDots() {
    dotsNode.innerHTML = `
      <button data-page="0" class="${state.page === 0 ? 'active' : ''}" aria-label="Page 1"></button>
      <button data-page="1" class="${state.page === 1 ? 'active' : ''}" aria-label="Page 2"></button>
      <button data-page="2" class="${state.page === 2 ? 'active' : ''}" aria-label="App Library" data-library>
        <svg viewBox="0 0 24 24" width="8" height="8" aria-hidden="true">
          <rect x="3" y="3" width="7" height="7" rx="1.6" fill="currentColor"/>
          <rect x="14" y="3" width="7" height="7" rx="1.6" fill="currentColor"/>
          <rect x="3" y="14" width="7" height="7" rx="1.6" fill="currentColor"/>
          <rect x="14" y="14" width="7" height="7" rx="1.6" fill="currentColor"/>
        </svg>
      </button>`;
  }

  /* ---------------------------------------------------------------- */
  /* Page paging                                                      */
  /* ---------------------------------------------------------------- */

  function goToPage(index, animate = true) {
    state.page = Math.max(0, Math.min(2, index));
    pagesNode.classList.toggle('dragging', !animate);
    pagesNode.style.transform = `translateX(-${state.page * 100}%)`;
    if (!animate) {
      requestAnimationFrame(() => pagesNode.classList.remove('dragging'));
    }
    renderDots();
    haptic(4);
  }

  function measure() {
    pageWidth = pagesNode.getBoundingClientRect().width || 390;
  }

  /* ---------------------------------------------------------------- */
  /* Lock screen                                                      */
  /* ---------------------------------------------------------------- */

  function renderLockNotifications() {
    if (!lockNotifications) return;
    lockNotifications.innerHTML = state.notifications.slice(0, 3).map((notification) => {
      const app = APP_BY_ID[notification.app] || APP_BY_ID.core;
      return `
        <button class="notification" data-napp="${notification.app}">
          <span class="n-icon" style="background:${app.tint}">
            <svg viewBox="0 0 24 24" aria-hidden="true">${app.glyph}</svg>
          </span>
          <span class="n-body">
            <span class="n-head"><span>${app.name}</span><span>${notification.time}</span></span>
            <span class="n-title">${notification.title}</span>
            <span class="n-text">${notification.body}</span>
          </span>
        </button>`;
    }).join('');
  }

  function unlock(fromLock = true) {
    if (!state.locked) return;
    state.locked = false;
    device.classList.remove('locked');
    haptic(12);
    if (fromLock) {
      lockScreen.classList.add('unlocking');
    }
    applyChromeTheme();
    setTimeout(() => {
      lockScreen.hidden = true;
      lockScreen.classList.remove('unlocking');
      lockScreen.style.transform = '';
      lockScreen.style.opacity = '';
    }, fromLock ? 480 : 0);
    refreshClock();
  }

  function lock() {
    hideSheet();
    closeControlCentre();
    closeNotificationCentre();
    closeSwitcher();
    if (state.appId) goHome(true);
    state.locked = true;
    device.classList.add('locked');
    lockScreen.hidden = false;
    lockScreen.style.display = '';
    lockScreen.classList.add('locking');
    applyChromeTheme();
    haptic(12);
    setTimeout(() => lockScreen.classList.remove('locking'), 520);
    renderLockNotifications();
    refreshClock();
  }

  /* ---------------------------------------------------------------- */
  /* App window                                                       */
  /* ---------------------------------------------------------------- */

  function openApp(id, origin) {
    if (Date.now() < suppressClickUntil) return;
    const app = APP_BY_ID[id];
    if (!app) return;
    if (state.ccOpen) closeControlCentre();
    if (state.ncOpen) closeNotificationCentre();
    if (state.sheetOpen) hideSheet();
    if (state.switcherOpen) closeSwitcher(true);

    if (state.appId === id && state.appInstance) return;

    if (state.appInstance) {
      state.appInstance.destroy?.();
      state.appInstance = null;
      appWindow.innerHTML = '';
    }

    const instance = buildApp(ctx, id);
    if (!instance) {
      toast(`${app.name} is not available`);
      return;
    }

    state.appId = id;
    state.appInstance = instance;
    appWindow.innerHTML = '';
    appWindow.appendChild(instance.node);
    appWindow.style.background = app.status === 'light' ? '#f2f2f7' : '#000';
    appHost.setAttribute('aria-hidden', 'false');
    appHost.classList.remove('closing');
    appHost.classList.add('open');

    if (origin) {
      const rect = origin.getBoundingClientRect();
      const deviceRect = device.getBoundingClientRect();
      const x = ((rect.left + rect.width / 2 - deviceRect.left) / deviceRect.width) * 100;
      const y = ((rect.top + rect.height / 2 - deviceRect.top) / deviceRect.height) * 100;
      appWindow.style.transformOrigin = `${x}% ${y}%`;
    } else {
      appWindow.style.transformOrigin = 'center center';
    }

    home.classList.add('pushed');
    state.recents = [id, ...state.recents.filter((item) => item !== id)].slice(0, 8);
    applyChromeTheme();
    haptic(8);

    requestAnimationFrame(() => {
      instance.mount?.();
    });
  }

  function goHome(instant = false) {
    hideSheet();
    closeControlCentre();
    closeNotificationCentre();
    if (state.switcherOpen) {
      closeSwitcher();
      return;
    }
    if (!state.appId) {
      if (state.locked) return;
      if (state.page !== 0) goToPage(0);
      return;
    }
    if (closing) return;
    closing = true;
    appHost.classList.add('closing');
    appHost.classList.remove('open');
    home.classList.remove('pushed');
    const finish = () => {
      state.appInstance?.destroy?.();
      state.appInstance = null;
      state.appId = null;
      appWindow.innerHTML = '';
      appHost.classList.remove('closing');
      appHost.setAttribute('aria-hidden', 'true');
      closing = false;
      applyChromeTheme();
    };
    if (instant) {
      finish();
    } else {
      setTimeout(finish, 330);
      haptic(6);
    }
  }

  /* ---------------------------------------------------------------- */
  /* Control Centre                                                   */
  /* ---------------------------------------------------------------- */

  function toggleConnectivity(name) {
    settings[name] = !settings[name];
    if (name === 'airplane' && settings.airplane) {
      settings.wifi = false;
      settings.bluetooth = false;
      settings.cellular = false;
    }
    if (name !== 'airplane' && settings[name] && settings.airplane) {
      settings.airplane = false;
    }
    saveSettings();
    refreshNetwork();
    renderControlCentre();
    haptic(9);
  }

  function utilitySheet(kind) {
    if (kind === 'calculator') {
      closeControlCentre();
      openApp('calculator');
      return;
    }
    if (kind === 'flashlight') {
      const on = device.classList.toggle('torch');
      toast(on ? 'Flashlight on' : 'Flashlight off');
      renderControlCentre();
      haptic(10);
      return;
    }
    if (kind === 'camera') {
      toast('The camera belongs to the hardware, not this build');
      return;
    }
    if (kind === 'timer') {
      showSheet({
        title: 'Timer',
        body: `<div style="display:flex;gap:8px;margin-bottom:6px">
          ${[10, 60, 300].map((seconds) => `
            <button class="sheet-action" data-timer="${seconds}" style="background:rgba(118,118,128,.22)">
              ${seconds < 60 ? `${seconds} sec` : `${seconds / 60} min`}
            </button>`).join('')}
        </div>
        <p style="font-size:13px;opacity:.6;margin:6px 0 0">DimOS will show a notification when the timer ends.</p>`
      });
      setTimeout(() => {
        $$('[data-timer]').forEach((button) => {
          button.addEventListener('click', () => {
            const seconds = Number(button.dataset.timer);
            hideSheet();
            toast(`Timer set for ${seconds < 60 ? `${seconds} seconds` : `${seconds / 60} minutes`}`);
            setTimeout(() => {
              notify('core', 'Timer', `Your ${seconds < 60 ? `${seconds}-second` : `${seconds / 60}-minute`} timer is done.`);
              toast('Timer done');
            }, seconds * 1000);
          });
        });
      }, 20);
    }
  }

  function ccHTML() {
    const track = player.track;
    const torch = device.classList.contains('torch');
    return `
      <div class="cc-grid">
        <div class="tile conn">
          <button class="conn-toggle ${settings.airplane ? 'on orange' : ''}" data-conn="airplane" aria-label="Airplane mode">${icon('airplane')}</button>
          <button class="conn-toggle ${settings.cellular && !settings.airplane ? 'on green' : ''}" data-conn="cellular" aria-label="Cellular">${icon('cellular')}</button>
          <button class="conn-toggle ${settings.wifi && !settings.airplane ? 'on' : ''}" data-conn="wifi" aria-label="Wi-Fi">${icon('wifi')}</button>
          <button class="conn-toggle ${settings.bluetooth && !settings.airplane ? 'on' : ''}" data-conn="bluetooth" aria-label="Bluetooth">${icon('bluetooth')}</button>
        </div>

        <div class="tile music-tile">
          <div class="m-head">Now Playing</div>
          <div class="m-row">
            <div class="m-art" style="background:${track.art}"></div>
            <div class="m-meta">
              <div class="m-title">${track.title}</div>
              <div class="m-artist">${track.artist}</div>
            </div>
          </div>
          <div class="m-bar"><i style="width:${(player.progress / track.length) * 100}%"></i></div>
          <div class="m-controls">
            <button data-prev aria-label="Previous">${icon('prev')}</button>
            <button data-play aria-label="Play">${player.playing ? icon('pause') : icon('play')}</button>
            <button data-next aria-label="Next">${icon('next')}</button>
            <span style="flex:1"></span>
            <button data-open="music" aria-label="Open Music">${icon('music')}</button>
          </div>
        </div>

        <div class="tile vslider bright" data-slider="brightness">
          <div class="v-fill" style="height:${settings.brightness * 100}%"></div>
          ${icon('sun')}
        </div>

        <div class="utility">
          <button class="util-toggle ${torch ? 'on' : ''}" data-util="flashlight">${icon('flashlight')}<span>Flashlight</span></button>
          <button class="util-toggle" data-util="timer">${icon('timer')}<span>Timer</span></button>
          <button class="util-toggle" data-util="calculator">${icon('calculator')}<span>Calculator</span></button>
          <button class="util-toggle" data-util="camera">${icon('camera')}<span>Camera</span></button>
        </div>

        <div class="tile vslider volume" data-slider="volume">
          <div class="v-fill" style="height:${settings.volume * 100}%"></div>
          ${icon('volume')}
        </div>

        <button class="tile cc-wide" data-focus>
          <span class="w-icon" style="background:${settings.focus ? '#5e5ce6' : 'rgba(255,255,255,.2)'}">
            ${icon('moon')}
          </span>
          <span>Focus</span>
          <span class="w-sub">${settings.focus ? 'On' : 'Off'}</span>
        </button>
        <button class="tile cc-wide" data-darkmode>
          <span class="w-icon">${icon('moon')}</span>
          <span>Dark Mode</span>
          <span class="w-sub">${effectiveAppearance() === 'light' ? 'Off' : 'On'}</span>
        </button>
        <button class="tile cc-wide" data-lowpower>
          <span class="w-icon" style="background:${settings.lowPower ? '#ffd60a' : 'rgba(255,255,255,.2)'}">
            ${icon('battery')}
          </span>
          <span>Low Power</span>
          <span class="w-sub">${settings.lowPower ? 'On' : 'Off'}</span>
        </button>
        <button class="tile cc-wide" data-lockapp>
          <span class="w-icon">${icon('lock')}</span>
          <span>Lock Screen</span>
          <span class="w-sub">Hold</span>
        </button>
      </div>
      <div class="cc-hint">Swipe up or press the home bar to close</div>`;
  }

  function renderControlCentre() {
    ccNode.innerHTML = ccHTML();
    bindControlCentre();
  }

  function bindControlCentre() {
    ccNode.querySelectorAll('[data-conn]').forEach((button) => {
      button.addEventListener('click', () => toggleConnectivity(button.dataset.conn));
    });

    ccNode.querySelectorAll('[data-util]').forEach((button) => {
      button.addEventListener('click', () => utilitySheet(button.dataset.util));
    });

    const playButton = ccNode.querySelector('[data-play]');
    if (playButton) playButton.addEventListener('click', () => player.toggle());
    const prev = ccNode.querySelector('[data-prev]');
    if (prev) prev.addEventListener('click', () => player.prev());
    const next = ccNode.querySelector('[data-next]');
    if (next) next.addEventListener('click', () => player.next());
    const openMusic = ccNode.querySelector('[data-open="music"]');
    if (openMusic) openMusic.addEventListener('click', () => openApp('music'));

    ccNode.querySelectorAll('[data-slider]').forEach((slider) => {
      const apply = (clientY) => {
        const rect = slider.getBoundingClientRect();
        const value = Math.max(0.02, Math.min(1, 1 - (clientY - rect.top) / rect.height));
        if (slider.dataset.slider === 'brightness') {
          settings.brightness = value;
          applyBrightness();
        } else {
          settings.volume = value;
        }
        saveSettings();
        const fill = slider.querySelector('.v-fill');
        if (fill) fill.style.height = `${value * 100}%`;
      };
      slider.addEventListener('pointerdown', (event) => {
        event.preventDefault();
        slider.setPointerCapture(event.pointerId);
        apply(event.clientY);
        const move = (moveEvent) => apply(moveEvent.clientY);
        const up = () => {
          slider.removeEventListener('pointermove', move);
          slider.removeEventListener('pointerup', up);
        };
        slider.addEventListener('pointermove', move);
        slider.addEventListener('pointerup', up);
      });
    });

    const focus = ccNode.querySelector('[data-focus]');
    if (focus) focus.addEventListener('click', () => {
      settings.focus = !settings.focus;
      saveSettings();
      renderControlCentre();
      toast(settings.focus ? 'Focus on — notifications are quiet' : 'Focus off');
      haptic(9);
    });

    const darkMode = ccNode.querySelector('[data-darkmode]');
    if (darkMode) darkMode.addEventListener('click', () => {
      setAppearance(effectiveAppearance() === 'light' ? 'dark' : 'light');
      renderControlCentre();
    });

    const lowPower = ccNode.querySelector('[data-lowpower]');
    if (lowPower) lowPower.addEventListener('click', () => {
      settings.lowPower = !settings.lowPower;
      saveSettings();
      renderControlCentre();
      toast(settings.lowPower ? 'Low Power Mode on' : 'Low Power Mode off');
    });

    const lockButton = ccNode.querySelector('[data-lockapp]');
    if (lockButton) lockButton.addEventListener('click', () => lock());

    const hint = ccNode.querySelector('.cc-hint');
    if (hint) hint.addEventListener('click', () => closeControlCentre());
  }

  function openControlCentre() {
    if (state.locked) return;
    if (state.ccOpen) return;
    closeNotificationCentre();
    closeSwitcher();
    state.ccOpen = true;
    renderControlCentre();
    ccNode.classList.add('open');
    ccNode.setAttribute('aria-hidden', 'false');
    applyChromeTheme();
    haptic(8);
  }

  function closeControlCentre() {
    if (!state.ccOpen) return;
    state.ccOpen = false;
    ccNode.classList.remove('open');
    ccNode.setAttribute('aria-hidden', 'true');
    applyChromeTheme();
  }

  /* ---------------------------------------------------------------- */
  /* Notification Centre                                              */
  /* ---------------------------------------------------------------- */

  function notificationMarkup(notification) {
    const app = APP_BY_ID[notification.app] || APP_BY_ID.core;
    return `
      <button class="notification" data-napp="${notification.app}">
        <span class="n-icon" style="background:${app.tint}">
          <svg viewBox="0 0 24 24" aria-hidden="true">${app.glyph}</svg>
        </span>
        <span class="n-body">
          <span class="n-head"><span>${app.name}</span><span>${notification.time}</span></span>
          <span class="n-title">${notification.title}</span>
          <span class="n-text">${notification.body}</span>
        </span>
      </button>`;
  }

  function renderNotificationCentre() {
    ncNode.innerHTML = `
      <div class="nc-head">
        <div>
          <div class="t" id="nc-time">${clockText()}</div>
          <div class="d" id="nc-date">${dateText()}</div>
        </div>
        ${state.notifications.length ? '<button class="nc-clear" data-clear>Clear</button>' : ''}
      </div>
      ${state.notifications.length
        ? state.notifications.map(notificationMarkup).join('')
        : '<div class="nc-empty">No new notifications</div>'}
      <div class="nc-footer">${icon('lock')}<span>Notifications are private to this device</span></div>`;

    const clear = ncNode.querySelector('[data-clear]');
    if (clear) clear.addEventListener('click', () => {
      state.notifications = [];
      renderNotificationCentre();
      renderLockNotifications();
      haptic(10);
    });
    ncNode.querySelectorAll('[data-napp]').forEach((node) => {
      node.addEventListener('click', () => {
        const id = node.dataset.napp;
        closeNotificationCentre();
        if (APP_BY_ID[id]) openApp(id);
      });
    });
  }

  function openNotificationCentre() {
    if (state.locked) return;
    if (state.ncOpen) return;
    closeControlCentre();
    closeSwitcher();
    state.ncOpen = true;
    renderNotificationCentre();
    ncNode.classList.add('open');
    ncNode.setAttribute('aria-hidden', 'false');
    applyChromeTheme();
    haptic(8);
  }

  function closeNotificationCentre() {
    if (!state.ncOpen) return;
    state.ncOpen = false;
    ncNode.classList.remove('open');
    ncNode.setAttribute('aria-hidden', 'true');
    applyChromeTheme();
  }

  function notify(appId, title, body) {
    state.notifications.unshift({
      id: `n${Date.now()}`,
      app: appId,
      title,
      body,
      time: 'now',
      accent: APP_BY_ID[appId] ? APP_BY_ID[appId].tint : '#0a84ff'
    });
    renderLockNotifications();
    if (state.ncOpen) renderNotificationCentre();
    haptic(10);
  }

  /* ---------------------------------------------------------------- */
  /* App switcher                                                     */
  /* ---------------------------------------------------------------- */

  function previewFor(id) {
    const dots = (count, className = '') =>
      Array.from({ length: count }, () => `<i class="${className}"></i>`).join('');

    switch (id) {
      case 'music':
        return `<div class="pv pv-music">
          <div class="pv-art" style="background:${player.track.art}"></div>
          <div class="pv-h">${player.track.title}</div>
          <div class="pv-sub">${player.track.artist}</div>
          <div class="pv-bar"><i style="width:38%"></i></div>
          <div class="pv-transport">${icon('prev')}${icon('play')}${icon('next')}</div>
        </div>`;
      case 'calculator':
        return `<div class="pv pv-calc">
          <div class="pv-display">1,024</div>
          <div class="pv-pad">${Array.from({ length: 20 }, () => '<i></i>').join('')}</div>
        </div>`;
      case 'settings':
        return `<div class="pv pv-settings">
          <div class="pv-h">Settings</div>
          ${Array.from({ length: 5 }, () => '<div class="pv-row"><span></span><b></b></div>').join('')}
        </div>`;
      case 'files':
        return `<div class="pv pv-files">
          <div class="pv-h">Files</div>
          ${Array.from({ length: 5 }, () => '<div class="pv-row"><span class="sq"></span><b></b></div>').join('')}
        </div>`;
      case 'weather':
        return `<div class="pv pv-weather">
          <div class="pv-city">Frankfurt</div>
          <div class="pv-temp">18°</div>
          <div class="pv-sub">Mostly Sunny</div>
          <div class="pv-hours">${dots(5, 'pill')}</div>
        </div>`;
      case 'snake':
        return `<div class="pv pv-grid">
          ${Array.from({ length: 48 }, (_, index) => `<i class="${[20, 21, 22, 29, 37, 36].includes(index) ? 'on' : ''}"></i>`).join('')}
        </div>`;
      case 'mines':
        return `<div class="pv pv-grid mines-grid">
          ${Array.from({ length: 36 }, (_, index) => `<i class="${[4, 9, 14, 19, 24, 29].includes(index) ? 'num' : ''}"></i>`).join('')}
        </div>`;
      case 'cheesy':
        return `<div class="pv pv-cheesy">
          <span class="ball b1"></span><span class="ball b2"></span><span class="ball b3"></span>
          <div class="pv-burp">NYAM!</div>
        </div>`;
      case 'terminal':
        return `<div class="pv pv-term">
          <div>dim@phone:~$ help</div>
          <div class="dim">DIR TYPE THEME OPEN</div>
          <div>dim@phone:~$ <b class="caret"></b></div>
        </div>`;
      case 'paint':
        return `<div class="pv pv-paint">
          <svg viewBox="0 0 100 160" preserveAspectRatio="none"><path d="M8 118c22-52 30 24 46-8s22-64 38-42" fill="none" stroke="#0a84ff" stroke-width="7" stroke-linecap="round"/></svg>
          <div class="pv-swatches">${dots(8)}</div>
        </div>`;
      case 'core':
      default:
        return `<div class="pv pv-core">
          <div class="pv-chip"><svg viewBox="0 0 24 24" aria-hidden="true">${APP_BY_ID.core.glyph}</svg></div>
          <div class="pv-h">DimOS Core</div>
          <div class="pv-sub">i686 · 55.7 KB</div>
        </div>`;
    }
  }

  function switcherCards() {
    if (!state.recents.length) {
      return '<div class="switcher-empty">No apps are open.<br>Swipe up from an app to see it here.</div>';
    }
    return state.recents.map((id) => {
      const app = APP_BY_ID[id];
      return `
        <div class="switch-card" data-card="${id}">
          <div class="sc-head">
            ${appIconHTML(app)}
            <span>${app.name}</span>
          </div>
          <div class="sc-body">${previewFor(id)}</div>
        </div>`;
    }).join('');
  }

  function openSwitcher() {
    if (state.locked) return;
    closeControlCentre();
    closeNotificationCentre();
    if (!state.recents.length) {
      toast('No recent apps');
      return;
    }
    state.switcherOpen = true;
    switcherNode.innerHTML = `
      <div class="switcher-track">${switcherCards()}</div>
      <div class="switcher-hint">Tap an app to open it · swipe a card up to close · tap the home bar to go home</div>`;
    switcherNode.classList.add('open');
    home.classList.add('switcher-open');
    bindSwitcher();
    applyChromeTheme();
    haptic(10);
  }

  function bindSwitcher() {
    switcherNode.querySelectorAll('[data-card]').forEach((card) => {
      card.addEventListener('click', () => {
        const id = card.dataset.card;
        closeSwitcher(true);
        openApp(id);
      });
      let start = null;
      card.addEventListener('pointerdown', (event) => {
        start = { y: event.clientY };
      });
      card.addEventListener('pointerup', (event) => {
        if (!start) return;
        const dy = event.clientY - start.y;
        start = null;
        if (dy < -70) {
          const id = card.dataset.card;
          state.recents = state.recents.filter((item) => item !== id);
          if (state.appId === id) goHome(true);
          haptic(14);
          if (!state.recents.length) closeSwitcher();
          else {
            switcherNode.querySelector('.switcher-track').innerHTML = switcherCards();
            bindSwitcher();
          }
        }
      });
    });
    switcherNode.addEventListener('click', (event) => {
      if (event.target === switcherNode) closeSwitcher();
    });
  }

  function closeSwitcher(keepHomeState = false) {
    if (!state.switcherOpen) return;
    state.switcherOpen = false;
    switcherNode.classList.remove('open');
    switcherNode.innerHTML = '';
    home.classList.remove('switcher-open');
    if (!keepHomeState) applyChromeTheme();
  }

  /* ---------------------------------------------------------------- */
  /* Sheets                                                           */
  /* ---------------------------------------------------------------- */

  function showSheet({ title = '', body = '', light = null }) {
    const isLight = light === null ? effectiveAppearance() === 'light' : light;
    sheetLayer.innerHTML = `
      <div class="sheet-scrim" data-close></div>
      <div class="sheet ${isLight ? 'light' : ''}" role="dialog" aria-label="${title}">
        <div class="grabber"></div>
        <h3>${title}</h3>
        ${body}
      </div>`;
    sheetLayer.classList.add('open');
    sheetLayer.setAttribute('aria-hidden', 'false');
    state.sheetOpen = true;
    sheetLayer.querySelector('[data-close]').addEventListener('click', hideSheet);
    applyChromeTheme();
    haptic(6);
  }

  function hideSheet() {
    if (!state.sheetOpen) return;
    state.sheetOpen = false;
    sheetLayer.classList.remove('open');
    sheetLayer.setAttribute('aria-hidden', 'true');
    sheetLayer.innerHTML = '';
    applyChromeTheme();
  }

  /* ---------------------------------------------------------------- */
  /* Haptic-touch context menu and jiggle mode                        */
  /* ---------------------------------------------------------------- */

  function openContextMenu(id, anchor) {
    const app = APP_BY_ID[id];
    if (!app) return;
    closeControlCentre();
    closeNotificationCentre();
    closeSwitcher();
    hideSheet();
    state.contextOpen = true;
    contextLayer.innerHTML = `
      <div class="context-backdrop" data-close-ctx></div>
      <div class="context-wrap">
        <div class="ctx-app">
          ${appIconHTML(app)}
          <span>${app.name}</span>
        </div>
        <div class="ctx-menu">
          <button data-ctx="open">Open ${app.name}</button>
          <button data-ctx="edit">Edit Home Screen</button>
          <button data-ctx="info">App Info</button>
        </div>
      </div>`;
    contextLayer.classList.add('open');
    contextLayer.setAttribute('aria-hidden', 'false');
    if (anchor) {
      const rect = anchor.getBoundingClientRect();
      const deviceRect = device.getBoundingClientRect();
      contextLayer.style.setProperty('--ctx-x', `${rect.left + rect.width / 2 - deviceRect.left}px`);
      contextLayer.style.setProperty('--ctx-y', `${rect.top + rect.height / 2 - deviceRect.top}px`);
    }
    contextLayer.querySelector('[data-close-ctx]').addEventListener('click', closeContextMenu);
    contextLayer.querySelector('[data-ctx="open"]').addEventListener('click', () => {
      closeContextMenu();
      openApp(id);
    });
    contextLayer.querySelector('[data-ctx="edit"]').addEventListener('click', () => {
      closeContextMenu();
      startEditing();
    });
    contextLayer.querySelector('[data-ctx="info"]').addEventListener('click', () => {
      closeContextMenu();
      showSheet({
        title: app.name,
        body: `<div class="ctx-about">
          <p>${app.category} · built into DimOS 14</p>
          <p style="opacity:.6;font-size:13px;line-height:1.5">This app also exists in the bare-metal x86 edition. Open Core to boot it.</p>
        </div>`
      });
    });
    haptic(14);
    applyChromeTheme();
  }

  function closeContextMenu() {
    if (!state.contextOpen) return;
    state.contextOpen = false;
    contextLayer.classList.remove('open');
    contextLayer.setAttribute('aria-hidden', 'true');
    contextLayer.innerHTML = '';
    applyChromeTheme();
  }

  function startEditing() {
    state.editing = true;
    device.classList.add('editing');
    const done = el('button', 'edit-done', 'Done');
    done.addEventListener('click', stopEditing);
    device.appendChild(done);
    toast('Arrange apps, then tap Done', 2600);
    haptic(12);
  }

  function stopEditing() {
    state.editing = false;
    device.classList.remove('editing');
    document.querySelector('.edit-done')?.remove();
    haptic(10);
  }

  function bindLongPress() {
    let timer = 0;
    let start = null;

    device.addEventListener('pointerdown', (event) => {
      const cell = event.target.closest('.app-cell[data-open]');
      if (!cell || state.locked || state.appId || state.editing || state.contextOpen) return;
      start = { x: event.clientX, y: event.clientY };
      clearTimeout(timer);
      timer = setTimeout(() => {
        if (!start) return;
        suppressClickUntil = Date.now() + 600;
        openContextMenu(cell.dataset.open, cell);
        start = null;
      }, 470);
    });

    const cancel = (event) => {
      if (start && event && Math.hypot(event.clientX - start.x, event.clientY - start.y) > 12) {
        clearTimeout(timer);
        start = null;
      }
    };
    device.addEventListener('pointermove', cancel);
    device.addEventListener('pointerup', () => { clearTimeout(timer); start = null; });
    device.addEventListener('pointercancel', () => { clearTimeout(timer); start = null; });
    device.addEventListener('contextmenu', (event) => event.preventDefault());
  }

  /* ---------------------------------------------------------------- */
  /* Settings from the outside                                        */
  /* ---------------------------------------------------------------- */

  function setWallpaper(id) {
    if (!WALLPAPERS.some((item) => item.id === id)) return;
    settings.wallpaper = id;
    saveSettings();
    applyWallpaper();
    toast('Wallpaper changed');
    haptic(8);
  }

  function setAppearance(mode) {
    if (!['dark', 'light', 'auto'].includes(mode)) return;
    settings.appearance = mode;
    saveSettings();
    applyAppearance();
    if (state.ccOpen) renderControlCentre();
    if (state.ncOpen) renderNotificationCentre();
    if (state.appId === 'settings') {
      const view = appWindow.querySelector('.appview');
      if (view) view.classList.toggle('light', effectiveAppearance() === 'light');
    }
    haptic(8);
  }

  function isInstalled() {
    return window.matchMedia('(display-mode: standalone)').matches || window.navigator.standalone === true;
  }

  async function promptInstall() {
    if (installPrompt) {
      installPrompt.prompt();
      const choice = await installPrompt.userChoice;
      if (choice.outcome === 'accepted') toast('DimOS 14 is installing');
      installPrompt = null;
      return;
    }
    showSheet({
      title: 'Install DimOS 14',
      body: `<p style="font-size:14px;line-height:1.55;opacity:.72;margin:0 0 4px">
        ${isInstalled()
          ? 'DimOS is already running from your home screen. Nice.'
          : 'Open this page in Chrome on Android, then use the browser menu and choose <b>Add to Home screen</b>. On iPhone, use Share → Add to Home Screen.'}
      </p>`
    });
  }

  function deviceInfo() {
    const connection = navigator.connection || {};
    return {
      Model: 'DimOS Phone 14',
      Edition: 'Phone (PWA) 14.0.1',
      Display: `${Math.round(window.screen.width)} × ${Math.round(window.screen.height)}`,
      'Design canvas': '390 × 844 pt',
      'Device pixel ratio': String(window.devicePixelRatio || 1),
      Platform: navigator.platform || 'Web',
      Engine: (navigator.userAgentData && navigator.userAgentData.brands?.[0]?.brand) || 'Browser',
      Network: connection.effectiveType ? connection.effectiveType.toUpperCase() : '5G',
      Language: navigator.language,
      'Standalone': isInstalled() ? 'Yes' : 'No',
      'x86 kernel': '55.7 KB · i686'
    };
  }

  /* ---------------------------------------------------------------- */
  /* Gestures                                                         */
  /* ---------------------------------------------------------------- */

  function unlockGesture() {
    let start = null;
    let moved = false;

    lockScreen.addEventListener('pointerdown', (event) => {
      if (event.target.closest('.lock-quick') || event.target.closest('.notification')) return;
      start = { y: event.clientY, x: event.clientX };
      moved = false;
      lockScreen.classList.add('dragging');
      try { lockScreen.setPointerCapture?.(event.pointerId); } catch (_) { /* synthetic events */ }
    });

    lockScreen.addEventListener('pointermove', (event) => {
      if (!start) return;
      const dy = event.clientY - start.y;
      if (Math.abs(dy) > 6 || Math.abs(event.clientX - start.x) > 6) moved = true;
      if (dy < 0) {
        lockScreen.style.transform = `translateY(${dy}px)`;
        lockScreen.style.opacity = String(Math.max(0.25, 1 + dy / 500));
      }
    });

    const finish = (event) => {
      if (!start) return;
      const dy = event.clientY - start.y;
      lockScreen.classList.remove('dragging');
      if (dy < -60 || !moved) {
        unlock();
        lockScreen.style.transform = '';
        lockScreen.style.opacity = '';
      } else {
        lockScreen.style.transform = '';
        lockScreen.style.opacity = '';
      }
      start = null;
    };
    lockScreen.addEventListener('pointerup', finish);
    lockScreen.addEventListener('pointercancel', () => {
      start = null;
      lockScreen.classList.remove('dragging');
      lockScreen.style.transform = '';
      lockScreen.style.opacity = '';
    });

    lockScreen.querySelectorAll('.notification').forEach((node) => {
      node.addEventListener('click', (event) => {
        event.stopPropagation();
        unlock();
        setTimeout(() => openApp(node.dataset.napp), 240);
      });
    });

    $('#lock-flashlight').addEventListener('click', (event) => {
      event.stopPropagation();
      const on = $('#lock-flashlight').classList.toggle('on');
      toast(on ? 'Flashlight on' : 'Flashlight off');
      haptic(10);
    });
    $('#lock-hint').addEventListener('click', (event) => {
      event.stopPropagation();
      unlock();
    });
    $('#lock-camera').addEventListener('click', (event) => {
      event.stopPropagation();
      toast('Camera needs a real sensor');
    });
  }

  function pagesGesture() {
    pagesNode.addEventListener('pointerdown', (event) => {
      if (event.pointerType === 'mouse' && event.button !== 0) return;
      if (state.locked || state.appId) return;
      if (event.target.closest('input')) return;
      measure();
      drag = {
        x: event.clientX,
        y: event.clientY,
        axis: null,
        startPage: state.page,
        moved: false,
        time: Date.now()
      };
      pagesNode.classList.add('dragging');
    });

    pagesNode.addEventListener('pointermove', (event) => {
      if (!drag) return;
      const dx = event.clientX - drag.x;
      const dy = event.clientY - drag.y;
      if (!drag.axis && Math.hypot(dx, dy) > 9) {
        drag.axis = Math.abs(dx) > Math.abs(dy) ? 'x' : 'y';
        if (drag.axis === 'x' && pagesNode.setPointerCapture) {
          try { pagesNode.setPointerCapture(event.pointerId); } catch (_) { /* ignore */ }
        }
      }
      if (drag.axis !== 'x') return;
      drag.moved = true;
      suppressClickUntil = Date.now() + 260;
      let offset = -drag.startPage * pageWidth + dx;
      const min = -2 * pageWidth;
      if (offset > 0) offset *= 0.35;
      if (offset < min) offset = min + (offset - min) * 0.35;
      pagesNode.style.transform = `translateX(${offset}px)`;
      event.preventDefault();
    });

    const end = () => {
      if (!drag) return;
      const wasX = drag.axis === 'x';
      const startPage = drag.startPage;
      pagesNode.classList.remove('dragging');
      if (wasX) {
        const delta = parseFloat((pagesNode.style.transform.match(/-?[\d.]+px/) || ['0px'])[0]);
        const moved = delta + startPage * pageWidth;
        let next = startPage;
        if (Math.abs(moved) > pageWidth * 0.18) next = startPage + (moved < 0 ? 1 : -1);
        goToPage(next);
      }
      drag = null;
    };

    pagesNode.addEventListener('pointerup', end);
    pagesNode.addEventListener('pointercancel', () => {
      pagesNode.classList.remove('dragging');
      drag = null;
      goToPage(state.page, false);
    });

    pagesNode.addEventListener('wheel', (event) => {
      if (Math.abs(event.deltaX) < Math.abs(event.deltaY) * 1.4) return;
      if (Date.now() < (pagesNode.__wheelLock || 0)) return;
      pagesNode.__wheelLock = Date.now() + 550;
      goToPage(state.page + (event.deltaX > 0 ? 1 : -1));
    }, { passive: true });
  }

  function statusBarGesture() {
    statusbar.style.pointerEvents = 'auto';
    let start = null;
    statusbar.addEventListener('pointerdown', (event) => {
      if (state.locked) return;
      start = { x: event.clientX, y: event.clientY, t: Date.now() };
      try { statusbar.setPointerCapture?.(event.pointerId); } catch (_) { /* synthetic events */ }
    });
    statusbar.addEventListener('pointermove', (event) => {
      if (!start) return;
      const dy = event.clientY - start.y;
      if (dy > 44 && !state.ccOpen && !state.ncOpen) {
        const rect = statusbar.getBoundingClientRect();
        const left = (start.x - rect.left) < rect.width / 2;
        if (left) openNotificationCentre();
        else openControlCentre();
        start = null;
      }
    });
    statusbar.addEventListener('pointerup', (event) => {
      if (!start) return;
      const dx = event.clientX - start.x;
      const rect = statusbar.getBoundingClientRect();
      if (Math.abs(dx) < 12) {
        const left = (start.x - rect.left) < rect.width / 2;
        if (left) {
          if (state.ncOpen) closeNotificationCentre();
          else openNotificationCentre();
        } else {
          if (state.ccOpen) closeControlCentre();
          else openControlCentre();
        }
      }
      start = null;
    });
  }

  function overlayGestures() {
    [ccNode, ncNode].forEach((overlay) => {
      let start = null;
      overlay.addEventListener('pointerdown', (event) => {
        start = { x: event.clientX, y: event.clientY };
      });
      overlay.addEventListener('pointerup', (event) => {
        if (!start) return;
        const dy = event.clientY - start.y;
        const dx = Math.abs(event.clientX - start.x);
        start = null;
        if (dy < -55 && dx < 120) {
          if (overlay === ccNode) closeControlCentre();
          else closeNotificationCentre();
        }
      });
      overlay.addEventListener('wheel', (event) => {
        if (event.deltaY > 24) {
          if (overlay === ccNode) closeControlCentre();
          else closeNotificationCentre();
        }
      }, { passive: true });
    });
  }

  function homebarGestures() {
    let start = null;
    let timer = 0;
    let fired = false;

    homebar.addEventListener('pointerdown', (event) => {
      start = { y: event.clientY, x: event.clientX };
      fired = false;
      clearTimeout(timer);
      timer = setTimeout(() => {
        fired = true;
        if (state.appId) {
          haptic(12);
          openSwitcher();
        } else if (!state.ccOpen && !state.ncOpen && !state.switcherOpen) {
          openSwitcher();
        }
      }, 480);
      try { homebar.setPointerCapture?.(event.pointerId); } catch (_) { /* synthetic events */ }
    });

    homebar.addEventListener('pointermove', (event) => {
      if (!start || fired) return;
      if (event.clientY - start.y < -34) {
        clearTimeout(timer);
        fired = true;
        if (state.switcherOpen) return;
        if (state.appId) {
          haptic(12);
          openSwitcher();
        }
      }
    });

    const finish = (event) => {
      clearTimeout(timer);
      const dy = event.clientY - (start ? start.y : event.clientY);
      start = null;
      if (fired) return;
      if (dy < -20) {
        goHome();
        return;
      }
      if (state.switcherOpen) closeSwitcher();
      else if (state.ccOpen) closeControlCentre();
      else if (state.ncOpen) closeNotificationCentre();
      else if (state.sheetOpen) hideSheet();
      else if (state.appId) goHome();
      else if (state.page !== 0) goToPage(0);
    };

    homebar.addEventListener('pointerup', finish);
    homebar.addEventListener('pointercancel', () => {
      clearTimeout(timer);
      start = null;
    });

    homebar.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        goHome();
      }
    });
  }

  function appGestures() {
    let start = null;
    appHost.addEventListener('pointerdown', (event) => {
      if (!state.appId) return;
      start = { x: event.clientX, y: event.clientY, time: Date.now() };
    });
    appHost.addEventListener('pointerup', (event) => {
      if (!start) return;
      const dx = event.clientX - start.x;
      const dy = event.clientY - start.y;
      const fromEdge = start.x < 60;
      start = null;
      if (dx > 90 && Math.abs(dy) < 70 && fromEdge) {
        goHome();
      }
    });
  }

  /* ---------------------------------------------------------------- */
  /* Global wiring                                                    */
  /* ---------------------------------------------------------------- */

  function bindHomeClicks() {
    device.addEventListener('click', (event) => {
      const opener = event.target.closest('[data-open]');
      if (opener) {
        if (state.editing) {
          toast('Tap Done when you are finished');
          return;
        }
        if (Date.now() < suppressClickUntil) return;
        const id = opener.dataset.open;
        haptic(8);
        if (id === 'none') {
          toast('No app for this widget yet');
          return;
        }
        if (state.ccOpen) closeControlCentre();
        openApp(id, opener.closest('.app-cell') || opener);
        return;
      }
    });

    dotsNode.addEventListener('click', (event) => {
      const button = event.target.closest('[data-page]');
      if (!button) return;
      goToPage(Number(button.dataset.page));
    });

    $('#search-pill').addEventListener('click', () => {
      goToPage(2);
      setTimeout(() => $('#library-search')?.focus({ preventScroll: true }), 380);
    });

    pagesNode.addEventListener('input', (event) => {
      if (event.target.id === 'library-search') renderLibrary(event.target.value);
    });

    $$('.lock-quick').forEach((button) => {
      button.addEventListener('pointerdown', (event) => event.stopPropagation());
    });
  }

  function bindKeyboard() {
    window.addEventListener('keydown', (event) => {
      if (event.ctrlKey && event.key === '1') {
        event.preventDefault();
        lock();
        return;
      }
      if (event.ctrlKey && event.key === '2') {
        event.preventDefault();
        openControlCentre();
        return;
      }
      if (event.ctrlKey && event.key === '3') {
        event.preventDefault();
        openNotificationCentre();
        return;
      }
      if (event.ctrlKey && event.key === '4') {
        event.preventDefault();
        openSwitcher();
        return;
      }
      if (event.key === 'Escape') {
        if (state.contextOpen) closeContextMenu();
        else if (state.editing) stopEditing();
        else if (state.sheetOpen) hideSheet();
        else if (state.ccOpen) closeControlCentre();
        else if (state.ncOpen) closeNotificationCentre();
        else if (state.switcherOpen) closeSwitcher();
        else if (state.appId) goHome();
        else if (state.locked) unlock();
        else if (state.page !== 0) goToPage(0);
        return;
      }
      if (event.key === 'ArrowRight' && !state.appId && !state.locked && !state.ccOpen && !state.ncOpen) {
        goToPage(state.page + 1);
      }
      if (event.key === 'ArrowLeft' && !state.appId && !state.locked && !state.ccOpen && !state.ncOpen) {
        goToPage(state.page - 1);
      }
    });
  }

  function bindInstall() {
    window.addEventListener('beforeinstallprompt', (event) => {
      event.preventDefault();
      installPrompt = event;
    });
    window.addEventListener('appinstalled', () => toast('DimOS 14 installed'));
  }

  function bindPlayer() {
    player.subscribe(() => {
      const widget = $('.music-widget');
      if (widget) {
        const title = widget.querySelector('.mw-title');
        const artist = widget.querySelector('.mw-artist');
        const art = widget.querySelector('.mw-art');
        const bar = widget.querySelector('.mw-progress i');
        if (title) title.textContent = player.track.title;
        if (artist) artist.textContent = player.track.artist;
        if (art) art.style.background = player.track.art;
        if (bar) bar.style.width = `${(player.progress / player.track.length) * 100}%`;
      }
      if (state.ccOpen) {
        const tile = ccNode.querySelector('.music-tile');
        if (tile) {
          const title = tile.querySelector('.m-title');
          const artist = tile.querySelector('.m-artist');
          const art = tile.querySelector('.m-art');
          const bar = tile.querySelector('.m-bar i');
          const play = tile.querySelector('[data-play]');
          if (title) title.textContent = player.track.title;
          if (artist) artist.textContent = player.track.artist;
          if (art) art.style.background = player.track.art;
          if (bar) bar.style.width = `${(player.progress / player.track.length) * 100}%`;
          if (play) play.innerHTML = player.playing ? icon('pause') : icon('play');
        }
      }
    });
  }

  /* ---------------------------------------------------------------- */
  /* Boot                                                             */
  /* ---------------------------------------------------------------- */

  function sizeDevice() {
    const desktop = window.matchMedia('(min-width: 620px), (min-height: 960px)').matches;
    if (desktop) {
      const available = window.innerHeight - 140;
      const zoom = Math.min(1.02, available / 844, (window.innerWidth - 40) / 390);
      device.style.setProperty('--zoom', String(Math.max(0.4, zoom)));
      device.style.setProperty('--u', '1px');
    } else {
      device.style.setProperty('--u', `${window.innerWidth / 390}px`);
      device.style.setProperty('--zoom', '1');
    }
    const height = device.getBoundingClientRect().height;
    if (desktop) {
      document.getElementById('stage').style.paddingBottom = '0';
      $('#desk-note').style.display = window.innerHeight > 700 ? 'flex' : 'none';
    }
    if (height) measure();
  }

  function tick() {
    refreshClock();
  }

  function init() {
    device.classList.add('locked');
    applyWallpaper();
    applyAppearance();
    applyBrightness();
    refreshNetwork();
    renderHome();
    renderLockNotifications();
    refreshClock();
    sizeDevice();
    bindHomeClicks();
    bindKeyboard();
    bindInstall();
    bindPlayer();
    unlockGesture();
    pagesGesture();
    statusBarGesture();
    overlayGestures();
    homebarGestures();
    appGestures();
    bindLongPress();
    initBattery();

    setInterval(tick, 1000);
    setInterval(() => {
      if (state.ncOpen) {
        const time = $('#nc-time');
        if (time) time.textContent = clockText();
      }
    }, 1000);

    window.addEventListener('resize', sizeDevice);
    window.addEventListener('orientationchange', () => setTimeout(sizeDevice, 250));
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) refreshClock();
    });

    setTimeout(() => boot.classList.add('gone'), 720);
    setTimeout(() => { boot.style.display = 'none'; }, 1450);
  }

  return { init, ctx, state };
}
