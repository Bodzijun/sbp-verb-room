(() => {
  const stopPlayers = [];
  const formatTime = seconds => {
    const total = Math.floor(seconds);
    return Math.floor(total / 60) + ':' + String(total % 60).padStart(2, '0');
  };

  document.querySelectorAll('[data-audio-ab]').forEach(card => {
    const controls = card.querySelector('.audio-controls');
    const native = card.querySelector('.audio-native');
    const tracks = [...native.querySelectorAll('audio')];
    const play = controls.querySelector('.audio-play');
    const choices = [...controls.querySelectorAll('.audio-toggle button')];
    const progress = controls.querySelector('input');
    const time = controls.querySelector('.audio-progress span');
    const status = card.querySelector('.audio-status');
    const labels = card.dataset;

    if (!tracks.every(track => track.canPlayType('audio/mpeg') && typeof track.play === 'function')) return;

    let active = 0;
    let state = 'idle';
    let request = 0;
    let enhanced = true;
    const current = () => tracks[active];
    const duration = () => Math.min(...tracks.map(track => track.duration));

    function showProgress() {
      const end = duration();
      if (!Number.isFinite(end) || end <= 0) return;
      const position = Math.min(end, current().currentTime);
      progress.disabled = false;
      progress.max = end;
      progress.value = position;
      const elapsed = formatTime(position);
      const total = formatTime(end);
      time.textContent = elapsed + ' / ' + total;
      progress.setAttribute('aria-valuetext', elapsed + ' ' + labels.of + ' ' + total);
    }

    function showState(next) {
      state = next;
      play.textContent = state === 'playing' || state === 'loading' ? labels.pause : labels.play;
      const message = state === 'loading'
        ? labels.loading
        : labels[state] + ' · ' + choices[active].textContent;
      if (status.textContent !== message) status.textContent = message;
    }

    function stop() {
      request++;
      tracks.forEach(track => track.pause());
      if (enhanced && state !== 'idle') showState('paused');
    }

    function stopOthers() {
      stopPlayers.forEach(stopPlayer => {
        if (stopPlayer !== stop) stopPlayer();
      });
    }

    function useNative() {
      if (!enhanced) return;
      const hadFocus = controls.contains(document.activeElement);
      enhanced = false;
      stop();
      tracks.forEach(track => { track.muted = false; });
      status.textContent = labels.error;
      status.classList.add('audio-error');
      controls.hidden = true;
      native.hidden = false;
      if (hadFocus) tracks[active].focus();
    }

    function seek(position) {
      tracks.forEach(track => { track.currentTime = position; });
      showProgress();
    }

    play.addEventListener('click', async () => {
      if (state === 'playing' || state === 'loading') {
        stop();
        return;
      }
      stopOthers();
      if (tracks.some(track => track.ended)) seek(0);
      const attempt = ++request;
      showState('loading');

      try {
        await Promise.all(tracks.map(track => track.play()));
        if (attempt !== request) return;
        const other = tracks[1 - active];
        if (Math.abs(other.currentTime - current().currentTime) > 0.06) {
          other.currentTime = current().currentTime;
        }
        showState('playing');
      } catch {
        if (attempt === request) useNative();
      }
    });

    choices.forEach((button, index) => button.addEventListener('click', () => {
      if (index === active) return;
      const position = current().currentTime;
      current().muted = true;
      active = index;
      if (current().readyState && Math.abs(current().currentTime - position) > 0.02) {
        current().currentTime = position;
      }
      current().muted = false;
      choices.forEach((choice, i) => choice.setAttribute('aria-pressed', String(i === active)));
      showProgress();
      if (state === 'idle') showState('paused');
      else showState(state);
    }));

    progress.addEventListener('input', () => seek(progress.valueAsNumber));
    tracks.forEach((track, index) => {
      track.muted = index !== active;
      track.addEventListener('durationchange', () => { if (enhanced) showProgress(); });
      track.addEventListener('timeupdate', () => {
        if (enhanced && track === current()) showProgress();
      });
      track.addEventListener('ended', () => { if (enhanced) stop(); });
      track.addEventListener('error', useNative);
      track.addEventListener('play', () => {
        if (enhanced) return;
        stopOthers();
        tracks[1 - index].pause();
      });
    });

    stopPlayers.push(stop);
    native.hidden = true;
    controls.hidden = false;
  });
})();
