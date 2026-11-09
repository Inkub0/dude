# DUDE website

Static site published to GitHub Pages (https://inkub0.github.io/dude/) by
`.github/workflows/pages.yml` whenever `site/` changes on `master`.
Plain HTML + one stylesheet; no build step. Preview locally with
`python3 -m http.server -d site 8000`.

One-time setup: repository **Settings → Pages → Source: GitHub Actions**.

## On every release

Nothing to edit for downloads. The buttons point at
`https://github.com/Inkub0/dude/releases/latest/download/<fixed name>`, which GitHub
redirects to that file in the release marked **Latest**:

    dude-win64.zip   dude-linux-x86_64.tar.gz   dude-q4-win64.zip   dude-q4-linux-x86_64.tar.gz

`.github/workflows/release-fixed-names.yml` adds those copies to every published `vX.Y`
release (from its versioned archives), and every Baked AO release is created with
`--latest=false` (see `docs/releasing.md`). The version number, release date and file sizes
on the pages are read from the GitHub API by `assets/js/site.js`; the numbers in the HTML
are only the fallback if that request fails, so refresh them now and then.

Still by hand: the Baked AO links (`baked-ao-vN`, only when a new pack ships) and text
that describes a specific release.

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
