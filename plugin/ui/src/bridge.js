/* Auto Malign plugin bridge: connects the HTML interface to the plugin when it runs inside the
   plugin window (JUCE WebBrowserComponent with native integration). In a normal browser this does nothing. */
// ---- scale to fit (always on in the plugin build): the layout keeps its full design width and the whole interface
// scales uniformly to the window. No scrolling, ever.
(function () {
  const DW = 1376, DH = 1118;   // .plugin (1340 x 1082 px) + margins; the main page is the tallest view
  const plug = document.querySelector('.plugin');
  let lastScale = 0;
  const fit = () => {
    const s = Math.max(0.2, Math.min(innerWidth / DW, innerHeight / DH));
    plug.style.transform = `translateX(-50%) scale(${s})`;
    plug.style.top = `${Math.max(0, (innerHeight - DH * s) / 2) + 18 * s}px`;   // margins scale too; centred vertically
    scrollTo(0, 0);
    if (Math.abs(s - lastScale) > 1e-3) {
      lastScale = s;
      requestAnimationFrame(() => { resizeView(); FX.forEach(f => f.size && f.size()); });
    }
  };
  addEventListener('resize', fit);
  addEventListener('scroll', () => scrollTo(0, 0));
  new ResizeObserver(fit).observe(document.documentElement);
  fit();
})(0);

