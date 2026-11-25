/* Align/Malign plugin bridge: connects the HTML interface to the plugin when it runs inside the
   plugin window (JUCE WebBrowserComponent with native integration). In a normal browser this does nothing. */
(function () {
  const J = window.__JUCE__;
  if (!J || !J.backend) return;
  document.documentElement.classList.add('in-plugin');

  let applying = 0;
  const send = (id, v) => { if (!applying) J.backend.emitEvent('param', { id, v }); };
  const act = (name) => J.backend.emitEvent('action', { name });

  // ---- UI -> plugin
  main.sub(v => send('alignment', Math.round((v - 0.5) * 2000) / 10)); // knob 0..1 -> -100 (align) .. +100 (malign)
  for (const id of ['match', 'width', 'motion', 'mix']) MAC[id].sub(v => send(id, v));
  document.querySelectorAll('.flavor button').forEach(b => b.addEventListener('click', () => send('flavour', b.dataset.flv)));
  for (const id of ['ab', 'delta']) {
    const b = document.getElementById(id);
    b.addEventListener('click', () => send(id, b.getAttribute('aria-pressed') === 'true'));
  }
  document.querySelectorAll('#scope button').forEach(b => b.addEventListener('click', () => send('scope', b.dataset.v)));

  // Learn: one button in the status bar drives the whole session
  const learn = document.getElementById('learn');
  learn.addEventListener('click', () => act('learn'));

  // ---- plugin -> UI
  const cap = document.querySelector('.pyrbox .cap');
  const sig = document.getElementById('sig');
  let wasLearning = null, wasSolved = null;
  const press = (sel, on) => { const b = document.querySelector(sel); if (b) b.setAttribute('aria-pressed', on ? 'true' : 'false'); };

  J.backend.addEventListener('state', s => {
    applying++;
    try {
      const kv = 0.5 + s.alignment / 200;
      if (Math.abs(main.get() - kv) > 0.0015) main.set(kv);
      for (const id of ['match', 'width', 'motion', 'mix'])
        if (Math.abs(MAC[id].get() - s[id]) > 0.0015) MAC[id].set(s[id]);
      if (P.flavor !== s.flavour) {
        const b = document.querySelector(`.flavor button[data-flv="${s.flavour}"]`);
        if (b) b.click();
      }
      for (const id of ['ab', 'delta']) {
        const b = document.getElementById(id);
        if ((b.getAttribute('aria-pressed') === 'true') !== !!s[id]) { b.setAttribute('aria-pressed', s[id] ? 'true' : 'false'); P[id] = !!s[id]; }
      }
      document.querySelectorAll('#scope button').forEach(b => b.setAttribute('aria-pressed', b.dataset.v === s.scope ? 'true' : 'false'));

      // the scene and meters follow the real output
      AU.ext = 1; AU.extT = performance.now(); AU.extB = s.bands || []; AU.extL = s.level || 0;

      // session status
      learn.setAttribute('aria-pressed', s.learning ? 'true' : 'false');
      learn.querySelector('.sk') ? (learn.querySelector('.sk').textContent = s.learning ? 'Stop learning' : 'Learn')
                                 : (learn.textContent = s.learning ? 'Stop learning' : 'Learn');
      if (cap) cap.innerHTML = `<b>${s.linked}</b> linked<br><b>${Math.round(s.seconds)}</b> s learned<br>${s.solved ? 'solved' : 'not solved'}`;
      if (sig) sig.textContent = s.track || 'Track';
      if (wasLearning !== null && s.learning !== wasLearning)
        toast('ok', s.learning ? `learning · play the song with all ${s.linked} tracks` : 'learning stopped · solving');
      if (wasSolved === false && s.solved) toast('ok', `alignment map ready · ${s.linked} tracks`);
      if (s.linked < 2 && s.learning && Math.random() < 0.01) toast('warn', 'put Align Malign on every track you want aligned');
      wasLearning = s.learning; wasSolved = s.solved;
    } finally { applying--; }
  });
})();
