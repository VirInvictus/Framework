# Flathub Submission Checklist

Prepared 2026-09-15 per Brandon's go: prep now, submit after the blitz
release tag (1.0.1) exists. The in-repo manifest
(`io.github.virinvictus.framework.yml`) is the source; the Flathub repo
carries its copy.

## Ready (already in-tree)

- Manifest is `type: git` + `tag:` on the GitHub repo (swapped at v1.0.0),
  release buildtype, with `x-checker-data` on both the framework git
  source (`^v[\d.]+$` tag pattern) and the MuPDF archive source
  (html checker against the mupdf.com archive index), so the Flathub
  bot proposes version bumps.
- MuPDF module bumped to 1.28.2, matching what dev and CI link and what
  the stress suite runs against.
- `<screenshots>` block live with four real captures
  (Flathub prefers 16:9; captures are 1500x980, and the metainfo note
  records the 1500x844 recrop target: recrop only if the review flags it).
- `appstreamcli validate --no-net` and `desktop-file-validate` pass.
- Permissions audit verdict (roadmap Phase 15): finish-args tight; no
  changes needed for submission.

## Steps when submitting (target: the blitz release tag)

1. Wait for the release tag with the final-audit fixes (never submit
   v1.0.0; the audit's memory-safety asterisk closes with 1.0.1).
2. Confirm the manifest's framework `tag:` matches that release tag.
3. Create the Flathub repo: `github.com/flathub/io.github.virinvictus.framework`
   (request via the Flathub submission issue, "I have committed to the
   maintainer role"; the manifest is the single module source).
4. Copy the in-repo manifest into the Flathub repo as the root manifest
   (Flathub convention: manifest at repo root, name matching the app ID).
5. Full `flatpak-builder --show-manifest` sanity build locally against
   `org.gnome.Platform//50` and `org.gnome.Sdk//50`, then
   `flatpak-builder --run` the app; confirm the schema post-install step
   still fires and the app launches.
6. `appstream-compose` validation runs inside the build; the 128px PNG
   icon must remain the only pixel-validated hicolor icon that
   GdkPixbuf cannot parse (SVGs stay `-Dflatpak=true`-gated).
7. Open the submission PR in the Flathub repo. The automated bot
   (`flathub/buildbot`) builds all arches; first review covers the
   manifest cleanliness, screenshots, and the OARS rating (default, no
   objectionable content).
8. After approval: the Flathub repo's `x-checker-data` handles future
   bump proposals; each release = flip the manifest `tag:` in the
   Flathub repo (a one-line PR) once CI is green here.

## Deliberately not done

- No `.flatpak` bundle assets on GitHub Releases: Flathub-first per
  decision (2026-09-15). Revisit only on real demand for non-Flathub
  binaries.
