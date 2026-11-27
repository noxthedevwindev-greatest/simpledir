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
    var status = document.getElementById('copy-status');
    var say = function (message) {
      if (status) status.textContent = message;
      btn.setAttribute('aria-label', 'copy install command' + (message ? ': ' + message : ''));
    };
    btn.addEventListener('click', function () {
      var command = document.getElementById('cmd');
      if (!command) return;
      var text = command.textContent.trim();
      var reset = function () {
        btn.textContent = 'copy';
        btn.removeAttribute('data-done');
        say('');
      };
      var done = function () {
        btn.textContent = 'copied';
        btn.setAttribute('data-done', '1');
        say('install command copied');
        setTimeout(reset, 1800);
      };
      var fail = function () {
        btn.textContent = 'copy failed';
        btn.removeAttribute('data-done');
        say('copy failed; select the command and copy it manually');
        setTimeout(reset, 2400);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, function () { fallback(text, done, fail); });
      } else {
        fallback(text, done, fail);
      }
    });
  }

  function fallback(text, done, fail) {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.cssText = 'position:absolute;left:-9999px';
    document.body.appendChild(ta);
    ta.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
    document.body.removeChild(ta);
    if (ok) done(); else fail();
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
