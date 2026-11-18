(() => {
  'use strict';
  const root = document.documentElement;
  const storageKey = 'fumetrix.theme';

  function readTheme() {
    try { return localStorage.getItem(storageKey); }
    catch { return null; }
  }

  function applyTheme(value) {
    const theme = value === 'light' ? 'light' : 'dark';
    const changed = root.dataset.theme !== theme;
    root.dataset.theme = theme;
    const button = document.querySelector('[data-theme-toggle]');
    if (button) {
      button.textContent = theme === 'light' ? 'Dark mode' : 'Light mode';
      button.hidden = false;
    }
    if (changed) window.dispatchEvent(new Event('fumetrix:themechange'));
  }

  applyTheme(readTheme());
  document.addEventListener('DOMContentLoaded', () => {
    const button = document.querySelector('[data-theme-toggle]');
    if (!button) return;
    applyTheme(root.dataset.theme);
    button.addEventListener('click', () => {
      const theme = root.dataset.theme === 'light' ? 'dark' : 'light';
      try { localStorage.setItem(storageKey, theme); }
      catch { /* The switch still works when browser storage is unavailable. */ }
      applyTheme(theme);
    });
  });
  window.addEventListener('storage', event => {
    if (event.key === storageKey || event.key === null) applyTheme(readTheme());
  });
})();