// ---- connection to the plugin. JUCE injects window.__JUCE__; wait for it briefly in case it arrives after the page.
(function connect(tries) {
  const J = window.__JUCE__;
  if (!J || !J.backend) { if (tries < 100) setTimeout(() => connect(tries + 1), 50); return; }

  let applying = 0;
  // Controls the user is touching ignore the plugin's echo for a moment, so a slightly stale value coming back
  // from the plugin never drags a knob backwards mid-turn (that tug felt like lag).
  const touched = {};
  const fresh = id => performance.now() - (touched[id] || 0) < 450;
  const send = (id, v) => { if (!applying) { touched[id] = performance.now(); J.backend.emitEvent('param', { id, v }); } };
  const act = (name, extra) => J.backend.emitEvent('action', Object.assign({ name }, extra || {}));

  // ---- UI -> plugin
  main.sub(v => send('alignment', Math.round((v - 0.5) * 2000) / 10)); // knob 0..1 -> -100 (align) .. +100 (malign)
  for (const id of ['match', 'width', 'motion', 'mix']) MAC[id].sub(v => send(id, v));
  document.querySelectorAll('.flavor button').forEach(b => b.addEventListener('click', () => send('flavour', b.dataset.flv)));
  for (const id of ['ab', 'delta']) {
    const b = document.getElementById(id);
    b.addEventListener('click', () => send(id, b.getAttribute('aria-pressed') === 'true'));
  }
  document.querySelectorAll('#scope button').forEach(b => b.addEventListener('click', () => send('scope', b.dataset.v)));

  // Only the plugin window you're using animates. Any interaction here claims it; the plugin tells the others to idle.
  let lastClaim = 0;
  const claim = () => { const t = performance.now(); if (t - lastClaim > 250) { lastClaim = t; act('activate'); } };
  for (const ev of ['pointerdown', 'pointermove', 'keydown', 'wheel', 'focus']) addEventListener(ev, claim, { passive: true });

  // Pro > Alignment: latency before this track (read from Live, or typed in), and the Live script installer
  const pf = k => PRO[0].fields.find(f => f.k === k);
  pf('latAuto').on = v => send('latAuto', v);
  pf('latMs').on = v => send('latMs', v);
  pf('installLive').on = () => act('installLive');

  // Pro page: per-copy controls (linked like the macros in Session scope) and the session-wide analysis settings
  const PF = k => { for (const list of [...PRO.map(sec => sec.fields || []), ...Object.values(REFINE)]) for (const f of list) if (f.k === k) return f; return null; };
  for (const k of ['famt', 'pmono', 'srot']) PF(k).on = v => send(k, v);
  for (const k of ['loHz', 'hiHz', 'smooth', 'stiff', 'guard']) PF(k).on = v => send(k, v);
  // Malodic notes (Refine flavour > MALODICS)
  for (const k of ['ppmode', 'ppassist', 'pppurity', 'ppcarrier', 'ppstrength', 'ppfold', 'ppfill', 'pptame', 'ppconst', 'pppartials', 'ppattack', 'pprelease', 'ppglide', 'ppbend'])
    PF(k).on = v => send(k, v);
  let ppSourceIds = [];                                   // select index >= 2 -> session track id
  PF('ppsource').on = v => {
    if (v < 2) send('ppsource', v);
    else { send('ppSourceTrack', ppSourceIds[v - 2] ?? -1); send('ppsource', 2); }
  };
  let anchorIds = [-1];                                   // select index -> track id (-1 = automatic)
  PF('anchor').on = v => send('anchor', anchorIds[v] ?? -1);
  // reflect a value into a Pro field (and its control, if that section is on screen)
  const showField = (k, v) => {
    const f = PF(k); if (!f || fresh(k)) return;
    const el = document.getElementById('pf-' + k);
    if (f.t === 'range') { if (Math.abs(f.v - v) < 1e-6) return; f.v = v; if (el && document.activeElement !== el) { el.value = v; el.dispatchEvent(new Event('input')); } }
    else if (f.t === 'select') { if (f.v === v) return; f.v = v; if (el) el.value = v; }
    else { if (f.v === !!v) return; f.v = !!v; if (el) el.checked = !!v; }
  };

  // Learn: one button in the status bar drives the whole session
  const learn = document.getElementById('learn');
  learn.addEventListener('click', () => act('learn'));

  // ---- plugin -> UI
  const cap = document.querySelector('.pyrbox .cap');
  const sig = document.getElementById('sig');
  let wasLearning = null, wasSolved = null, lastMsg = '';
  const press = (sel, on) => { const b = document.querySelector(sel); if (b) b.setAttribute('aria-pressed', on ? 'true' : 'false'); };

  J.backend.addEventListener('state', s => {
    applying++;
    try {
      const idle = s.active === false;
      if (idle !== !!S.idle) { S.idle = idle; S.dirty = true; }   // paused windows show a still frame
      const kv = 0.5 + s.alignment / 200;
      if (!fresh('alignment') && Math.abs(main.get() - kv) > 0.0015) main.set(kv);
      for (const id of ['match', 'width', 'motion', 'mix'])
        if (!fresh(id) && Math.abs(MAC[id].get() - s[id]) > 0.0015) MAC[id].set(s[id]);
      if (!fresh('flavour') && P.flavor !== s.flavour) {
        const b = document.querySelector(`.flavor button[data-flv="${s.flavour}"]`);
        if (b) b.click();
      }
      for (const id of ['ab', 'delta']) {
        const b = document.getElementById(id);
        if ((b.getAttribute('aria-pressed') === 'true') !== !!s[id]) { b.setAttribute('aria-pressed', s[id] ? 'true' : 'false'); P[id] = !!s[id]; }
      }
      document.querySelectorAll('#scope button').forEach(b => b.setAttribute('aria-pressed', b.dataset.v === s.scope ? 'true' : 'false'));

      // latency before this track
      const la = pf('latAuto'), lm = pf('latMs');
      la.v = !!s.latAuto;
      if (!fresh('latMs')) lm.v = s.latMs;
      const cb = document.getElementById('pf-latAuto');
      if (cb) {
        cb.checked = la.v;
        const hint = cb.closest('.field')?.querySelector('.hint');
        if (hint) hint.textContent = s.latSource === 'live' ? `Live reports ${s.latUsedMs.toFixed(1)} ms in front of Auto Malign on "${s.latTrack}".`
          : s.latSource === 'manual' ? `Using your manual value: ${s.latUsedMs.toFixed(1)} ms.`
          : s.liveSeen ? 'Live is connected, but this instance was not found yet. Keep the transport running a moment.'
          : 'Live control surface not detected. Install it below, or type the latency manually.';
      }
      const rg = document.getElementById('pf-latMs');
      if (rg && !fresh('latMs') && document.activeElement !== rg) { rg.value = lm.v; rg.dispatchEvent(new Event('input')); }
      if (s.msg && s.msg !== lastMsg) { lastMsg = s.msg; toast(s.msgOk ? 'ok' : 'warn', s.msg); }

      // Malodic notes
      if (s.phantom) {
        const p = s.phantom;
        showField('ppmode', p.mode); showField('ppassist', p.assist); showField('pppurity', p.purity); showField('ppcarrier', p.carrier);
        showField('ppstrength', p.strength); showField('ppfold', p.fold); showField('ppfill', p.fill); showField('pptame', p.tame); showField('ppconst', p.constant); showField('pppartials', p.partials);
        showField('ppattack', p.attack); showField('pprelease', p.release); showField('ppglide', p.glide); showField('ppbend', p.bend);
        // source list: this track, sidechain, then every other track with Auto Malign
        const others = (s.tracks || []).filter(t => !t.self);
        const ids = others.map(t => t.id), opts = ['This track + noise', 'Sidechain input' + (p.sidechain ? '' : ' (off in the host)'), ...others.map(t => 'Track: ' + (t.name || 'Track'))];
        const f = PF('ppsource'), el = document.getElementById('pf-ppsource');
        if (JSON.stringify(opts) !== JSON.stringify(f.opts) || JSON.stringify(ids) !== JSON.stringify(ppSourceIds)) {
          f.opts = opts; ppSourceIds = ids;
          if (el) { el.textContent = ''; opts.forEach((o, i) => { const op = document.createElement('option'); op.value = i; op.textContent = o; el.appendChild(op); }); }
        }
        const idx = p.source < 2 ? p.source : Math.max(0, 2 + ids.indexOf(p.sourceTrack));
        if (!fresh('ppsource')) { f.v = idx; if (el) el.value = idx; }
        const info = document.getElementById('pf-ppnotes');
        if (info) info.textContent = (p.notes && p.notes.length)
          ? 'Sounding: ' + p.notes.map(noteName).join('  ')
          : 'Play MIDI into this track. Live: on a MIDI track set MIDI To > this track > Auto Malign. Best on headphones.';
      }

      // Pro page values
      for (const k of ['famt', 'pmono', 'srot']) if (k in s) showField(k, s[k]);
      if (s.analysis) {
        const a = s.analysis;
        showField('loHz', a.loHz); showField('hiHz', a.hiHz); showField('smooth', a.smooth);
        showField('stiff', a.stiff); showField('guard', a.guard);
        // anchor choices: Auto + every track Learn listens to
        const list = (s.tracks || []).filter(t => !t.excluded);
        const ids = [-1, ...list.map(t => t.id)], opts = ['Auto (loudest low end)', ...list.map(t => t.name || 'Track')];
        const f = PF('anchor'), el = document.getElementById('pf-anchor');
        if (JSON.stringify(opts) !== JSON.stringify(f.opts) || JSON.stringify(ids) !== JSON.stringify(anchorIds)) {
          f.opts = opts; anchorIds = ids;
          if (el) { el.textContent = ''; opts.forEach((o, i) => { const op = document.createElement('option'); op.value = i; op.textContent = o; el.appendChild(op); }); }
        }
        const idx = Math.max(0, ids.indexOf(a.anchor));
        if (!fresh('anchor')) { f.v = idx; if (el) el.value = idx; }
      }

      // Groups: every linked Auto Malign, named by the host; a chip press sets that track's group
      if (Array.isArray(s.tracks)) {
        const tb = document.getElementById('groupsTable');
        const sig = JSON.stringify(s.tracks);
        if (tb && tb.dataset.sig !== sig) {
          tb.dataset.sig = sig;
          buildGroups(tb, s.tracks, (r, g) => act('setGroup', { id: r.id, group: g }));
        }
      }

      // the scene and meters follow the real output
      AU.ext = 1; AU.extT = performance.now(); AU.extB = s.bands || []; AU.extL = s.level || 0;
      const toMeter = db => Math.max(0, Math.min(1, (db + 60) / 60));         // -60..0 dBFS
      if ('inDb' in s) { AU.extIn = toMeter(s.inDb); AU.extOut = toMeter(s.outDb); }

      // session status
      learn.setAttribute('aria-pressed', s.learning ? 'true' : 'false');
      learn.querySelector('.sk') ? (learn.querySelector('.sk').textContent = s.learning ? 'Stop learning' : 'Learn')
                                 : (learn.textContent = s.learning ? 'Stop learning' : 'Learn');
      if (cap) cap.innerHTML = `<b>${s.linked}</b> linked<br><b>${Math.round(s.seconds)}</b> s learned<br>${s.solved ? 'solved' : 'not solved'}`;
      if (sig) sig.textContent = s.track || 'Track';
      if (wasLearning !== null && s.learning !== wasLearning)
        toast('ok', s.learning ? `learning · play the song with all ${s.linked} tracks` : 'learning stopped · solving');
      if (wasSolved === false && s.solved) toast('ok', `alignment map ready · ${s.linked} tracks`);
      if (s.linked < 2 && s.learning && Math.random() < 0.01) toast('warn', 'put Auto Malign on every track you want aligned');
      wasLearning = s.learning; wasSolved = s.solved;
    } finally { applying--; }
  });
})(0);
