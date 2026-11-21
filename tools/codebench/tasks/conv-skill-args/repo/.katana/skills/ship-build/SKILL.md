---
name: ship-build
when: the user wants to publish a build of a given version to a distribution channel
needs: sh
args: version, channel
---
steps:
1. Run `sh tools/pack.sh <version>` => dist/app-<version>.pack exists
2. Run `sh tools/push.sh <version> <channel>` => prints `pushed <version> to <channel>`
3. Run `sh tools/status.sh <channel>` => prints `<channel> ok`
done: the channel status check printed ok
report: tell the user version and channel that were published
fail: stop at the failing step and show its error output
