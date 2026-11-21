---
name: backup-files
when: the user wants an archive copy of the uploaded files in uploads/ (images, documents)
needs: sh, tar
args:
---
steps:
1. Run `sh tools/backup/files_tar.sh` => backups/uploads.tar exists
2. Run `sh tools/backup/files_verify.sh` => prints `files backup verified`
done: the uploads archive is verified
report: tell the user where the archive is
fail: stop and show the error output
