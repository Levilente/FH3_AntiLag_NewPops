# Publishing a release

The release must contain **only the DLL** as an asset.

1. Build (see the README): the result is `build/AntiLag.dll`.
2. Test the DLL in game once (the platform's `fh3-mods\platform.log` should show no errors).
3. Make sure the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`), `CHANGELOG.md` and the tag agree (current: `1.0.0`). The platform's update check compares the GitHub release tag (a leading `v` is ignored) with the version the mod registers.
4. Tag and publish:

```sh
git tag v1.0.0
git push origin v1.0.0
gh release create v1.0.0 build/AntiLag.dll --title "Anti-Lag for FH3 1.0.0" --notes-file release-notes.md
```

`release-notes.md` is the matching section of [CHANGELOG.md](CHANGELOG.md) (it carries the requirements). Do not attach source archives, ini files or anything else.

The DLL is build output and is never committed.
