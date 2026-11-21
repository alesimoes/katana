---
name: cut-release
when: the user asks to cut, close or prepare a release of this project
needs: sh
args:
---
steps:
1. Run `sh tools/rel/check.sh` => prints `check ok`
2. Run `sh tools/rel/bump.sh` => VERSION now says 1.5.0
3. Run `sh tools/rel/notes.sh` => RELEASE_NOTES.md exists
4. Run `sh tools/rel/seal.sh` => dist/READY exists
done: dist/READY exists and VERSION was bumped
report: tell the user the new version number
fail: stop at the failing step, do not run the later ones, and tell the user which step failed
