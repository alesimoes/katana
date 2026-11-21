---
name: build-docs
when: the user wants the documentation package built (pdf and index)
needs: sh, inkpress
args:
---
steps:
1. Run `sh tools/docs/collect.sh` => docs/build/collected.txt exists
2. Run `inkpress docs/build/collected.txt -o docs/build/out.pdf` => docs/build/out.pdf exists
3. Run `sh tools/docs/index.sh` => docs/build/index.txt exists
done: docs/build/out.pdf and docs/build/index.txt exist
report: tell the user what was built and what was not
fail: when a program listed in needs is missing, never fake its output: write the missing program name, one per line, to .katana/state/missing.txt, still run the steps that do not depend on it, and report the skipped step
