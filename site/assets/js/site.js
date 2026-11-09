// Tiny screenshot lightbox. The pages work without it (links open the full image).
(function () {
  var links = document.querySelectorAll(".gallery a");
  if (!links.length) return;
  var box = document.createElement("div");
  box.className = "lightbox";
  box.setAttribute("role", "dialog");
  box.setAttribute("aria-label", "Screenshot");
  var img = document.createElement("img");
  box.appendChild(img);
  document.body.appendChild(box);
  function close() { box.classList.remove("open"); img.removeAttribute("src"); }
  box.addEventListener("click", close);
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") close(); });
  links.forEach(function (a) {
    a.addEventListener("click", function (e) {
      e.preventDefault();
      img.src = a.href;
      img.alt = a.querySelector("img").alt;
      box.classList.add("open");
    });
  });
})();

// Install page: the case picker shows one case at a time. Without JavaScript every
// case stays visible, so the page still works as one long guide.
(function () {
  var bar = document.querySelector(".tabs[data-switch]");
  if (!bar) return;
  var links = Array.prototype.slice.call(bar.querySelectorAll("a"));
  var panels = links.map(function (a) { return document.getElementById(a.hash.slice(1)); });
  var hint = document.querySelector(".pick-hint");
  links.forEach(function (a, i) {
    a.setAttribute("role", "tab");
    a.setAttribute("aria-controls", panels[i].id);
    panels[i].setAttribute("role", "tabpanel");
  });

  function show(id) {
    var any = false;
    links.forEach(function (a, i) {
      var on = panels[i].id === id;
      a.setAttribute("aria-selected", on ? "true" : "false");
      panels[i].hidden = !on;
      any = any || on;
    });
    if (hint) hint.hidden = any;
    return any;
  }

  // A hash naming a case (or something inside one) opens that case and scrolls to it.
  // Other hashes (#before, #first-launch) leave the current choice alone.
  function fromHash(scroll) {
    var id = location.hash.slice(1);
    var el = id && document.getElementById(id);
    var panel = el && el.closest(".scenario");
    if (panel && show(panel.id)) {
      if (scroll) el.scrollIntoView();
      return true;
    }
    return false;
  }

  bar.addEventListener("click", function (e) {
    var a = e.target.closest("a");
    if (!a) return;
    e.preventDefault();
    history.replaceState(null, "", a.hash);
    show(a.hash.slice(1));
  });
  window.addEventListener("hashchange", function () { fromHash(true); });
  if (!fromHash(true)) show(null);
})();

// Steam Launch Options: build the Windows line from the folder the player pastes,
// and give every line to paste a Copy button.
(function () {
  document.querySelectorAll(".lo-builder").forEach(function (box) {
    var input = box.querySelector("input");
    var out = box.querySelector("code");
    var exe = box.getAttribute("data-exe");
    var fallback = input.getAttribute("placeholder");
    function update() {
      var dir = input.value.trim().replace(/^"+|"+$/g, "").replace(/[\\\/]+$/, "");
      // A pasted path that already ends in the exe: keep just its folder.
      if (dir.toLowerCase().slice(-exe.length - 1) === "\\" + exe) dir = dir.slice(0, -exe.length - 1);
      out.textContent = '"' + (dir || fallback) + "\\" + exe + '" %command%';
    }
    input.addEventListener("input", update);
    update();
  });

  document.querySelectorAll("code.cmdline").forEach(function (code) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "copy";
    btn.textContent = "Copy";
    btn.addEventListener("click", function () {
      var text = code.textContent;
      function done() {
        btn.textContent = "Copied";
        setTimeout(function () { btn.textContent = "Copy"; }, 1500);
      }
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(done, function () { select(); });
      } else {
        select();
      }
      function select() {
        var r = document.createRange();
        r.selectNodeContents(code);
        var sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(r);
        btn.textContent = "Press Ctrl+C";
      }
    });
    var wrap = document.createElement("div");
    wrap.className = "cmdwrap";
    code.parentNode.insertBefore(wrap, code);
    wrap.appendChild(code);
    wrap.appendChild(btn);
  });
})();

// Version, date and file sizes of the latest release, read from GitHub. The download
// links (releases/latest/download/<fixed name>) work without this; the page just keeps
// the numbers it was published with if the request fails.
(function () {
  var version = document.querySelectorAll('[data-latest="version"]');
  var date = document.querySelectorAll('[data-latest="date"]');
  var sizes = document.querySelectorAll("[data-size]");
  if (!version.length && !date.length && !sizes.length) return;
  if (!window.fetch) return;
  fetch("https://api.github.com/repos/Inkub0/dude/releases/latest")
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (rel) {
      // Only a DUDE release counts (tags v0.9.8, v1.0, ...), never a Baked AO one.
      if (!rel || !/^v\d/.test(rel.tag_name || "")) return;
      var v = rel.tag_name.replace(/^v/, "");
      version.forEach(function (el) { el.textContent = v; });
      if (rel.published_at) date.forEach(function (el) { el.textContent = rel.published_at.slice(0, 10); });
      var byName = {};
      (rel.assets || []).forEach(function (a) { byName[a.name] = a.size; });
      sizes.forEach(function (el) {
        var n = byName[el.getAttribute("data-size")];
        if (n) el.textContent = Math.round(n / 1e6) + " MB";
      });
    })
    .catch(function () {});
})();
