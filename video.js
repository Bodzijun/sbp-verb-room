document.querySelectorAll('.video[data-yt]').forEach((video) => {
  const trigger = video.querySelector('.video-trigger');
  const videoId = video.dataset.yt;
  const watchUrl = 'https://www.youtube.com/watch?v=' + videoId;

  trigger.addEventListener('click', () => {
    // Pages opened from disk send no referrer, which causes YouTube error 153.
    if (location.protocol !== 'http:' && location.protocol !== 'https:') {
      window.open(watchUrl, '_blank', 'noopener');
      return;
    }
    const player = document.createElement('iframe');
    player.title = video.dataset.title;
    player.src = 'https://www.youtube-nocookie.com/embed/' + videoId + '?autoplay=1&rel=0';
    player.allow = 'autoplay; encrypted-media; picture-in-picture; fullscreen';
    player.allowFullscreen = true;
    player.referrerPolicy = 'strict-origin-when-cross-origin';
    video.replaceChildren(player);
    player.focus();
  });
  video.hidden = false;
});
