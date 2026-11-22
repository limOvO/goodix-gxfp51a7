# Publishing this to GitHub

The exported directory is already a git repository with one commit, so this is
all that's needed. Nothing here is secret: no keys, no biometric data, no vendor
binaries.

## Option A — GitHub CLI (simplest)

```bash
cd ~/下載/goodix-gxfp51a7
gh auth login
gh repo create goodix-gxfp51a7 --public --source=. --push \
  --description "Goodix GXFP51A7 fingerprint sensor on Linux (Huawei MateBook 13 WRT-WX9, firmware APP_14115)"
```

## Option B — create the repo in the browser, then push

Create an empty repo (no README, no .gitignore) at
<https://github.com/new>, then:

```bash
cd ~/下載/goodix-gxfp51a7
git remote add origin git@github.com:<your-user>/goodix-gxfp51a7.git
git branch -M main
git push -u origin main
```

Use the HTTPS form if you have no SSH key configured:

```bash
git remote add origin https://github.com/<your-user>/goodix-gxfp51a7.git
```

## Before publishing

- The commit is authored as `limm <limm@localhost>`. Fix it if you want your
  real address attached:

  ```bash
  git commit --amend --reset-author --no-edit
  ```

- Add a `LICENSE` file if you want the licence stated at the repo root. The
  driver code is LGPL-2.1-or-later (inherited from upstream), and `COPYING`
  lives in the upstream checkout rather than here.

## Notes for readers

The repository deliberately does **not** contain the upstream sources. Run
`./bootstrap.sh` to clone them at the exact revisions used here and apply
`01-app-14115-support.patch`, so the diff stays small and reviewable rather
than vendoring libfprint.
