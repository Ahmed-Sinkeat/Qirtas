# Publishing Qirtas to the AUR

Once this is on the AUR, any Arch user installs it with an AUR helper:

```sh
yay -S qirtas-git      # or: paru -S qirtas-git
```

## One-time AUR setup (you do this once)

1. Make an account at <https://aur.archlinux.org> and add your SSH public key
   (My Account → SSH Public Key).
2. Make sure the GitHub repo `https://github.com/Ahmed-Sinkeat/Qirtas` is
   **public** (the PKGBUILD clones it).

## Publish / update

```sh
# Clone the (empty) AUR package repo — the name becomes the package name.
git clone ssh://aur@aur.archlinux.org/qirtas-git.git
cd qirtas-git

# Copy in the PKGBUILD and generate the .SRCINFO metadata AUR requires.
cp /path/to/Qirtas/packaging/aur/PKGBUILD .
makepkg --printsrcinfo > .SRCINFO

# Sanity check it builds.
makepkg -f

git add PKGBUILD .SRCINFO
git commit -m "Initial import: qirtas-git"
git push
```

That's it — it's live. To push an update later, regenerate `.SRCINFO`
(`makepkg --printsrcinfo > .SRCINFO`) and `git push` again. The `pkgver()`
function auto-derives the version from git, so the package tracks HEAD.

## Notes

- This is a **`-git`** package: it always builds the latest commit. If you cut
  tagged releases later, you can add a separate stable `qirtas` package that
  builds a specific tag with a pinned `sha256sums`.
- `provides`/`conflicts=('qirtas')` mean this and a future stable `qirtas`
  package can't both be installed at once (correct — they're the same program).
