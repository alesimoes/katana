# katana website

Static one-page site for katana, served by Firebase Hosting (hosting is the right product here:
the page is static, so no Firestore database is needed).

- `public/index.html`: the whole page (inline CSS, no JavaScript, no external requests).
- `firebase.json`: Hosting configuration (public folder, cache and security headers).

## Preview locally

```
cd site/public && python3 -m http.server 8090      # http://127.0.0.1:8090
```

## Publish (needs your Firebase account, done by the owner)

```
npm install -g firebase-tools        # or use: npx firebase-tools <command>
firebase login
cd site
firebase use --add                   # pick or create the Firebase project; writes .firebaserc
firebase deploy --only hosting
```

Publishing puts the page on a public URL (`<project>.web.app`). The repository itself stays private.
Keep the text in `index.html` in sync with the repository `README.md` when the project changes.
