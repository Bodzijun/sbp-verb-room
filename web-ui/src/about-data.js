// Edit SPONSORS to update the thank-you list shown in the About dialog.
// Keep this array empty ([]) when no supporters are listed: the UI hides
// the entire thank-you sentence automatically.
export const SPONSORS = [
  'smandrap',
  'Teniente Powell',
];

export const PLUGIN_INFO = {
  name: 'Verb Room',
  tagline: 'Physically based room reverb for moving sources and listener',
  license: 'AGPLv3 \u00b7 Built with JUCE',
};

export const WHATS_NEW_VERSION = '0.3.1';

export const WHATS_NEW = [
  'Baked late-tail tables now survive project save and reload when an analytic source pattern is fixed.',
  'Fixed a worker thread race in tail status tracking (workerJumpSeen).',
  'Wide host output buses up to 64 channels are now accepted, silencing unused channels beyond the rendered layout.',
  'Added a permanent Sources rack reminder for M / S / Gain behaviour on host-passed channels.',
  'Added a non-blocking Mix and Settings warning when Positioned dry is off with Ambisonics or surround outputs.',
  'Updated the user manual with a signal-flow diagram and multichannel/Ambisonics routing details.',
];

export const TIPS = [
  'Switch to Creative mode in the header when using modulation routes or filter and gain overrides.',
  'Before drawing host automation, set Edit room \u2192 Scene \u2192 Automation range \u2192 Fit to scene.',
  'Finish room geometry and path edits before Bake path: a later change to the room or path makes the bake outdated.',
  'If the motion comes from host automation, press Record automation, play it once, press Stop, then Bake path.',
  'Open Edit room \u2192 Scene for room-wide surface materials, damping, and automation range settings.',
  'See the Manual link below for ray-tracing order, surround routing, and parameter details.',
];

export const SUPPORT = {
  intro: 'Verb Room is free to download and use under AGPLv3. Voluntary support helps fund continued development:',
  kofi: {
    label: 'Support the project',
    platform: 'Ko-fi',
    url: 'https://ko-fi.com/s/405ebacfcc',
  },
  monobank: {
    label: 'Monobank jar',
    note: 'Ukrainian page, amounts in UAH (~45 UAH per 1 USD)',
    url: 'https://send.monobank.ua/jar/7uwNLVbptd',
  },
  paypal: {
    label: 'PayPal',
    email: 'bodzik@gmail.com',
  },
  contact: {
    label: 'Contact',
    email: 'bodzik@gmail.com',
  },
};

export const LINKS = [
  {
    id: 'manual',
    label: 'Manual',
    url: 'https://github.com/Bodzijun/sbp-verb-room/blob/main/docs/Manual_en.md',
  },
  {
    id: 'project',
    label: 'Project page',
    url: 'https://bodzijun.github.io/sbp-verb-room/',
  },
  {
    id: 'source',
    label: 'Source',
    url: 'https://github.com/Bodzijun/sbp-verb-room',
  },
];

export const ALLOWED_EXTERNAL_HOSTS = new Set([
  'ko-fi.com',
  'send.monobank.ua',
  'github.com',
  'bodzijun.github.io',
]);
