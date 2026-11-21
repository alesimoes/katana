---
name: backup-db
when: the user wants a snapshot of the database before a schema change or migration
needs: sh
args:
---
steps:
1. Run `sh tools/backup/db_dump.sh` => prints `dump ok`
2. Run `sh tools/backup/db_verify.sh` => prints `db backup verified`
done: the db backup is verified
report: tell the user where the snapshot is
fail: stop and show the error output
