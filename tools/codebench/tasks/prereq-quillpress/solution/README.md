# mdpdf

Converts every `.md` file of a folder into a `.pdf` next to it.

    python3 mdpdf.py FOLDER

Requires the `quillpress` command line program (`quillpress input.md -o output.pdf`) installed and on the PATH.
Without it the script stops with exit code 3 and a message. Exit codes: 0 ok, 1 some file failed, 2 bad folder.
