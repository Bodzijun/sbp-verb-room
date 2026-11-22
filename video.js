const video = document.querySelector('.video[data-yt]');
const trigger = video.querySelector('.video-trigger');
const watchUrl = 'https://www.youtube.com/watch?v=' + video.dataset.yt;

trigger.addEventListener('click', () => {
  // YouTube refuses to embed (error 153) when the page sends no referrer,
  // e.g. when the landing page is opened straight from disk (file://).
  // In that case open the video on YouTube instead of embedding it.
  if (location.protocol !== 'http:' && location.protocol !== 'https:') {
    window.open(watchUrl, '_blank', 'noopener');
    return;
  }
  const player = document.createElement('iframe');
  player.title = video.dataset.title;
  player.src = 'https://www.youtube-nocookie.com/embed/' + video.dataset.yt + '?autoplay=1&rel=0';
  player.allow = 'autoplay; encrypted-media; picture-in-picture; fullscreen';
  player.allowFullscreen = true;
  player.referrerPolicy = 'strict-origin-when-cross-origin';
  video.replaceChildren(player);
  player.focus();
});
video.hidden = false;
