(function () {
  var topbar = document.getElementById('topbar');
  var update = function () {
    if (!topbar) return;
    if (window.scrollY > 12) topbar.classList.add('scrolled');
    else topbar.classList.remove('scrolled');
  };
  window.addEventListener('scroll', update, { passive: true });
  update();

  var menu = document.getElementById('mobile-menu');
  if (menu) {
    menu.querySelectorAll('a').forEach(function (link) {
      link.addEventListener('click', function () { menu.removeAttribute('open'); });
    });
  }

  var btn = document.getElementById('copy');
  if (btn) {
    btn.addEventListener('click', function () {
      var command = document.getElementById('cmd');
      if (!command) return;
      var text = command.textContent.trim();
      var done = function () {
        btn.textContent = 'copied';
        btn.setAttribute('data-done', '1');
        setTimeout(function () {
          btn.textContent = 'copy';
          btn.removeAttribute('data-done');
        }, 1800);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, function () { fallback(text, done); });
      } else {
        fallback(text, done);
      }
    });
  }

  function fallback(text, done) {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.cssText = 'position:absolute;left:-9999px';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); done(); } catch (e) {}
    document.body.removeChild(ta);
  }

  var card = document.getElementById('benchmark-card');
  if (card) {
    requestAnimationFrame(function () {
      requestAnimationFrame(function () { card.classList.add('ready'); });
    });
  }

  var here = window.location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('[data-page]').forEach(function (link) {
    if (link.getAttribute('data-page') === here) {
      link.classList.add('active');
      link.setAttribute('aria-current', 'page');
    }
  });
})();
