# DUDE website

Static site published to GitHub Pages (https://inkub0.github.io/dude/) by
`.github/workflows/pages.yml` whenever `site/` changes on `master`.
Plain HTML + one stylesheet; no build step. Preview locally with
`python3 -m http.server -d site 8000`.

One-time setup: repository **Settings → Pages → Source: GitHub Actions**.

## On every release

Releases carry only the versioned archives, one name per file (`dude-X.Y-win64.zip`,
`dude-X.Y-linux-x86_64.tar.gz`, `dude-q4-X.Y-win64.zip`, `dude-q4-X.Y-linux-x86_64.tar.gz`); no
fixed-name copies. Every link to one is a template, `data-asset="dude-VER-win64.zip"` (plus
`data-asset-name` on a displayed file name and `data-size` on a size), and `assets/js/site.js`
fills link, name, size, version and date from the GitHub API's latest release. A release whose tag
does not start with `v` (Baked AO, Baked PBR) is ignored, and those are created with
`--latest=false` anyway (see `docs/releasing.md`).

By hand, once per release: the HTML's fallback (the hrefs `releases/download/vX.Y/...`, the file
names, the `data-latest` version and date, the MB figures), used when the API request fails or
JavaScript is off. When site.js changes, bump its `?v=N` in the pages' script tags so browsers do
not keep the old one. Also by hand: the Baked AO / PBR pack links (only when a new pack ships) and
text that describes a specific release.

## Search engines

Each page carries its title, description, canonical URL and share-card tags in `<head>`
(keep the title under ~60 characters, the description under ~160); `index.html` also has
JSON-LD (WebSite + SoftwareApplication). `sitemap.xml` lists the pages: add a new page
there and bump its `lastmod` when a page changes a lot, then resubmit it in Google Search
Console. There is no `robots.txt`: crawlers only read one at the root of
inkub0.github.io, which this project site does not own. This README is not published
(`pages.yml` removes it before upload).

## Images

Screenshots come from the ModDB page (2560x1440 originals resized to 1920 px,
plus 640 px thumbnails in `assets/img/shots/`). `hero.jpg` is the home banner,
`og.jpg` the 1200x630 social-media preview, `logo.png` the pixel logo (rendered
with `image-rendering: pixelated`, so keep it small and let CSS scale it).
