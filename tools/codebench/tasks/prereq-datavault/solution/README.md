# dvupload

    python3 dvupload.py FILE

Uploads FILE to Datavault and prints the new file id.

Needs:

- a Datavault token in `~/.datavault/credentials`, a JSON file such as `{"token": "..."}` (without it the script exits with 1 and says so);
- network access to the API: `DATAVAULT_URL` (default `https://api.datavault.example`).
