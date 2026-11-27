# Distribution

How the converted model files and the Linux binary are meant to be published. Everything here is prepared but nothing has been uploaded: the scripts in `tools/release/` only act when the owner runs them. Nothing in this plan was measured or tested.

## What is hosted where

| Artifact | Where | Source in this repo |
|---|---|---|
| FLUX.2 [klein] 4B in GGUF (3 files, about 9.2 GB) | Hugging Face model repo `<owner>/katana-flux2-klein` | `tools/release/cards/katana-flux2-klein.md` |
| EmbeddingGemma 2 Q8_0 (about 0.3 GB) | `<owner>/katana-embeddinggemma` | `tools/release/cards/katana-embeddinggemma.md` |
| Stable Diffusion 1.5 render files (3 files, about 5.3 GB) | `<owner>/katana-sd15`, **not publishable yet** (license BLOCKED) | none |
| Linux x86_64 binary | GitHub release asset `katana-linux-x86_64.tar.gz` | `tools/release/package-linux.sh` |

Repos are created private; `--public` is an explicit choice. The sha256 and size of every hosted file are in `tools/release/MANIFEST.txt` (regenerate with `tools/release/manifest.sh`).

The files are format conversions only (GGUF re-encoding and quantization), with no retraining. Cards say so.

## How `katana pull` and `katana setup` get them

The catalog is fixed in the engine (`src/app/catalog.c`): each item names a repo, an exact revision (commit sha) and the sha256 of the file. `katana pull <id>` downloads from `https://huggingface.co/<repo>/resolve/<rev>/<file>` with curl, resumes interrupted downloads and verifies the sha256. Pinning the revision means a later change to the repo cannot alter what katana downloads.

Today the catalog has only the two chat models. To make the hosted files pullable, each file needs an entry with: `repo` = `<owner>/katana-...`, `rev` = the commit sha printed by `hf-upload.sh`, `sha` and `bytes` = from `MANIFEST.txt`. A private repo needs a Hugging Face token at download time, so the catalog entries should only be switched on once the repos are public. `katana setup` uses the same catalog.

## Licence status per file

Details and quotes are in `tools/release/LICENSES.md`.

| Group | Upstream license as written locally | Status |
|---|---|---|
| FLUX.2 [klein] 4B (transformer, VAE, text encoder) | `license: apache-2.0` | can be published |
| EmbeddingGemma 2 | `license: apache-2.0`, but the header link points to a Gemma license page | can be published; owner should open that link first and confirm it is Apache 2.0 |
| SD 1.5 group (Realistic Vision, LCM-LoRA, ControlNet canny, VAE) | `creativeml-openrail-m`, `openrail++`, `openrail`, `mit` | BLOCKED: OpenRAIL redistribution needs the license text and use restrictions passed on; the local folders only carry the identifiers. No card, and `hf-upload.sh` skips the group |

## Cutting a release

1. Build and package: `sh tools/release/package-linux.sh`. It runs `sh build.sh`, prints `ldd`, and writes `dist/katana-linux-x86_64.tar.gz` (binary, README, `data/coder/kb`, LICENSE if present). Shaders, the web page and the CUDA kernels are embedded in the binary; Vulkan, CUDA and OpenCL libraries are loaded at run time with dlopen when present. The binary links only libm, libmvec and libc (glibc), so it needs a glibc at least as new as the build machine's. The desktop app is not included.
2. Regenerate the manifest if any model file changed: `sh tools/release/manifest.sh models > tools/release/MANIFEST.txt`.
3. Upload the models (below), then update the catalog and rebuild.
4. Tag the commit and attach the tarball to a GitHub release.

## Manual steps for the owner

1. `hf auth login` (the `hf` CLI comes from `pip install -U huggingface_hub`).
2. Dry run, which prints repos, files and sizes and sends nothing: `sh tools/release/hf-upload.sh <owner>`.
3. Upload for real, private first: `sh tools/release/hf-upload.sh <owner> --yes`. Check the repos on the site, then publish by rerunning with `--public` or by changing visibility in the repo settings. The script prints the commit sha of each upload.
4. Paste the revisions, sha256 and sizes into `src/app/catalog.c`, build, commit.
5. Open the DeepMind license link from the EmbeddingGemma card (see above) before making that repo public.
6. Resolve the SD 1.5 license problem (add license texts and restrictions, write `cards/katana-sd15.md`) or leave that group unpublished.
7. The GitHub repository is private. Either make it public, or create a separate releases-only public repo and attach the tarball there, so the source stays private.
