# MeerCut website

The bilingual website is hosted by GitHub Pages from `main`, `/docs`:
https://codewithameer.github.io/MeerCut/

No build service, JavaScript framework, analytics, external fonts, or runtime dependencies are required.
English lives at `/MeerCut/`; Arabic lives at `/MeerCut/ar/` with native RTL markup.
Both pages work without JavaScript; the small script enhances the screenshot gallery.

## Updating

1. Edit English and Arabic copy in `tools/build_website.py`.
2. Run `python tools/build_website.py` from the repository root (Python standard library only).
3. Edit `docs/assets/site.css` for styling, or `docs/assets/site.js` for gallery behaviour.
4. Preview: `python -m http.server 8080 --directory docs`. Check both `/` and `/ar/`, including mobile widths and keyboard navigation.
5. Commit the generated pages and source files, then push through GitHub Desktop. GitHub Pages deploys `/docs`.

Use relative local links so project-path hosting works. When updating a release, update both languages, the download URLs, version metadata and structured data in the builder. Verify facts against that release before changing the copy. Screenshots and the icon are the repository’s real assets, used without altering their content.

## Sources and attribution

Website copy was checked against the v1.0.0 release, README, LICENSE, CREDITS.md and AI_MODELS.md. Hardware listed on the site is the documented test machine, not an invented minimum requirement. The existing GPL-3.0-or-later licence applies.

Screenshot sample footage from Sintel and Tears of Steel is © Blender Foundation, CC BY 3.0; visible attribution and source links appear in both page footers. See CREDITS.md for original-source and modification details.
