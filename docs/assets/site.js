/* Progressive enhancement: language links and downloads work without JavaScript. */
(() => {
  const links = Array.from(document.querySelectorAll('[data-gallery]'));
  const panels = Array.from(document.querySelectorAll('.gallery-panel'));
  if (!links.length) return;
  function select(id) {
    for (const panel of panels) panel.hidden = panel.id !== id;
    for (const link of links) link.setAttribute('aria-current', String(link.hash === '#' + id));
  }
  select(panels.some(panel => '#' + panel.id === location.hash) ? location.hash.slice(1) : panels[0].id);
  for (const link of links) link.addEventListener('click', event => {
    event.preventDefault();
    select(link.hash.slice(1));
  });
})();
