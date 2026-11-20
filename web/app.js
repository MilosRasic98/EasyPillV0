/* EasyPill GUI.
 *
 * Two rules shape this file:
 *
 *  - Every timestamp from the box is a *local* epoch (UTC seconds already
 *    shifted by the device's offset), so dates are read back with the UTC
 *    getters. Using the local getters would apply the browser's offset on top
 *    and shift everything by hours.
 *  - The firmware's WebServer serves one client at a time, so every request
 *    goes through a single-flight queue. Parallel fetches would leave one
 *    socket waiting on the other and the 1 Hz poll would start missing beats.
 */
'use strict';
(function () {

  /* ------------------------------------------------------------------ util */

  var SVGNS = 'http://www.w3.org/2000/svg';
  var DAY = 86400;

  function $(id) { return document.getElementById(id); }
  function qs(sel, root) { return (root || document).querySelector(sel); }
  function qsa(sel, root) {
    return Array.prototype.slice.call((root || document).querySelectorAll(sel));
  }
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined && text !== null) n.textContent = text;
    return n;
  }
  function icon(name, cls) {
    var s = document.createElementNS(SVGNS, 'svg');
    s.setAttribute('class', cls || 'i');
    s.setAttribute('aria-hidden', 'true');
    var u = document.createElementNS(SVGNS, 'use');
    u.setAttribute('href', '#i-' + name);
    s.appendChild(u);
    return s;
  }
  function setIcon(svg, name) {
    var u = svg.querySelector('use');
    if (u) u.setAttribute('href', '#i-' + name);
  }
  function clear(n) { while (n.firstChild) n.removeChild(n.firstChild); }
  function show(n, on) { if (n) n.hidden = !on; }

  function num(v, d) {
    return (typeof v === 'number' && isFinite(v)) ? v : (d === undefined ? 0 : d);
  }
  function str(v) { return typeof v === 'string' ? v : ''; }
  function bool(v) { return v === true; }
  function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
  function pad2(n) { return n < 10 ? '0' + n : '' + n; }

  var DOW = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  var MON = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  var LIDNAME = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];

  function D(ts) { return new Date(ts * 1000); }
  function hhmm(ts) { var d = D(ts); return pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes()); }
  function hhmmss(ts) { var d = D(ts); return hhmm(ts) + ':' + pad2(d.getUTCSeconds()); }
  function dateShort(ts) { var d = D(ts); return DOW[d.getUTCDay()] + ' ' + d.getUTCDate() + ' ' + MON[d.getUTCMonth()]; }
  function stamp(ts) { return dateShort(ts) + ' ' + hhmm(ts); }

  function dayIdx(ts) { return Math.floor(ts / DAY); }
  function minOfDay(ts) { return Math.floor((ts - dayIdx(ts) * DAY) / 60); }
  function wdMon0(day) { return ((day % 7) + 3 + 7) % 7; }   // 1970-01-01 was a Thursday
  function minToHhmm(m) { m = clamp(Math.round(num(m)), 0, 1439); return pad2(Math.floor(m / 60)) + ':' + pad2(m % 60); }
  function hhmmToMin(s) {
    var m = /^(\d{1,2}):(\d{2})/.exec(str(s));
    return m ? clamp(+m[1], 0, 23) * 60 + clamp(+m[2], 0, 59) : 0;
  }

  function dhms(s) {
    s = Math.floor(Math.abs(s));
    if (s < 60) return s + ' s';
    var m = Math.floor(s / 60) % 60, h = Math.floor(s / 3600) % 24, d = Math.floor(s / DAY);
    if (d > 0) return d + ' d ' + pad2(h) + ' h';
    if (h > 0) return h + ' h ' + pad2(m) + ' m';
    return m + ' min';
  }
  function rel(ts, now) {
    var d = now - ts;
    if (Math.abs(d) < 45) return d >= 0 ? 'just now' : 'in a moment';
    return d > 0 ? dhms(d) + ' ago' : 'in ' + dhms(d);
  }
  function dayWord(ts, now) {
    var a = dayIdx(ts), b = dayIdx(now);
    if (a === b) return 'today';
    if (a === b + 1) return 'tomorrow';
    if (a === b - 1) return 'yesterday';
    return dateShort(ts);
  }

  /* --------------------------------------------------------------- network */

  var queue = [];
  var busy = false;

  function pump() {
    if (busy || !queue.length) return;
    busy = true;
    var job = queue.shift();
    var ac = (typeof AbortController === 'function') ? new AbortController() : null;
    var timer = setTimeout(function () { if (ac) ac.abort(); }, job.timeout);
    var init = { method: job.method, cache: 'no-store' };
    if (ac) init.signal = ac.signal;
    if (job.body !== undefined) {
      init.headers = { 'Content-Type': 'application/json' };
      init.body = JSON.stringify(job.body);
    }
    var p = fetch(job.url, init).then(function (r) {
      return r.text().then(function (txt) {
        var j = null;
        try { j = txt ? JSON.parse(txt) : null; } catch (e) { j = null; }
        if (!r.ok) {
          var e1 = new Error((j && str(j.error)) || ('HTTP ' + r.status));
          e1.status = r.status; e1.body = j; throw e1;
        }
        if (j && j.ok === false) {
          var e2 = new Error(str(j.error) || 'the box refused that');
          e2.body = j; throw e2;
        }
        return j;
      });
    });
    function done() { clearTimeout(timer); busy = false; pump(); }
    p.then(function (v) { done(); job.res(v); }, function (e) { done(); job.rej(e); });
  }

  function req(method, url, body, timeout) {
    return new Promise(function (res, rej) {
      queue.push({ method: method, url: url, body: body, timeout: timeout || 6000, res: res, rej: rej });
      pump();
    });
  }
  function get(u, t) { return req('GET', u, undefined, t); }

  // Several things can ask for the same document at once (boot, a tab switch,
  // a reconcile after a POST). On a board that answers one request at a time,
  // firing three identical GETs is three missed hall scans, so share one.
  var inflight = Object.create(null);
  function getOnce(key, u, t) {
    if (inflight[key]) return inflight[key];
    var p = get(u, t);
    inflight[key] = p;
    function clearKey() { delete inflight[key]; }
    p.then(clearKey, clearKey);
    return p;
  }
  function post(u, b, t) { return req('POST', u, b || {}, t); }

  /* ----------------------------------------------------------- app state */

  var S = {
    st: null,            // last good /api/state, never cleared by a failed poll
    settings: null,
    sched: null,
    hist: [],
    histDays: 14,
    histSel: -1,
    info: null,
    view: 'now',
    fails: 0,
    lastOkMs: 0,
    dirty: false,
    seen: Object.create(null),
    simScale: 1
  };

  var devNow = 0, devAt = 0;
  function anchorClock(sec) { devNow = sec; devAt = (window.performance ? performance.now() : Date.now()); }
  function nowLocal() {
    if (!devNow) return 0;
    var mono = (window.performance ? performance.now() : Date.now());
    return devNow + (mono - devAt) / 1000;
  }

  /* --------------------------------------------------------------- toasts */

  var toastsEl = $('toasts');
  function toast(msg, kind, ms) {
    var t = el('div', 'toast' + (kind ? ' toast--' + kind : ''), msg);
    toastsEl.appendChild(t);
    setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, ms || (kind === 'bad' ? 7000 : 3500));
  }

  /* --------------------------------------------------------------- dialog */

  var dlg = $('dlg'), dlgH = $('dlgH'), dlgBody = $('dlgBody'), dlgFoot = $('dlgFoot');
  var dlgResolve = null, primaryBtn = null;

  function openDlg(title, nodes, buttons) {
    dlgH.textContent = title;
    clear(dlgBody);
    nodes.forEach(function (n) { dlgBody.appendChild(n); });
    clear(dlgFoot);
    primaryBtn = null;
    buttons.forEach(function (b) {
      var btn = el('button', 'btn' + (b.cls ? ' ' + b.cls : '') + (b.left ? ' left' : ''), b.label);
      btn.type = 'button';
      btn.addEventListener('click', function () {
        if (b.before && b.before() === false) return;
        closeDlg(b.value);
      });
      if (b.primary) primaryBtn = btn;
      dlgFoot.appendChild(btn);
    });
    return new Promise(function (res) {
      dlgResolve = res;
      if (typeof dlg.showModal === 'function') dlg.showModal(); else dlg.setAttribute('open', '');
      var first = qs('input, select, textarea, button', dlgBody);
      if (first) try { first.focus(); } catch (e) { /* iOS can refuse focus */ }
    });
  }
  // close() fires its event in a later task, while the promise handler runs in
  // a microtask - so when one dialog opens the next (dose editor -> bulk
  // editor) the stale close event would land on the dialog just opened.
  var selfClose = false;
  function closeDlg(v) {
    var r = dlgResolve; dlgResolve = null;
    if (typeof dlg.close === 'function' && dlg.open) { selfClose = true; dlg.close(); }
    else dlg.removeAttribute('open');
    if (r) r(v);
  }
  dlg.addEventListener('cancel', function (e) { e.preventDefault(); closeDlg(null); });
  dlg.addEventListener('close', function () {
    if (selfClose) { selfClose = false; return; }
    if (dlgResolve) closeDlg(null);
  });
  $('dlgForm').addEventListener('submit', function (e) {
    e.preventDefault();
    if (primaryBtn) primaryBtn.click();
  });

  function confirmDlg(title, body, okLabel, danger) {
    var p = el('p', 'lede', body);
    return openDlg(title, [p], [
      { label: 'Cancel', value: false },
      { label: okLabel, value: true, primary: true, cls: danger ? 'btn--danger' : 'btn--primary' }
    ]).then(function (v) { return v === true; });
  }

  /* ----------------------------------------------------------- state pips */

  var PIP = {
    taken: ['taken', 'check', 'Taken on time'],
    late: ['late', 'clock', 'Taken late'],
    early: ['early', 'ff', 'Taken early'],
    pending: ['pending', 'dot', 'Not taken yet'],
    missed: ['missed', 'x', 'Missed'],
    skipped: ['skipped', 'minus', 'Skipped'],
    // Scheduled before the box was powered on. Nobody knows whether it was
    // taken, the firmware does not count it, and saying "not taken yet" would
    // claim knowledge the box does not have.
    unwatched: ['none', 'minus', 'Before the box was watching'],
    none: ['none', 'minus', 'Nothing scheduled']
  };
  function pipInfo(state) { return PIP[state] || PIP.none; }
  function applyPip(span, state, small) {
    var p = pipInfo(state);
    span.className = 'pip pip--' + p[0] + (small ? ' pip--sm' : '');
    span.setAttribute('role', 'img');
    span.setAttribute('aria-label', p[2]);
    span.title = p[2];
    var svg = span.querySelector('svg');
    if (svg) setIcon(svg, p[1]);
  }
  function newPip(state, small) {
    var span = el('span');
    span.appendChild(icon('dot'));
    applyPip(span, state, small);
    return span;
  }

  /* ------------------------------------------------------------- polling */

  var bannerEl = $('banner'), bannerTxt = $('bannerTxt');
  var pollT = 0, retryAtMs = 0;
  var BACKOFF = [1, 2, 3, 5, 8, 10, 15];

  function pollEvery() { return document.hidden ? 5000 : 1000; }

  function schedulePoll(ms) {
    clearTimeout(pollT);
    retryAtMs = Date.now() + ms;
    pollT = setTimeout(poll, ms);
  }

  function poll() {
    if (busy) { schedulePoll(250); return; }   // a user action is in flight; it wins
    get('/api/state', 4500).then(onState, onPollFail);
  }

  function onState(j) {
    if (!j) { onPollFail(new Error('empty reply')); return; }
    S.st = j;
    S.fails = 0;
    S.lastOkMs = Date.now();
    anchorClock(num(j.now));
    show(bannerEl, false);
    document.body.classList.remove('stale');
    renderState();
    schedulePoll(pollEvery());
  }

  function onPollFail(e) {
    S.fails++;
    // Keep the last known state on screen. A pillbox GUI that blanks every
    // time the box deep-sleeps is worse than one showing a stale reading.
    if (S.fails >= 2) {
      show(bannerEl, true);
      document.body.classList.add('stale');
    }
    schedulePoll(BACKOFF[Math.min(S.fails - 1, BACKOFF.length - 1)] * 1000);
    renderStrip();
  }

  $('bannerRetry').addEventListener('click', function () { schedulePoll(0); });

  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) schedulePoll(0); else schedulePoll(pollEvery());
  });

  /* ----------------------------------------------------- render: top bar */

  function renderHeader() {
    var st = S.st || {}, setg = S.settings || {};
    var ui = setg.ui || {};
    var name = str(ui.deviceName) || 'EasyPill';
    $('devName').textContent = name;
    document.title = name;
    var who = str(ui.patientName);
    $('patientName').textContent = who ? 'Medication for ' + who : '';
    show($('simBadge'), bool(st.simulated));
  }

  function stripItem(id, iconName, value, cls, title) {
    var n = $(id);
    if (!n) return;
    n.className = 'strip__it' + (id === 'sAge' ? ' strip__it--age' : '') + (cls ? ' ' + cls : '');
    if (iconName) {
      var svg = n.querySelector('svg');
      if (svg) setIcon(svg, iconName);
    }
    qs('.strip__v', n).textContent = value;
    if (title) n.title = title; else n.removeAttribute('title');
  }

  function renderStrip() {
    var st = S.st;
    if (!st) {
      stripItem('sAge', null, 'connecting…', 'is-stale');
      return;
    }
    var p = st.power || {}, w = st.wifi || {};

    // clock
    var verified = bool(st.timeVerified);
    stripItem('sClock', 'clock', devNow ? hhmmss(nowLocal()) : '--:--:--',
      verified ? '' : 'is-warn',
      verified ? dateShort(nowLocal()) : 'The clock has not been checked against NTP yet, so times may be wrong');

    // battery
    var pct = num(p.percent);
    stripItem('sBatt', 'battery', pct + '%',
      bool(p.lowBattery) ? 'is-bad' : '',
      (num(p.vbatMv) / 1000).toFixed(2) + ' V');

    // charger
    var chg = 'on battery', ci = 'plug', ccls = '';
    if (bool(p.charging)) { chg = 'charging'; ci = 'bolt'; ccls = 'is-ok'; }
    else if (bool(p.usbPresent)) { chg = 'USB'; ci = 'plug'; ccls = 'is-ok'; }
    else if (bool(p.coilPresent)) { chg = 'coil'; ci = 'bolt'; ccls = 'is-ok'; }
    stripItem('sChg', ci, chg, ccls,
      'USB ' + (num(p.usbMv) / 1000).toFixed(2) + ' V, coil ' + (num(p.coilMv) / 1000).toFixed(2) + ' V');

    // wifi
    var mode = str(w.mode);
    var wtxt = mode === 'ap' ? 'setup AP' : (str(w.ssid) || 'offline');
    stripItem('sWifi', 'wifi', wtxt, (mode === 'sta' && str(w.ip)) ? '' : 'is-warn',
      (str(w.ip) || 'no address') + (num(w.rssi) ? ', ' + num(w.rssi) + ' dBm' : ''));

    // freshness
    var age = S.lastOkMs ? Math.round((Date.now() - S.lastOkMs) / 1000) : 0;
    stripItem('sAge', null, S.fails === 0 ? 'live' : age + ' s ago', S.fails >= 2 ? 'is-stale' : '',
      'Polling every ' + (pollEvery() / 1000) + ' s');
  }

  function updateBanner() {
    if (bannerEl.hidden) return;
    var secs = Math.max(0, Math.round((retryAtMs - Date.now()) / 1000));
    var age = S.lastOkMs ? dhms((Date.now() - S.lastOkMs) / 1000) : null;
    bannerTxt.textContent = 'Cannot reach the box — retrying in ' + secs + ' s.' +
      (age ? ' Showing the reading from ' + age + ' ago.' : '');
  }

  /* -------------------------------------------------- render: next dose */

  var nextCard = $('next');

  function renderPillList(ul, pills) {
    clear(ul);
    var arr = Array.isArray(pills) ? pills : [];
    if (!arr.length) { ul.appendChild(el('li', null, 'No pills listed')); return; }
    arr.forEach(function (p) {
      var li = el('li');
      li.appendChild(el('b', null, str(p.name) || 'Pill'));
      var d = str(p.dose);
      if (d) li.appendChild(el('small', null, d));
      var c = num(p.count, 1);
      if (c > 1) li.appendChild(el('small', null, '× ' + c));
      ul.appendChild(li);
    });
  }

  function renderNext() {
    var nd = (S.st && S.st.nextDose) || {};
    var valid = bool(nd.valid);
    nextCard.classList.toggle('is-none', !valid);
    if (!valid) {
      $('nextLabel').textContent = 'Nothing scheduled';
      clear($('nextPills'));
      $('nextLidName').textContent = '—';
      $('nextAt').textContent = '';
      $('nextWhen').textContent = '';
      $('nextCountdown').textContent = '—';
      $('nextSub').textContent = 'Add doses in the Schedule tab.';
      nextCard.classList.remove('is-due');
      return;
    }
    $('nextLabel').textContent = str(nd.label) || 'Dose';
    renderPillList($('nextPills'), nd.pills);
    $('nextLidName').textContent = str(nd.lidName) || ('Lid ' + (num(nd.lid) + 1));
    $('nextAt').textContent = hhmm(num(nd.at));
    updateCountdown();
  }

  function updateCountdown() {
    var nd = (S.st && S.st.nextDose) || {};
    if (!bool(nd.valid) || !devNow) return;
    var at = num(nd.at), now = nowLocal(), left = at - now;
    $('nextWhen').textContent = dayWord(at, now) + ' at ' + hhmm(at);
    nextCard.classList.toggle('is-due', left <= 0);
    $('nextCountdown').textContent = left > 0 ? 'in ' + dhms(left) : 'due ' + dhms(left) + ' ago';
    $('nextSub').textContent = stamp(at) + ' · ' + (str(nd.lidName) || 'lid') + ', dose ' + (num(nd.dose) + 1);
  }

  /* ------------------------------------------------------ render: lids */

  var lidEls = [];

  function ensureLids() {
    if (lidEls.length) return;
    var tpl = $('t-lid'), host = $('lids');
    for (var i = 0; i < 7; i++) {
      var n = tpl.content.firstElementChild.cloneNode(true);
      lidEls.push({
        root: n,
        day: qs('.lid__day', n),
        pip: qs('.pip', n),
        state: qs('.lid__statetxt', n),
        next: qs('.lid__next', n),
        open: null
      });
      (function (lid) {
        function goto() { setView('schedule'); highlightLidRow(lid); }
        n.addEventListener('click', goto);
        n.addEventListener('keydown', function (e) {
          if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); goto(); }
        });
      }(i));
      host.appendChild(n);
    }
  }

  function lidNextLabel(at, now) {
    var a = dayIdx(at), b = dayIdx(now);
    if (a === b) return hhmm(at);
    if (a === b + 1) return 'tom ' + hhmm(at);
    return DOW[D(at).getUTCDay()] + ' ' + hhmm(at);
  }

  function renderLids() {
    ensureLids();
    var st = S.st; if (!st) return;
    var lids = Array.isArray(st.lids) ? st.lids : [];
    var now = nowLocal();
    for (var i = 0; i < 7; i++) {
      var L = lids[i] || {}, o = lidEls[i];
      var nm = str(L.name) || LIDNAME[i];
      if (o.day.textContent !== nm) o.day.textContent = nm;

      var open = bool(L.open);
      if (o.open !== open) {
        o.open = open;
        o.root.classList.toggle('is-open', open);
        // Re-trigger the nudge so a lid moving between two polls is impossible
        // to miss on camera.
        o.root.classList.remove('just');
        void o.root.offsetWidth;
        o.root.classList.add('just');
      }
      o.state.textContent = open ? 'Open' : 'Closed';
      o.root.classList.toggle('lid--today', bool(L.activeToday));

      var ts = str(L.todayState) || 'none';
      applyPip(o.pip, ts, false);

      var na = num(L.nextAt);
      o.next.textContent = na ? lidNextLabel(na, now) : (num(L.doseCount) ? '—' : 'no doses');
      o.root.setAttribute('aria-label', nm + ': lid ' + (open ? 'open' : 'closed') +
        ', ' + pipInfo(ts)[2].toLowerCase() + (na ? ', next dose ' + hhmm(na) : ''));
    }
  }

  /* -------------------------------------------------- render: timeline */

  function lidForDay(sch, day) {
    if (!sch) return wdMon0(day);
    if (sch.mode === 'sequential') {
      var d = (day - num(sch.anchor)) % 7;
      return (d + 7) % 7;
    }
    return wdMon0(day);
  }

  function dosesOnDay(day) {
    var sch = S.sched;
    if (!sch) return [];
    var lid = lidForDay(sch, day);
    var L = sch.lids[lid];
    if (!L) return [];
    var out = [];
    L.doses.forEach(function (d, idx) {
      if (!d.enabled) return;
      out.push({ at: day * DAY + d.minute * 60, lid: lid, dose: idx, label: d.label, pills: d.pills, lidName: L.name });
    });
    out.sort(function (a, b) { return a.at - b.at; });
    return out;
  }

  function recordFor(at, lid, dose) {
    var h = S.hist;
    for (var i = 0; i < h.length; i++) {
      if (h[i].scheduledAt === at && h[i].lid === lid && h[i].dose === dose) return h[i];
    }
    return null;
  }

  function stateWord(st, rec) {
    var w = pipInfo(st)[2];
    if (rec && (st === 'late' || st === 'early') && num(rec.deltaMin)) {
      var dm = Math.abs(num(rec.deltaMin));
      w += ' (' + dm + ' min)';
    }
    return w;
  }

  function renderTimeline() {
    var host = $('timeline');
    clear(host);
    if (!devNow) return;
    var now = nowLocal(), day = dayIdx(now);
    var list = dosesOnDay(day);
    var nd = (S.st && S.st.nextDose) || {};

    $('todaySub').textContent = dateShort(now) +
      (S.st && S.st.adherence ? ' · ' + num(S.st.adherence.takenToday) + ' of ' + num(S.st.adherence.dueToday) + ' taken' : '');

    if (!S.sched) { host.appendChild(el('li', 'empty', 'Loading the schedule…')); return; }
    if (!list.length) { host.appendChild(el('li', 'empty', 'Nothing is scheduled for today.')); return; }

    list.forEach(function (d) {
      var rec = recordFor(d.at, d.lid, d.dose);
      // No record yet means the box has not judged this dose. Before its window
      // closes that is simply "pending"; after, the firmware writes a missed
      // record, so a gap here is still shown as pending rather than invented.
      var st = rec ? str(rec.state) || 'pending' : 'pending';
      // watchingSince is the local epoch this box was last cold-booted. A dose
      // older than that was never its business, which is why the counter in
      // the subtitle leaves it out; say so rather than showing it as untaken.
      var floor = num(S.st && S.st.watchingSince);
      if (!rec && floor && d.at < floor) st = 'unwatched';
      var li = el('li', 'tl--' + st);
      if (bool(nd.valid) && num(nd.at) === d.at && num(nd.lid) === d.lid && num(nd.dose) === d.dose) {
        li.classList.add('is-next');
      }
      li.appendChild(newPip(st, true));
      li.appendChild(el('span', 't', hhmm(d.at)));
      var nm = el('span', 'nm');
      nm.appendChild(el('b', null, d.label || 'Dose ' + (d.dose + 1)));
      var names = d.pills.map(function (p) {
        return (str(p.name) || 'pill') + (num(p.count, 1) > 1 ? ' ×' + num(p.count, 1) : '');
      }).join(', ');
      nm.appendChild(el('small', null, names || 'no pills listed'));
      li.appendChild(nm);
      li.appendChild(el('span', 'st', stateWord(st, rec)));
      host.appendChild(li);
    });
  }

  /* ------------------------------------------------------ render: feed */

  var EV = {
    boot: ['power', 'info'], lidOpened: ['pill', 'warn'], lidClosed: ['check', ''],
    doseTaken: ['check', 'ok'], doseMissed: ['x', 'bad'], doseDue: ['clock', 'info'],
    doseReminder: ['bell', 'info'], unexpectedOpen: ['warn', 'warn'], wrongDayOpen: ['warn', 'warn'],
    buttonPress: ['dot', ''], buttonHold: ['dot', ''], chargerIn: ['bolt', 'ok'],
    chargerOut: ['plug', ''], batteryLow: ['battery', 'warn'], batteryCritical: ['battery', 'bad'],
    timeSynced: ['clock', 'ok'], wifiUp: ['wifi', 'ok'], wifiDown: ['wifi', 'warn'],
    sleepEnter: ['moon', ''], sleepExit: ['refresh', ''], configChanged: ['sliders', ''],
    sensorFault: ['warn', 'bad']
  };

  function humanType(t) {
    var s = str(t).replace(/([a-z0-9])([A-Z])/g, '$1 $2');
    return s ? s.charAt(0).toUpperCase() + s.slice(1).toLowerCase() : 'Event';
  }
  // Not keyed on `at`: an event logged before the first sync carries at == 0 and
  // has its real stamp backfilled later, so keying on it would file one event
  // twice. uptimeMs is stamped once, when the event is logged.
  function evKey(e) {
    return num(e.uptimeMs) + '|' + str(e.type) + '|' + num(e.lid) + '|' + str(e.text);
  }

  // An event logged before the first sync carries at == 0, because the clock
  // had never been verified at the time, and only its uptimeMs means anything.
  // There is no wall-clock time to show and the box is not allowed to
  // invent one, so those rows are placed by their uptime against the uptimeS in
  // the same reply - the same reconstruction backfillTimes() does on the box.
  function evUptimeWord(upMs) {
    var upNow = num(S.st && S.st.uptimeS);
    if (!upNow) return 'before the clock was set';
    var age = upNow - upMs / 1000;
    // The event log survives a reboot but millis() does not, so an uptime ahead
    // of this boot's belongs to an earlier one. uptimeS is whole seconds, hence
    // the slack.
    if (age < -2) return 'before this boot';
    return age < 45 ? 'just now' : dhms(age) + ' ago';
  }

  function paintEvTime(b) {
    var at = num(parseInt(b.dataset.at, 10)), text, title;
    if (!at) {
      var upMs = num(parseInt(b.dataset.up, 10));
      text = evUptimeWord(upMs);
      title = 'Logged ' + dhms(upMs / 1000) + ' after power-on, when the clock had not been checked yet';
    } else {
      text = !devNow ? hhmm(at) : (b.dataset.abs === '1' ? stamp(at) : rel(at, nowLocal()));
      title = stamp(at) + (b.dataset.unv === '1' ? ' (clock unverified when recorded)' : '');
    }
    var aria = 'Recorded ' + (at ? stamp(at) : text + ', when the clock had not been checked yet');
    // Only write what actually changed: this runs on every row once a second,
    // and replacing a text node that reads the same interrupts a selection and
    // makes a screen reader start the row again.
    if (b.textContent !== text) b.textContent = text;
    if (b.title !== title) b.title = title;
    if (b.getAttribute('aria-label') !== aria) b.setAttribute('aria-label', aria);
  }

  function makeEvNode(e) {
    var n = $('t-ev').content.firstElementChild.cloneNode(true);
    var def = EV[str(e.type)] || ['info', ''];
    setIcon(qs('.ev__i svg', n), def[0]);
    if (def[1]) n.classList.add('ev--' + def[1]);
    qs('.ev__txt', n).textContent = str(e.text) || humanType(e.type);
    var tb = qs('.ev__t', n);
    tb.dataset.at = String(num(e.at));
    tb.dataset.up = String(num(e.uptimeMs));
    tb.dataset.unv = e.timeVerified === false ? '1' : '0';
    paintEvTime(tb);
    tb.addEventListener('click', function () {
      var abs = tb.dataset.abs === '1';
      tb.dataset.abs = abs ? '0' : '1';
      refreshEvTimes();
    });
    if (e.timeVerified === false) n.classList.add('unverified');
    return n;
  }

  function addEvents(list, append) {
    var host = $('feed');
    var empty = qs('.empty', host);
    var added = 0;
    // Only genuinely new events slide in; the first batch after a reload is
    // history, and animating all of it just looks like a glitch.
    var hadContent = !empty && host.children.length > 0;
    // /api/state hands them over newest first; walking backwards and inserting
    // at the top keeps the feed ordered without re-sorting the DOM.
    var order = append ? list.slice() : list.slice().reverse();
    order.forEach(function (e) {
      var k = evKey(e);
      if (S.seen[k]) {
        // Already on screen. The one thing that can still change is the stamp,
        // which arrives when the box backfills an at == 0 event after a sync.
        var old = qs('.ev__t', S.seen[k]);
        if (old && old.dataset.at !== String(num(e.at))) {
          old.dataset.at = String(num(e.at));
          paintEvTime(old);
        }
        return;
      }
      var n = makeEvNode(e);
      S.seen[k] = n;
      if (append) host.appendChild(n);
      else {
        if (hadContent) n.classList.add('is-new');
        if (host.firstChild) host.insertBefore(n, host.firstChild);
        else host.appendChild(n);
      }
      added++;
    });
    if (empty && added) host.removeChild(empty);
    if (!host.firstChild) host.appendChild(el('li', 'empty', 'No events recorded yet.'));
    while (host.children.length > 90) host.removeChild(host.lastChild);
    return added;
  }

  // No !devNow guard: the rows derived from uptime keep ageing even on a box
  // that has never had a clock at all.
  function refreshEvTimes() {
    qsa('.ev__t').forEach(paintEvTime);
  }

  $('moreEvents').addEventListener('click', function (e) {
    var b = e.currentTarget;
    b.disabled = true;
    get('/api/events?limit=64', 8000).then(function (j) {
      b.disabled = false;
      var n = addEvents(Array.isArray(j && j.events) ? j.events : [], true);
      toast(n ? 'Loaded ' + n + ' older events' : 'Nothing older to load', n ? 'ok' : null);
    }, function (err) { b.disabled = false; toast('Could not load events: ' + err.message, 'bad'); });
  });

  /* ------------------------------------------------- render: simulator */

  var simBtns = [];

  function ensureSim() {
    if (simBtns.length) return;
    var host = $('simLids');
    for (var i = 0; i < 7; i++) {
      var b = el('button', 'btn');
      b.type = 'button';
      b.appendChild(el('span', null, LIDNAME[i]));
      (function (lid, btn) {
        btn.addEventListener('click', function () {
          var L = (S.st && S.st.lids && S.st.lids[lid]) || {};
          var body = {};
          body[bool(L.open) ? 'closeLid' : 'openLid'] = lid;
          simSend(body);
        });
      }(i, b));
      simBtns.push(b);
      host.appendChild(b);
    }
  }

  function simSend(body) {
    post('/api/sim', body, 6000).then(function (j) {
      if (j && j.sim && typeof j.sim.timeScale === 'number') { S.simScale = j.sim.timeScale; renderSimScale(); }
      schedulePoll(0);
    }, function (e) { toast('Simulator: ' + e.message, 'bad'); });
  }

  function renderSimScale() {
    qsa('button', $('simScale')).forEach(function (b) {
      b.setAttribute('aria-pressed', String(+b.dataset.ts === S.simScale));
    });
  }

  var simVDrag = false;

  function renderSim() {
    var on = bool(S.st && S.st.simulated);
    show($('sim'), on);
    if (!on) return;
    ensureSim();
    var lids = (S.st && S.st.lids) || [];
    for (var i = 0; i < 7; i++) {
      var open = bool((lids[i] || {}).open);
      var b = simBtns[i];
      qs('span', b).textContent = (open ? 'Close ' : 'Open ') + str((lids[i] || {}).name || LIDNAME[i]);
      b.classList.toggle('btn--on', open);
    }
    var p = (S.st && S.st.power) || {};
    if (!simVDrag) {
      var mv = clamp(num(p.vbatMv, 3900), 3200, 4200);
      $('simV').value = String(mv);
      $('simVOut').value = (mv / 1000).toFixed(2) + ' V';
    }
    if (document.activeElement !== $('simUsb')) $('simUsb').checked = bool(p.usbPresent);
    if (document.activeElement !== $('simCoil')) $('simCoil').checked = bool(p.coilPresent);
    renderSimScale();
  }

  (function wireSim() {
    var v = $('simV'), out = $('simVOut');
    function showV() { out.value = (num(+v.value) / 1000).toFixed(2) + ' V'; }
    v.addEventListener('input', function () { simVDrag = true; showV(); });
    v.addEventListener('change', function () { simVDrag = false; simSend({ setVbatMv: +v.value }); });
    $('simUsb').addEventListener('change', function () { simSend({ setUsb: $('simUsb').checked }); });
    $('simCoil').addEventListener('change', function () { simSend({ setCoil: $('simCoil').checked }); });
    qsa('button', $('simScale')).forEach(function (b) {
      b.addEventListener('click', function () { S.simScale = +b.dataset.ts; renderSimScale(); simSend({ timeScale: +b.dataset.ts }); });
    });
    qsa('[data-sim]').forEach(function (b) {
      b.addEventListener('click', function () {
        var k = b.dataset.sim;
        if (k === 'press') simSend({ pressButton: 400 });
        else if (k === 'hold') simSend({ pressButton: 3000 });
        else if (k === 'h1') simSend({ advanceMin: 60 });
        else if (k === 'd1') simSend({ advanceMin: 1440 });
        else if (k === 'nextdose') {
          // The shortcut a demo actually needs: stand the box up right before a
          // reminder instead of nudging the clock an hour at a time.
          var nd = (S.st && S.st.nextDose) || {};
          if (!bool(nd.valid)) { toast('Nothing is scheduled to jump to', 'bad'); return; }
          simSend({ setClock: Math.round(num(nd.at) - 120) });
        }
        else if (k === 'reset') {
          confirmDlg('Reset the simulator', 'Puts the fake hardware back to its defaults: every lid closed, battery at 3.9 V, real-time clock speed. The schedule and settings are not touched.', 'Reset', true)
            .then(function (ok) { if (ok) simSend({ reset: true }); });
        }
      });
    });
  }());

  /* ------------------------------------------------------- render: all */

  var histStale = '';
  var cfgStale = null;   // null until the first reply is looked at
  var lastTodayLid = -1;

  function renderState() {
    renderHeader();
    renderStrip();
    renderNext();
    renderLids();
    renderSim();
    addEvents(Array.isArray(S.st.events) ? S.st.events : [], false);
    renderTimeline();

    // Dose records only change when something happens, so refetch the history
    // on the newest event rather than on a timer.
    var evs = S.st.events;
    var sig = (Array.isArray(evs) && evs.length ? evKey(evs[0]) : '') + '|' +
      (S.st.adherence ? num(S.st.adherence.takenToday) + '/' + num(S.st.adherence.missedToday) : '');
    if (sig !== histStale) {
      histStale = sig;
      loadHistory(false);
    }

    // /api/state carries no schedule, so a change made from another browser
    // would sit unnoticed in this one's cache until the page is reloaded. The
    // configChanged event every write logs is the only invalidation we get.
    var cfg = '';
    if (Array.isArray(evs)) {
      for (var ci = 0; ci < evs.length; ci++) {
        if (str(evs[ci].type) === 'configChanged') { cfg = evKey(evs[ci]); break; }
      }
    }
    if (cfg !== cfgStale) {
      // Not while a dialog is open or somebody is typing in the week table:
      // renderSchedule() rebuilds the table and would take the focus and the
      // half-typed lid name with it. The next poll tries again.
      var firstLook = cfgStale === null;
      var editing = dlg.open || $('v-schedule').contains(document.activeElement);
      if (firstLook || !editing) {
        cfgStale = cfg;
        if (!firstLook && S.sched) loadSchedule(true);
      }
    }
    if (S.view === 'history') renderHistStats();

    // Which lid is "today" is only known once the clock has arrived, and it
    // changes again at midnight. Redraw the week when it moves.
    if (S.sched) {
      var t = lidForDay(S.sched, dayIdx(nowLocal()));
      if (t !== lastTodayLid) { lastTodayLid = t; renderSchedule(); }
    }
  }

  /* --------------------------------------------------- schedule: model */

  function doseMinuteOf(d) {
    if (typeof d.minute === 'number' && isFinite(d.minute)) return clamp(Math.round(d.minute), 0, 1439);
    var m = /^(\d{1,2}):(\d{2})/.exec(str(d.time));
    return m ? clamp(+m[1], 0, 23) * 60 + clamp(+m[2], 0, 59) : 480;
  }

  function normDose(d) {
    d = d || {};
    var pills = [], ps = Array.isArray(d.pills) ? d.pills : [];
    for (var i = 0; i < ps.length && pills.length < 4; i++) {
      var p = ps[i] || {};
      pills.push({ name: str(p.name), dose: str(p.dose), count: clamp(num(p.count, 1), 1, 99) });
    }
    return { enabled: d.enabled !== false, minute: doseMinuteOf(d), label: str(d.label), pills: pills };
  }

  function normSched(j) {
    j = j || {};
    var s = {
      mode: str(j.mode) === 'sequential' ? 'sequential' : 'weekday',
      anchor: num(j.sequentialAnchorDay),
      windowBeforeMin: num(j.windowBeforeMin, 60),
      windowAfterMin: num(j.windowAfterMin, 120),
      missedGraceMin: num(j.missedGraceMin, 15),
      remindBeforeMin: num(j.remindBeforeMin, 0),
      lids: []
    };
    var src = Array.isArray(j.lids) ? j.lids : [];
    for (var i = 0; i < 7; i++) {
      var L = src[i] || {}, lid = { name: str(L.name) || LIDNAME[i], doses: [] };
      var ds = Array.isArray(L.doses) ? L.doses : [];
      for (var k = 0; k < ds.length && lid.doses.length < 4; k++) lid.doses.push(normDose(ds[k]));
      s.lids.push(lid);
    }
    return s;
  }

  function doseBody(d) {
    return {
      enabled: d.enabled, time: minToHhmm(d.minute), minute: d.minute, label: d.label,
      pills: d.pills.map(function (p) { return { name: p.name, dose: p.dose, count: p.count }; })
    };
  }
  // Post only what the user touched. Lids absent from the array are left
  // alone, and so is a listed lid's dose list if the entry carries no
  // 'doses' key. Sending all seven lids out of a cache that may be hours old
  // silently undoes whatever another browser changed in the meantime.
  //   touch.lids   lid indices to include; the key is omitted when there are none
  //   touch.doses  include those lids' dose lists - false means rename only
  //   touch.rules  include the top-level timing fields
  // mode and sequentialAnchorDay are read-only in this GUI, so they are never
  // echoed back: absent means "leave it alone" on both servers.
  function schedBody(s, touch) {
    touch = touch || {};
    var body = {};
    if (touch.rules) {
      body.windowBeforeMin = s.windowBeforeMin;
      body.windowAfterMin = s.windowAfterMin;
      body.missedGraceMin = s.missedGraceMin;
      body.remindBeforeMin = s.remindBeforeMin;
    }
    if (touch.lids && touch.lids.length) {
      body.lids = touch.lids.map(function (i) {
        var L = s.lids[i], e = { lid: i, name: L.name };
        if (touch.doses) e.doses = L.doses.map(doseBody);
        return e;
      });
    }
    return body;
  }

  function loadSchedule(quiet) {
    return getOnce('sched', '/api/schedule', 8000).then(function (j) {
      S.sched = normSched(j);
      renderSchedule();
      renderTimeline();
    }, function (e) {
      if (!quiet) toast('Could not read the schedule: ' + e.message, 'bad');
    });
  }

  // Optimistic: change the local copy, draw it, then post. A refused post puts
  // the old schedule back on screen so the user sees the change undone.
  function commitSchedule(mutate, what, touch) {
    if (!S.sched) return;
    var snap = JSON.stringify(S.sched);
    mutate(S.sched);
    renderSchedule();
    renderTimeline();
    post('/api/schedule', schedBody(S.sched, touch), 9000).then(function () {
      toast(what + ' saved', 'ok');
      loadSchedule(true);
      schedulePoll(0);
    }, function (e) {
      S.sched = JSON.parse(snap);
      renderSchedule();
      renderTimeline();
      toast('Not saved: ' + e.message + '. The change has been undone.', 'bad');
    });
  }

  /* ---------------------------------------------------- schedule: view */

  function pillSummary(d) {
    if (!d.pills.length) return 'no pills';
    return d.pills.map(function (p) {
      return (str(p.name) || 'pill') + (num(p.count, 1) > 1 ? ' ×' + num(p.count, 1) : '');
    }).join(', ');
  }

  function renderSchedSummary() {
    var host = $('schedSum');
    clear(host);
    if (!S.sched) return;
    var doses = 0, pills = 0, emptyLids = 0, perDay = [];
    S.sched.lids.forEach(function (L) {
      var n = 0;
      L.doses.forEach(function (d) {
        if (!d.enabled) return;
        n++;
        d.pills.forEach(function (p) { pills += num(p.count, 1); });
      });
      doses += n;
      perDay.push(n);
      if (!n) emptyLids++;
    });
    var lo = Math.min.apply(null, perDay), hi = Math.max.apply(null, perDay);
    function chip(v, lbl) {
      var c = el('div', 'chip');
      c.appendChild(el('b', null, v));
      c.appendChild(el('span', null, lbl));
      host.appendChild(c);
    }
    chip(String(pills), 'pills per week');
    chip(String(doses), 'doses per week');
    chip(lo === hi ? String(lo) : lo + '–' + hi, 'doses per day');
    chip(String(emptyLids), emptyLids === 1 ? 'empty lid' : 'empty lids');
  }

  function highlightLidRow(lid) {
    var row = qs('tr[data-lid="' + lid + '"]', $('weekBody'));
    if (!row) return;
    var inp = qs('input', row);
    if (inp) try { inp.focus(); } catch (e) { /* ignore */ }
    row.scrollIntoView({ block: 'center' });
  }

  function renderSchedule() {
    renderSchedSummary();
    var body = $('weekBody');
    clear(body);
    if (!S.sched) {
      var tr0 = el('tr'), td0 = el('td', 'empty', 'Loading…');
      td0.colSpan = 5; tr0.appendChild(td0); body.appendChild(tr0);
      return;
    }
    var today = devNow ? lidForDay(S.sched, dayIdx(nowLocal())) : -1;

    S.sched.lids.forEach(function (L, i) {
      var tr = el('tr');
      tr.dataset.lid = String(i);
      if (i === today) tr.className = 'is-today';

      var th = el('th');
      th.scope = 'row';
      var nameIn = el('input', 'lidname');
      nameIn.type = 'text';
      nameIn.value = L.name;
      nameIn.maxLength = 15;
      nameIn.setAttribute('aria-label', 'Name of lid ' + (i + 1));
      nameIn.addEventListener('change', function () {
        var v = nameIn.value.trim() || LIDNAME[i];
        if (v === L.name) return;
        commitSchedule(function (s) { s.lids[i].name = v; }, 'Lid name', { lids: [i] });
      });
      th.appendChild(nameIn);
      th.appendChild(el('span', null, i === today ? 'today' : 'lid ' + (i + 1)));
      tr.appendChild(th);

      for (var k = 0; k < 4; k++) {
        var td = el('td');
        if (k < L.doses.length) td.appendChild(doseCell(i, k, L.doses[k]));
        else if (k === L.doses.length) td.appendChild(addCell(i));
        tr.appendChild(td);
      }
      body.appendChild(tr);
    });
    fillRules();
  }

  function doseCell(lid, idx, d) {
    var b = el('button', 'dcell' + (d.enabled ? '' : ' dcell--off'));
    b.type = 'button';
    var t = el('span', 'dcell__t');
    t.appendChild(document.createTextNode(minToHhmm(d.minute)));
    b.appendChild(t);
    b.appendChild(el('span', 'dcell__lb', d.label || 'Dose ' + (idx + 1)));
    b.appendChild(el('span', 'dcell__p', pillSummary(d)));
    b.setAttribute('aria-label', 'Edit the ' + minToHhmm(d.minute) + ' dose on ' +
      S.sched.lids[lid].name + (d.enabled ? '' : ', currently disabled'));
    b.addEventListener('click', function () { editDose(lid, idx); });
    return b;
  }

  function addCell(lid) {
    var b = el('button', 'dcell dcell--add');
    b.type = 'button';
    b.appendChild(icon('plus'));
    b.appendChild(el('span', null, 'Add dose'));
    b.setAttribute('aria-label', 'Add a dose to ' + S.sched.lids[lid].name);
    b.addEventListener('click', function () { editDose(lid, -1); });
    return b;
  }

  /* ------------------------------------------------ schedule: dose form */

  function field(labelTxt, input, hint) {
    var p = el('p', 'field');
    var id = input.id || ('f' + Math.random().toString(36).slice(2, 8));
    input.id = id;
    var l = el('label', null, labelTxt);
    l.htmlFor = id;
    p.appendChild(l);
    p.appendChild(input);
    if (hint) p.appendChild(el('span', 'hint', hint));
    return p;
  }

  function buildDoseForm(d) {
    var wrap = el('div');

    var en = el('input'); en.type = 'checkbox'; en.checked = d.enabled;
    var enL = el('label', 'sw');
    enL.appendChild(en);
    enL.appendChild(el('span', null, 'This dose is active'));
    wrap.appendChild(enL);

    var grid = el('div', 'form form--2');
    var time = el('input'); time.type = 'time'; time.required = true; time.value = minToHhmm(d.minute);
    var label = el('input'); label.type = 'text'; label.maxLength = 15; label.value = d.label;
    label.setAttribute('list', 'doseLabels');
    grid.appendChild(field('Time', time));
    grid.appendChild(field('Label', label, 'Morning, Noon, Evening, Night…'));
    wrap.appendChild(grid);

    var dl = el('datalist');
    dl.id = 'doseLabels';
    ['Morning', 'Noon', 'Afternoon', 'Evening', 'Night'].forEach(function (v) {
      var o = el('option'); o.value = v; dl.appendChild(o);
    });
    wrap.appendChild(dl);

    wrap.appendChild(el('h3', null, 'Pills in this dose'));
    var list = el('div', 'pilled');
    wrap.appendChild(list);

    var addBtn = el('button', 'btn');
    addBtn.type = 'button';
    addBtn.appendChild(icon('plus'));
    addBtn.appendChild(el('span', null, 'Add a pill'));
    var addRow = el('div', 'btnrow');
    addRow.appendChild(addBtn);
    wrap.appendChild(addRow);

    var rows = [];
    function addPill(p) {
      if (rows.length >= 4) { toast('Four pills per dose is the firmware limit', null); return; }
      var row = el('div', 'pilled__row');
      var nm = el('input'); nm.type = 'text'; nm.maxLength = 23; nm.value = str(p && p.name);
      var ds = el('input'); ds.type = 'text'; ds.maxLength = 15; ds.value = str(p && p.dose);
      var ct = el('input'); ct.type = 'number'; ct.min = '1'; ct.max = '99'; ct.value = String(num(p && p.count, 1));
      var rm = el('button', 'btn btn--danger-ghost rm');
      rm.type = 'button';
      rm.title = 'Remove this pill';
      rm.setAttribute('aria-label', 'Remove this pill');
      rm.appendChild(icon('trash'));
      row.appendChild(field('Name', nm));
      row.appendChild(field('Dose', ds));
      row.appendChild(field('Count', ct));
      row.appendChild(rm);
      var entry = { row: row, nm: nm, ds: ds, ct: ct };
      rm.addEventListener('click', function () {
        list.removeChild(row);
        rows.splice(rows.indexOf(entry), 1);
        addBtn.disabled = false;
      });
      rows.push(entry);
      list.appendChild(row);
      addBtn.disabled = rows.length >= 4;
    }
    addBtn.addEventListener('click', function () { addPill(null); });
    if (d.pills.length) d.pills.forEach(addPill); else addPill(null);

    return {
      root: wrap,
      read: function () {
        var pills = [];
        rows.forEach(function (r) {
          var n = r.nm.value.trim();
          if (!n) return;                       // a blank row just means "not used"
          pills.push({ name: n, dose: r.ds.value.trim(), count: clamp(num(parseInt(r.ct.value, 10), 1), 1, 99) });
        });
        return {
          enabled: en.checked,
          minute: hhmmToMin(time.value || '08:00'),
          label: label.value.trim(),
          pills: pills
        };
      }
    };
  }

  function editDose(lid, idx) {
    if (!S.sched) return;
    var existing = idx >= 0;
    var d = existing ? S.sched.lids[lid].doses[idx] : { enabled: true, minute: 480, label: '', pills: [] };
    var form = buildDoseForm(d);
    var title = (existing ? 'Edit dose ' + (idx + 1) + ' — ' : 'New dose — ') + S.sched.lids[lid].name;
    var buttons = [];
    if (existing) buttons.push({ label: 'Remove dose', value: 'rm', cls: 'btn--danger-ghost', left: true });
    buttons.push({ label: 'Apply across days…', value: 'bulk' });
    buttons.push({ label: 'Cancel', value: null });
    buttons.push({ label: existing ? 'Save dose' : 'Add dose', value: 'ok', primary: true, cls: 'btn--primary' });

    openDlg(title, [form.root], buttons).then(function (v) {
      if (!v) return;
      if (v === 'rm') {
        commitSchedule(function (s) { s.lids[lid].doses.splice(idx, 1); },
                       'Dose removed', { lids: [lid], doses: true });
        return;
      }
      var nd = form.read();
      if (v === 'bulk') { bulkDialog(nd, existing ? idx : 255); return; }
      commitSchedule(function (s) {
        // The schedule can be refetched while this dialog is open, so a slot
        // that no longer exists becomes an append: a hole in the doses array
        // would hand doseBody an undefined.
        if (existing && idx < s.lids[lid].doses.length) s.lids[lid].doses[idx] = nd;
        else if (s.lids[lid].doses.length < 4) s.lids[lid].doses.push(nd);
      }, 'Dose', { lids: [lid], doses: true });
    });
  }

  /* ------------------------------------------------ schedule: bulk tools */

  function lidPicker(preset) {
    var wrap = el('div');
    var shortcuts = el('div', 'btnrow');
    var pick = el('div', 'lidpick');
    var boxes = [];
    for (var i = 0; i < 7; i++) {
      var lab = el('label');
      var cb = el('input'); cb.type = 'checkbox';
      cb.checked = preset ? (preset & (1 << i)) !== 0 : false;
      lab.appendChild(cb);
      lab.appendChild(el('span', null, (S.sched ? S.sched.lids[i].name : LIDNAME[i])));
      pick.appendChild(lab);
      boxes.push(cb);
    }
    [['All seven', 127], ['Weekdays', 31], ['Weekend', 96], ['None', 0]].forEach(function (s) {
      var b = el('button', 'btn btn--tiny', s[0]);
      b.type = 'button';
      b.addEventListener('click', function () {
        boxes.forEach(function (cb, i) { cb.checked = (s[1] & (1 << i)) !== 0; });
      });
      shortcuts.appendChild(b);
    });
    wrap.appendChild(shortcuts);
    wrap.appendChild(pick);
    return {
      root: wrap,
      mask: function () {
        var m = 0;
        boxes.forEach(function (cb, i) { if (cb.checked) m |= (1 << i); });
        return m;
      }
    };
  }

  function bulkDialog(seed, slot) {
    if (!S.sched) return;
    var d = seed || { enabled: true, minute: 480, label: '', pills: [] };
    var nodes = [];

    nodes.push(el('p', 'lede',
      'Puts one dose definition on several lids in a single request. This is the "same pill every day" button.'));

    var opSel = el('select');
    [['set', 'Set this dose on the selected lids'],
     ['remove', 'Remove that dose slot from the selected lids'],
     ['clear', 'Clear every dose on the selected lids']].forEach(function (o) {
      var op = el('option', null, o[1]); op.value = o[0]; opSel.appendChild(op);
    });
    var slotSel = el('select');
    var apnd = el('option', null, 'First free slot'); apnd.value = '255'; slotSel.appendChild(apnd);
    for (var k = 0; k < 4; k++) {
      var o2 = el('option', null, 'Dose slot ' + (k + 1)); o2.value = String(k); slotSel.appendChild(o2);
    }
    slotSel.value = String(num(slot, 255));

    var opGrid = el('div', 'form form--2');
    opGrid.appendChild(field('What to do', opSel));
    opGrid.appendChild(field('Which slot', slotSel, 'Slot 1 is the first dose of that day.'));
    nodes.push(opGrid);

    var picker = lidPicker(127);
    nodes.push(el('h3', null, 'Lids to change'));
    nodes.push(picker.root);

    var form = buildDoseForm(d);
    var formWrap = el('div');
    formWrap.appendChild(el('h3', null, 'The dose'));
    formWrap.appendChild(form.root);
    nodes.push(formWrap);

    function syncOp() {
      var op = opSel.value;
      formWrap.hidden = op !== 'set';
      slotSel.disabled = op === 'clear';
      // 255 means "the first free slot", which only means anything for 'set'.
      // Both servers refuse remove+255 because the firmware core reads it as
      // "wipe this lid", so the option is not offered for a removal at all.
      apnd.disabled = op === 'remove';
      apnd.hidden = op === 'remove';
      if (op === 'remove' && slotSel.value === '255') slotSel.value = '0';
    }
    opSel.addEventListener('change', syncOp);
    syncOp();

    openDlg('Apply across days', nodes, [
      { label: 'Cancel', value: null },
      { label: 'Apply', value: 'go', primary: true, cls: 'btn--primary' }
    ]).then(function (v) {
      if (v !== 'go') return;
      var mask = picker.mask();
      if (!mask) { toast('Select at least one lid', 'bad'); return; }
      var op = opSel.value;
      var slotNum = num(parseInt(slotSel.value, 10), 255);
      // Backstop for the select: a removal needs a real slot, never 255.
      if (op === 'remove' && slotNum > 3) { toast('Pick the dose slot to remove', 'bad'); return; }
      var body = { lidMask: mask, doseIndex: slotNum, op: op };
      if (op === 'set') body.dose = doseBody(form.read());
      var lids = 0;
      for (var b = 0; b < 7; b++) if (mask & (1 << b)) lids++;
      var lidWord = lids + (lids === 1 ? ' lid' : ' lids');
      // 'set' is reversible by editing the dose again; the other two throw
      // pills away on up to seven days at once, so they get asked about.
      var ask = op === 'set' ? Promise.resolve(true) : confirmDlg(
        op === 'clear' ? 'Clear every dose' : 'Remove a dose slot',
        op === 'clear'
          ? 'Deletes every dose on ' + lidWord + '. There is no undo.'
          : 'Deletes dose slot ' + (slotNum + 1) + ' on ' + lidWord + '. There is no undo.',
        op === 'clear' ? 'Clear' : 'Remove', true);
      ask.then(function (okd) {
        if (!okd) return;
        post('/api/schedule/bulk', body, 9000).then(function (j) {
          var n = num(j && j.changed);
          toast(n + (n === 1 ? ' lid updated' : ' lids updated'), 'ok');
          loadSchedule(true);
          schedulePoll(0);
        }, function (e) { toast('Bulk edit failed: ' + e.message, 'bad'); });
      });
    });
  }

  function copyDayDialog() {
    if (!S.sched) return;
    var src = el('select');
    S.sched.lids.forEach(function (L, i) {
      var o = el('option', null, L.name + ' (' + L.doses.length + (L.doses.length === 1 ? ' dose)' : ' doses)'));
      o.value = String(i);
      src.appendChild(o);
    });
    if (devNow) src.value = String(lidForDay(S.sched, dayIdx(nowLocal())));
    var picker = lidPicker(0);
    var nodes = [
      el('p', 'lede', 'Copies every dose of one lid onto the others, replacing whatever they hold now.'),
      field('Copy from', src),
      el('h3', null, 'Copy onto')
    ];
    nodes.push(picker.root);

    openDlg('Copy a day', nodes, [
      { label: 'Cancel', value: null },
      { label: 'Copy', value: 'go', primary: true, cls: 'btn--primary' }
    ]).then(function (v) {
      if (v !== 'go') return;
      var from = num(parseInt(src.value, 10));
      var mask = picker.mask() & ~(1 << from);
      if (!mask) { toast('Select at least one other lid', 'bad'); return; }
      var targets = [];
      for (var ti = 0; ti < 7; ti++) if (mask & (1 << ti)) targets.push(ti);
      commitSchedule(function (s) {
        var doses = JSON.parse(JSON.stringify(s.lids[from].doses));
        targets.forEach(function (i) { s.lids[i].doses = JSON.parse(JSON.stringify(doses)); });
      }, 'Copied day', { lids: targets, doses: true });
    });
  }

  $('btnBulk').addEventListener('click', function () { bulkDialog(null, 255); });
  $('btnCopyDay').addEventListener('click', copyDayDialog);

  $('btnSaveRules').addEventListener('click', function () {
    if (!S.sched) return;
    commitSchedule(function (s) {
      qsa('[data-sck]').forEach(function (inp) {
        s[inp.dataset.sck] = clamp(num(parseInt(inp.value, 10)), 0, 720);
      });
    }, 'Timing rules', { rules: true });
  });

  function fillRules() {
    if (!S.sched) return;
    qsa('[data-sck]').forEach(function (inp) {
      if (document.activeElement !== inp) inp.value = String(num(S.sched[inp.dataset.sck]));
    });
  }

  /* ------------------------------------------------------------ history */

  function normRec(r) {
    r = r || {};
    return {
      scheduledAt: num(r.scheduledAt), actualAt: num(r.actualAt),
      lid: num(r.lid), dose: num(r.dose),
      state: str(r.state) || 'pending', deltaMin: num(r.deltaMin)
    };
  }

  function loadHistory(loud) {
    return getOnce('hist' + S.histDays, '/api/history?days=' + S.histDays, 9000).then(function (j) {
      S.hist = (j && Array.isArray(j.records) ? j.records : []).map(normRec);
      S.hist.sort(function (a, b) { return a.scheduledAt - b.scheduledAt; });
      renderHistory();
      renderTimeline();
    }, function (e) {
      if (loud) toast('Could not read the history: ' + e.message, 'bad');
    });
  }

  function recsByDay() {
    var m = Object.create(null);
    S.hist.forEach(function (r) {
      var d = dayIdx(r.scheduledAt);
      (m[d] = m[d] || []).push(r);
    });
    return m;
  }

  function renderHistStats() {
    var host = $('histStats');
    clear(host);
    var a = (S.st && S.st.adherence) || {};
    function chip(v, lbl) {
      var c = el('div', 'chip');
      c.appendChild(el('b', null, v));
      c.appendChild(el('span', null, lbl));
      host.appendChild(c);
    }
    chip(num(a.pct7d) + '%', 'last 7 days');
    chip(num(a.pct30d) + '%', 'last 30 days');
    chip(num(a.takenToday) + '/' + num(a.dueToday), 'taken today');
    chip(String(num(a.missedToday)), 'missed today');
  }

  function renderHistory() {
    renderHistStats();
    var body = $('calBody');
    clear(body);
    if (!devNow) return;

    var today = dayIdx(nowLocal());
    var first = today - (S.histDays - 1);
    var rowStart = first - wdMon0(first);                // back up to Monday
    var byDay = recsByDay();

    for (var d = rowStart; d <= today; d += 7) {
      var tr = el('tr');
      for (var k = 0; k < 7; k++) {
        var day = d + k;
        var td = el('td');
        td.appendChild(calCell(day, first, today, byDay[day]));
        tr.appendChild(td);
      }
      body.appendChild(tr);
    }

    renderLegend();
    renderHistSummary(first, today, byDay);
    renderHistDetail();
  }

  function dayPips(day, today, recs) {
    // Today and later: start from what is scheduled, so a day half way through
    // shows the doses still to come alongside the ones already judged.
    if (day >= today) {
      var planned = dosesOnDay(day);
      if (planned.length) {
        return planned.map(function (x) {
          var r = recordFor(x.at, x.lid, x.dose);
          return r ? { state: r.state, rec: r } : { state: 'pending', dose: x };
        });
      }
    }
    if (recs && recs.length) {
      return recs.slice().sort(function (a, b) { return a.scheduledAt - b.scheduledAt; })
        .map(function (r) { return { state: r.state, rec: r }; });
    }
    return [];
  }

  function calCell(day, first, today, recs) {
    var inRange = day >= first && day <= today;
    var b = el('button', 'calcell' + (inRange ? '' : ' calcell--out') + (day === today ? ' calcell--today' : ''));
    b.type = 'button';
    var ts = day * DAY;
    b.appendChild(el('span', 'calcell__d', String(D(ts).getUTCDate())));
    var row = el('span', 'calcell__p');
    var pips = inRange ? dayPips(day, today, recs) : [];
    var counts = Object.create(null);
    pips.forEach(function (p) {
      counts[p.state] = num(counts[p.state]) + 1;
      row.appendChild(newPip(p.state, true));
    });
    b.appendChild(row);
    var words = Object.keys(counts).map(function (k) { return counts[k] + ' ' + k; }).join(', ');
    b.setAttribute('aria-label', dateShort(ts) + (words ? ': ' + words : ': nothing recorded'));
    b.setAttribute('aria-pressed', String(S.histSel === day));
    if (!inRange) { b.disabled = true; return b; }
    b.addEventListener('click', function () {
      S.histSel = (S.histSel === day) ? -1 : day;
      qsa('.calcell', $('calBody')).forEach(function (c) { c.setAttribute('aria-pressed', 'false'); });
      b.setAttribute('aria-pressed', String(S.histSel === day));
      renderHistDetail();
    });
    return b;
  }

  function renderLegend() {
    var host = $('legend');
    clear(host);
    ['taken', 'late', 'early', 'pending', 'missed', 'skipped'].forEach(function (s) {
      var li = el('li');
      li.appendChild(newPip(s, true));
      li.appendChild(document.createTextNode(pipInfo(s)[2]));
      host.appendChild(li);
    });
  }

  function renderHistSummary(first, today, byDay) {
    var c = { taken: 0, late: 0, early: 0, missed: 0, skipped: 0, pending: 0 };
    var total = 0, emptyDays = 0;
    for (var day = first; day <= today; day++) {
      var recs = byDay[day];
      var pips = dayPips(day, today, recs);
      if (!pips.length) emptyDays++;
      pips.forEach(function (p) {
        if (c[p.state] === undefined) c[p.state] = 0;
        c[p.state]++;
        total++;
      });
    }
    var days = today - first + 1;
    var parts = [];
    if (c.taken) parts.push(c.taken + ' on time');
    if (c.late) parts.push(c.late + ' late');
    if (c.early) parts.push(c.early + ' early');
    if (c.missed) parts.push(c.missed + ' missed');
    if (c.skipped) parts.push(c.skipped + ' skipped');
    if (c.pending) parts.push(c.pending + ' still to come');

    var txt;
    if (!total) {
      txt = 'No doses are recorded over the last ' + days + ' days.';
    } else {
      txt = 'Over the last ' + days + ' days the box recorded ' + total +
        (total === 1 ? ' dose: ' : ' doses: ') + listWords(parts) + '.';
      var good = c.taken + c.late + c.early;
      var judged = total - c.pending - c.skipped;
      if (judged > 0) {
        txt += ' Of the ' + judged + ' that came due, ' +
          Math.round(good / judged * 100) + '% were opened.';
      }
      if (emptyDays) txt += ' ' + emptyDays + (emptyDays === 1 ? ' day has' : ' days have') + ' nothing recorded.';
    }
    $('histSummary').textContent = txt;
  }

  function listWords(a) {
    if (a.length <= 1) return a.join('');
    return a.slice(0, -1).join(', ') + ' and ' + a[a.length - 1];
  }

  function renderHistDetail() {
    var host = $('histDetail');
    clear(host);
    if (S.histSel < 0) { show(host, false); return; }
    show(host, true);
    var day = S.histSel, today = devNow ? dayIdx(nowLocal()) : day;
    host.appendChild(el('h3', null, dateShort(day * DAY)));
    var byDay = recsByDay();
    var pips = dayPips(day, today, byDay[day]);
    if (!pips.length) {
      host.appendChild(el('p', 'muted', 'Nothing recorded for this day.'));
      return;
    }
    var ul = el('ul');
    pips.forEach(function (p) {
      var li = el('li');
      var at = p.rec ? p.rec.scheduledAt : p.dose.at;
      var lid = p.rec ? p.rec.lid : p.dose.lid;
      var dose = p.rec ? p.rec.dose : p.dose.dose;
      li.appendChild(newPip(p.state, true));
      li.appendChild(el('span', 'mono', hhmm(at)));
      var g = el('span', 'grow');
      var lidName = (S.sched && S.sched.lids[lid]) ? S.sched.lids[lid].name : ('Lid ' + (lid + 1));
      g.appendChild(el('b', null, lidName + ' · dose ' + (dose + 1)));
      var sub = pipInfo(p.state)[2];
      if (p.rec && p.rec.actualAt) sub += ', opened ' + hhmm(p.rec.actualAt);
      if (p.rec && p.rec.deltaMin) sub += ' (' + (p.rec.deltaMin > 0 ? '+' : '') + p.rec.deltaMin + ' min)';
      g.appendChild(el('small', null, sub));
      li.appendChild(g);

      if (p.state === 'pending' || p.state === 'missed') {
        li.appendChild(markBtn('markTaken', 'Mark taken', at, lid, dose));
        li.appendChild(markBtn('markSkipped', 'Skip', at, lid, dose));
      }
      ul.appendChild(li);
    });
    host.appendChild(ul);
  }

  function markBtn(action, label, at, lid, dose) {
    var b = el('button', 'btn btn--tiny', label);
    b.type = 'button';
    b.addEventListener('click', function () {
      b.disabled = true;
      post('/api/action', { action: action, scheduledAt: at, lid: lid, dose: dose }, 8000).then(function () {
        toast(label + ' recorded', 'ok');
        loadHistory(true);
        schedulePoll(0);
      }, function (e) { b.disabled = false; toast('Failed: ' + e.message, 'bad'); });
    });
    return b;
  }

  $('histDays').addEventListener('change', function () {
    S.histDays = num(parseInt($('histDays').value, 10), 14);
    S.histSel = -1;
    loadHistory(true);
  });
  $('histReload').addEventListener('click', function () { loadHistory(true); });

  /* ----------------------------------------------------------- settings */

  function pathGet(o, p) {
    var parts = p.split('.');
    for (var i = 0; i < parts.length && o != null; i++) o = o[parts[i]];
    return o;
  }
  function pathSet(o, p, v) {
    var parts = p.split('.');
    for (var i = 0; i < parts.length - 1; i++) {
      var k = parts[i], nextNum = /^\d+$/.test(parts[i + 1]);
      if (o[k] === undefined || o[k] === null || typeof o[k] !== 'object') o[k] = nextNum ? [] : {};
      o = o[k];
    }
    o[parts[parts.length - 1]] = v;
  }

  function buildHwWidgets() {
    var inv = $('hInv');
    if (!inv.firstChild) {
      for (var i = 0; i < 7; i++) {
        var lab = el('label');
        var cb = el('input'); cb.type = 'checkbox'; cb.dataset.bit = String(i);
        lab.appendChild(cb);
        lab.appendChild(el('span', null, 'Hall ' + (i + 1)));
        inv.appendChild(lab);
      }
    }
    var map = $('hMap');
    if (!map.firstChild) {
      for (var k = 0; k < 7; k++) {
        var d = el('div');
        var sel = el('select');
        sel.dataset.k = 'hw.hallToLid.' + k;
        sel.dataset.t = 'n';
        sel.id = 'hMap' + k;
        for (var j = 0; j < 7; j++) {
          var o = el('option', null, LIDNAME[j]); o.value = String(j); sel.appendChild(o);
        }
        var l = el('label', null, 'Sensor ' + (k + 1) + ' →');
        l.htmlFor = sel.id;
        d.appendChild(l);
        d.appendChild(sel);
        map.appendChild(d);
      }
    }
    qsa('[data-opts="ch"]').forEach(function (sel) {
      if (sel.firstChild) return;
      for (var c = 0; c < 4; c++) {
        var o = el('option', null, 'Channel ' + c); o.value = String(c); sel.appendChild(o);
      }
    });
  }

  function fillForm() {
    var s = S.settings;
    if (!s) return;
    buildHwWidgets();
    qsa('[data-k]').forEach(function (inp) {
      var v = pathGet(s, inp.dataset.k);
      if (inp.type === 'checkbox') { inp.checked = v === true; return; }
      if (inp.dataset.secret !== undefined) {
        var isSet = v === '__set__';
        inp.value = '';
        inp.placeholder = isSet ? 'saved — type to replace' : 'not set';
        inp.classList.toggle('is-set', isSet);
        delete inp.dataset.dirty;
        delete inp.dataset.cleared;
        return;
      }
      if (inp.dataset.t === 'min') { inp.value = minToHhmm(num(v)); return; }
      inp.value = (v === undefined || v === null) ? '' : String(v);
    });
    var mask = num(pathGet(s, 'hw.hallInvertMask'));
    qsa('[data-bit]').forEach(function (cb) { cb.checked = (mask & (1 << +cb.dataset.bit)) !== 0; });

    show($('advWrap'), bool(pathGet(s, 'ui.showAdvanced')));
    $('hostEcho').textContent = str(pathGet(s, 'wifi.hostname')) || 'easypill';
    renderTimeRo();
    renderSubUrl();
    renderQuiet();
    renderHeader();
  }

  function collectPatch() {
    var out = {};
    qsa('[data-k]').forEach(function (inp) {
      var k = inp.dataset.k;
      if (inp.type === 'checkbox') { pathSet(out, k, inp.checked); return; }
      if (inp.dataset.secret !== undefined) {
        // The "__set__" protocol: a secret we were never shown is never echoed
        // back. Only a value the user actually typed, or an explicit clear.
        if (inp.dataset.cleared === '1') pathSet(out, k, '');
        else if (inp.dataset.dirty === '1' && inp.value !== '') pathSet(out, k, inp.value);
        return;
      }
      if (inp.dataset.t === 'min') { pathSet(out, k, hhmmToMin(inp.value)); return; }
      if (inp.dataset.t === 'n' || inp.type === 'number') {
        pathSet(out, k, clamp(num(parseFloat(inp.value)), -2147483648, 2147483647));
        return;
      }
      pathSet(out, k, inp.value);
    });
    var mask = 0;
    qsa('[data-bit]').forEach(function (cb) { if (cb.checked) mask |= (1 << +cb.dataset.bit); });
    pathSet(out, 'hw.hallInvertMask', mask);
    return out;
  }

  function markDirty(on) {
    S.dirty = on;
    show($('savebar'), on);
  }

  function onFormInput(e) {
    var t = e.target;
    if (!t || !t.dataset) return;
    if (t.dataset.secret !== undefined) {
      t.dataset.dirty = '1';
      delete t.dataset.cleared;
      t.placeholder = 'will be saved';
    }
    if (t.dataset.k) {
      // The same setting appears in two places (channel enables), so keep the
      // twins in step or the one the user did not touch would overwrite them.
      qsa('[data-k="' + t.dataset.k + '"]').forEach(function (o) {
        if (o === t) return;
        if (o.type === 'checkbox') o.checked = t.checked; else o.value = t.value;
      });
    }
    if (t.dataset.k === 'wifi.hostname') $('hostEcho').textContent = t.value || 'easypill';
    if (t.dataset.k === 'ui.showAdvanced') show($('advWrap'), t.checked);
    if (t.dataset.k === 'notify.ntfyTopic' || t.dataset.k === 'notify.ntfyServer') renderSubUrl();
    if (t.dataset.k === 'notify.quietFromMinute' || t.dataset.k === 'notify.quietToMinute') renderQuiet();
    markDirty(true);
  }

  ['input', 'change'].forEach(function (evt) {
    $('v-settings').addEventListener(evt, onFormInput);
    $('v-notify').addEventListener(evt, onFormInput);
  });

  qsa('[data-clear]').forEach(function (b) {
    b.addEventListener('click', function () {
      var inp = $(b.dataset.clear);
      if (!inp) return;
      inp.value = '';
      inp.dataset.cleared = '1';
      delete inp.dataset.dirty;
      inp.placeholder = 'will be cleared on save';
      inp.classList.remove('is-set');
      markDirty(true);
    });
  });

  // A truncated reply is what heap pressure on the box looks like from here, and
  // a tree missing a section would be painted into the form as blanks and then
  // posted straight back over the hall calibration. Nothing short of the whole
  // tree counts as loaded.
  var SETTINGS_SECTIONS = ['ui', 'wifi', 'time', 'notify', 'power', 'hw'];
  function settingsComplete(j) {
    if (!j || typeof j !== 'object') return false;
    for (var i = 0; i < SETTINGS_SECTIONS.length; i++) {
      var sec = j[SETTINGS_SECTIONS[i]];
      if (!sec || typeof sec !== 'object') return false;
    }
    return true;
  }

  function loadSettings(force) {
    if (S.dirty && !force) return Promise.resolve();
    return getOnce('settings', '/api/settings', 8000).then(function (j) {
      if (!settingsComplete(j)) {
        // Leave S.settings as it was: a good tree from an earlier read is worth
        // more than this one, and null keeps Save refusing to post blanks.
        toast('The box sent an incomplete settings reply, so nothing was loaded. Try again.', 'bad');
        return;
      }
      S.settings = j;
      fillForm();
      markDirty(false);
    }, function (e) { toast('Could not read the settings: ' + e.message, 'bad'); });
  }

  $('btnSave').addEventListener('click', function () {
    var b = $('btnSave');
    if (!S.settings) {
      // Without a successful GET the form holds no real values, and posting it
      // would write blanks over everything the box has.
      toast('The settings have not loaded yet, so there is nothing safe to save.', 'bad');
      return;
    }
    b.disabled = true;
    $('saveMsg').textContent = 'Saving…';
    post('/api/settings', collectPatch(), 10000).then(function (j) {
      b.disabled = false;
      $('saveMsg').textContent = 'Unsaved changes';
      if (settingsComplete(j)) { S.settings = j; fillForm(); }
      markDirty(false);
      toast(j && j.changed ? 'Saved. Some values were clamped to what the box accepts.' : 'Settings saved', 'ok');
      schedulePoll(0);
    }, function (e) {
      b.disabled = false;
      $('saveMsg').textContent = 'Not saved';
      toast('Could not save: ' + e.message, 'bad');
    });
  });

  $('btnDiscard').addEventListener('click', function () {
    markDirty(false);
    loadSettings(true);
  });

  $('btnGoNotify').addEventListener('click', function () { setView('notify'); });

  function renderTimeRo() {
    var host = $('timeRo');
    clear(host);
    var t = (S.settings && S.settings.time) || {};
    var st = S.st || {};
    var ppm = num(t.driftPpm);
    function row(k, v, title) {
      var d = el('div');
      d.appendChild(el('dt', null, k));
      var dd = el('dd', null, v);
      if (title) dd.title = title;
      d.appendChild(dd);
      host.appendChild(d);
    }
    row('Clock', bool(st.timeVerified) ? 'checked against NTP' : 'not verified yet',
      'Lid events recorded before the first sync are marked, never given an invented wall-clock time.');
    row('Device time', devNow ? stamp(nowLocal()) : '—');
    row('UTC offset', (num(st.tzOffsetSec) / 3600).toFixed(2).replace(/\.00$/, '') + ' h');
    row('Measured drift', ppm + ' ppm', 'Learned by comparing elapsed RTC seconds with elapsed NTP seconds.');
    row('That works out to', (Math.abs(ppm) * 86400 / 1e6).toFixed(1) + ' s per day ' + (ppm < 0 ? 'slow' : 'fast'));
    row('Uptime', dhms(num(st.uptimeS)));
  }

  /* -------------------------------------------------------- wifi + scan */

  $('btnScan').addEventListener('click', function () {
    var b = $('btnScan'), list = $('netList');
    b.disabled = true;
    qs('span', b).textContent = 'Scanning…';
    clear(list);
    show(list, true);
    list.appendChild(el('p', 'muted', 'Scanning takes a few seconds; the box stops answering while the radio sweeps.'));
    get('/api/scan', 20000).then(function (j) {
      b.disabled = false;
      qs('span', b).textContent = 'Scan';
      clear(list);
      var nets = (j && Array.isArray(j.networks)) ? j.networks : [];
      if (!nets.length) { list.appendChild(el('p', 'muted', 'No networks found.')); return; }
      nets.sort(function (x, y) { return num(y.rssi) - num(x.rssi); });
      nets.forEach(function (n) {
        var btn = el('button', 'btn');
        btn.type = 'button';
        btn.appendChild(icon(bool(n.secure) ? 'lock' : 'wifi'));
        btn.appendChild(el('span', null, str(n.ssid) || '(hidden)'));
        btn.appendChild(el('span', 'rssi', num(n.rssi) + ' dBm'));
        btn.addEventListener('click', function () {
          $('wSsid').value = str(n.ssid);
          onFormInput({ target: $('wSsid') });
          show(list, false);
          try { $('wPass').focus(); } catch (e) { /* ignore */ }
        });
        list.appendChild(btn);
      });
    }, function (e) {
      b.disabled = false;
      qs('span', b).textContent = 'Scan';
      clear(list);
      list.appendChild(el('p', 'muted', 'Scan failed: ' + e.message));
    });
  });

  $('btnConnect').addEventListener('click', function () {
    var inp = $('wPass');
    var ssid = $('wSsid').value.trim();
    if (!ssid) { toast('Enter a network name first', 'bad'); return; }
    // The same "__set__" protocol collectPatch() uses: a secret the GUI was
    // never shown is not echoed back. Both servers swap "__set__" for the
    // stored password and both accept "" for an open network.
    var storedSsid = str(pathGet(S.settings || {}, 'wifi.ssid'));
    var hasStored = pathGet(S.settings || {}, 'wifi.pass') === '__set__';
    var pass;
    if (inp.dataset.cleared === '1') pass = '';                   // explicitly cleared
    else if (inp.dataset.dirty === '1') pass = inp.value;         // actually typed
    else if (hasStored && ssid === storedSsid) pass = '__set__';  // nudge a reconnect
    else pass = '';                                               // nothing to send
    var go = function () {
      post('/api/wifi', { ssid: ssid, pass: pass }, 9000).then(function () {
        toast('Joining ' + ssid + '. Watch the Wi-Fi chip in the status strip.', 'ok');
        schedulePoll(1500);
      }, function (e) { toast('Could not start the join: ' + e.message, 'bad'); });
    };
    // Whatever is sent here is stored, so a blank replaces the saved password -
    // and if the open network then does not come up, the credentials for the
    // real one are gone with it.
    if (pass === '') {
      confirmDlg('Join without a password',
        '"' + ssid + '" will be joined as an open network. If it does have a password, type it first: the box stores whatever is sent here, and a blank one replaces the saved password.',
        'Join anyway').then(function (ok) { if (ok) go(); });
    } else go();
  });

  /* ------------------------------------------------------------ actions */

  var ACTS = {
    beep: { body: { action: 'beep', onMs: 120, repeats: 2 } },
    led: { body: { action: 'led', r: 0, g: 160, b: 255, ms: 1500 }, timeout: 8000 },
    identifyLed: { body: { action: 'identifyLed' }, timeout: 15000, note: 'Watch the LED: it drives channel 0, then 1, then 2 for a second and a half each. Note which colour lit for each and set them below.' },
    syncTime: { body: { action: 'syncTime' }, timeout: 15000 },
    testNotify: { body: { action: 'testNotify', target: 'all' }, timeout: 20000 },
    calibrateHalls: {
      body: { action: 'calibrateHalls' }, timeout: 15000,
      confirm: ['Calibrate the lid sensors',
        'Close all seven lids before you run this. The box learns which way the sensors read from what it sees right now, so a lid left open teaches it the wrong polarity.',
        'Every lid is closed — calibrate']
    },
    clearEvents: { body: { action: 'clearEvents' }, confirm: ['Clear the event log', 'Deletes every recorded event. The dose history is not touched.', 'Clear the log', true] },
    clearHistory: { body: { action: 'clearHistory' }, confirm: ['Clear the dose history', 'Deletes every dose record, so the adherence figures start again from zero. The schedule is not touched.', 'Clear the history', true] },
    reboot: { body: { action: 'reboot' }, confirm: ['Reboot the box', 'The box restarts. It will be unreachable for a few seconds and the clock has to re-sync.', 'Reboot', true], expectLoss: true },
    powerOff: { body: { action: 'powerOff' }, confirm: ['Power off', 'Drops the power latch. On battery the box switches off completely and somebody has to press the button to bring it back. On USB it cannot actually stay off.', 'Power off', true], expectLoss: true },
    factoryReset: {
      body: { action: 'factoryReset', confirm: true }, timeout: 12000, expectLoss: true,
      confirm: ['Factory reset', 'Erases the Wi-Fi credentials, the schedule, the notification setup and every hardware tweak, then reboots into the defaults. This cannot be undone.', 'Erase everything', true]
    }
  };

  function showActOut(node, obj) {
    show(node, true);
    node.textContent = typeof obj === 'string' ? obj : JSON.stringify(obj, null, 2);
  }

  function runAction(name, out, body) {
    var def = ACTS[name];
    if (!def) return;
    var fire = function () {
      var o = out || $('actOut');
      showActOut(o, 'Running ' + name + '…');
      post('/api/action', body || def.body, def.timeout || 8000).then(function (j) {
        var res = (j && j.result !== undefined) ? j.result : j;
        showActOut(o, (def.note ? def.note + '\n\n' : '') + (res === undefined || res === null ? 'done' : (typeof res === 'string' ? res : JSON.stringify(res, null, 2))));
        toast(name + ' done', 'ok');
        if (name === 'clearEvents') { S.seen = Object.create(null); clear($('feed')); }
        if (name === 'clearHistory') { S.hist = []; renderHistory(); renderTimeline(); }
        if (name === 'calibrateHalls' || name === 'factoryReset') loadSettings(true);
        schedulePoll(def.expectLoss ? 4000 : 0);
      }, function (e) {
        if (def.expectLoss) {
          showActOut(o, 'The box stopped answering, which is what a ' + name + ' looks like from here. The GUI will reconnect by itself if it comes back.');
          schedulePoll(4000);
        } else {
          showActOut(o, 'Failed: ' + e.message);
          toast(name + ' failed: ' + e.message, 'bad');
        }
      });
    };
    if (def.confirm) {
      confirmDlg(def.confirm[0], def.confirm[1], def.confirm[2], def.confirm[3] === true)
        .then(function (ok) { if (ok) fire(); });
    } else fire();
  }

  qsa('[data-act]').forEach(function (b) {
    b.addEventListener('click', function () { runAction(b.dataset.act, $('actOut')); });
  });

  /* ------------------------------------------------------ notifications */

  function renderSubUrl() {
    var server = ($('nServer').value || 'https://ntfy.sh').replace(/\/+$/, '');
    var topic = $('nTopic').value.trim();
    $('subUrl').textContent = topic ? server + '/' + topic : 'pick a topic first';
  }

  $('btnTopic').addEventListener('click', function () {
    var abc = 'abcdefghijkmnopqrstuvwxyz23456789';   // no l, 0 or 1: they get mistyped
    var n = 8, out = 'easypill-';
    if (window.crypto && crypto.getRandomValues) {
      var buf = new Uint8Array(n);
      crypto.getRandomValues(buf);
      for (var i = 0; i < n; i++) out += abc.charAt(buf[i] % abc.length);
    } else {
      for (var j = 0; j < n; j++) out += abc.charAt(Math.floor(Math.random() * abc.length));
    }
    $('nTopic').value = out;
    onFormInput({ target: $('nTopic') });
    renderSubUrl();
    toast('Topic generated. Save it, then subscribe the app to exactly this name.', 'ok', 6000);
  });

  $('btnCopyUrl').addEventListener('click', function () {
    var txt = $('subUrl').textContent;
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(txt).then(function () { toast('Copied', 'ok'); },
        function () { toast('Copy refused by the browser — select the address by hand', 'bad'); });
    } else {
      toast('This browser will not copy for us — select the address by hand', 'bad');
    }
  });

  function wireTest(btnId, target) {
    $(btnId).addEventListener('click', function () {
      if (S.dirty) { toast('Save first — the box sends the test with what it has stored, not what is on screen.', 'bad'); return; }
      runAction('testNotify', $('notifyOut'), { action: 'testNotify', target: target });
    });
  }
  wireTest('btnTestNtfy', 'ntfy');
  wireTest('btnTestPo', 'pushover');
  wireTest('btnTestWh', 'webhook');

  function renderQuiet() {
    var f = hhmmToMin($('qFrom').value), t = hhmmToMin($('qTo').value);
    $('quietState').textContent = (f === t)
      ? 'Quiet hours are off: notifications are sent at any time of day.'
      : 'Nothing is sent between ' + minToHhmm(f) + ' and ' + minToHhmm(t) +
        (t < f ? ' the next morning.' : '.');
  }

  /* --------------------------------------------------------------- info */

  function loadInfo() {
    return get('/api/info', 8000).then(function (j) {
      S.info = j || {};
      var host = $('infoRo');
      clear(host);
      var rows = [
        ['Firmware', str(j.fw)], ['Built', str(j.buildDate)], ['Chip', str(j.chip)],
        ['Cores', String(num(j.cores))], ['Flash', num(j.flashMb) + ' MB'],
        ['Free heap', Math.round(num(j.heapFree) / 1024) + ' kB'],
        ['Heap low mark', Math.round(num(j.heapMin) / 1024) + ' kB'],
        ['MAC', str(j.macAddress)], ['Reset reason', str(j.resetReason)],
        ['Wake reason', str(j.wakeReason)], ['SDK', str(j.sdkVersion)],
        ['Filesystem', Math.round(num(j.fsUsedBytes) / 1024) + ' of ' + Math.round(num(j.fsTotalBytes) / 1024) + ' kB']
      ];
      rows.forEach(function (r) {
        if (!r[1]) return;
        var d = el('div');
        d.appendChild(el('dt', null, r[0]));
        d.appendChild(el('dd', null, r[1]));
        host.appendChild(d);
      });
      $('footFw').textContent = 'EasyPill ' + (str(j.fw) || '') + (str(j.buildDate) ? ' · built ' + str(j.buildDate) : '');
      $('footChip').textContent = str(j.chip) + (num(j.flashMb) ? ', ' + num(j.flashMb) + ' MB flash' : '');
    }, function () { /* the footer is decoration; a failure here is not worth a toast */ });
  }

  /* --------------------------------------------------------------- tabs */

  var VIEWS = ['now', 'schedule', 'history', 'settings', 'notify'];

  function setView(name) {
    if (VIEWS.indexOf(name) < 0) name = 'now';
    S.view = name;
    VIEWS.forEach(function (v) {
      var tab = $('tab-' + v), pane = $('v-' + v);
      var on = v === name;
      tab.setAttribute('aria-selected', String(on));
      tab.tabIndex = on ? 0 : -1;
      show(pane, on);
    });
    if (location.hash !== '#' + name) {
      try { history.replaceState(null, '', '#' + name); } catch (e) { location.hash = name; }
    }
    if (name === 'schedule') { if (!S.sched) loadSchedule(false); else { renderSchedule(); fillRules(); } }
    if (name === 'history') { renderHistory(); if (!S.hist.length) loadHistory(false); }
    if (name === 'settings' || name === 'notify') { if (!S.settings) loadSettings(false); }
    window.scrollTo(0, 0);
  }

  qsa('.tab').forEach(function (t) {
    t.addEventListener('click', function () { setView(t.dataset.view); });
    t.addEventListener('keydown', function (e) {
      var i = VIEWS.indexOf(t.dataset.view), j = -1;
      if (e.key === 'ArrowRight') j = (i + 1) % VIEWS.length;
      else if (e.key === 'ArrowLeft') j = (i + VIEWS.length - 1) % VIEWS.length;
      else if (e.key === 'Home') j = 0;
      else if (e.key === 'End') j = VIEWS.length - 1;
      if (j < 0) return;
      e.preventDefault();
      setView(VIEWS[j]);
      $('tab-' + VIEWS[j]).focus();
    });
  });
  window.addEventListener('hashchange', function () {
    setView(location.hash.replace(/^#/, ''));
  });
  window.addEventListener('beforeunload', function (e) {
    if (!S.dirty) return;
    e.preventDefault();
    e.returnValue = '';
  });

  /* --------------------------------------------------------------- tick */

  function tick() {
    if (devNow) {
      renderStrip();
      updateCountdown();
      if (S.view === 'settings' && S.settings) renderTimeRo();
    }
    refreshEvTimes();
    updateBanner();
  }
  setInterval(tick, 1000);

  /* --------------------------------------------------------------- boot */

  setView((location.hash || '#now').replace(/^#/, ''));
  poll();
  loadSettings(false);
  loadSchedule(false);
  loadInfo();

}());
