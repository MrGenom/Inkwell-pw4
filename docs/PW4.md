# Inkwell on the Kindle Paperwhite 4: this fork

This branch builds on the Paperwhite 4 port by Pietro Boccaletto (akaped/Inkwell),
which builds on Inkwell by Pragyaan Gaur. It was built and run on a Paperwhite 4
(model PQ94WIF, 10th generation, i.MX6SLL, 512 MB RAM) with firmware 5.18.1,
jailbroken with SpiderCat.

## What this fork changes

| Change | Where |
| --- | --- |
| **USB network on Windows 11.** Windows 11 24H2 binds a COM port driver to the USB gadget's stock IDs (0525:A4A2) and hides its own RNDIS driver, so the Kindle never shows up as a network adapter. `boot.sh` now reads the IDs from `linux/usb-ids`, and the release ships it set to `04E8:6881`, a Samsung RNDIS ID. | `src/linux/boot.sh`, `kindle/linux/usb-ids` |
| **Files**, a file manager on the home screen. Two modes: **Device** shows the whole Kindle (the stock system as init sees it), **Linux** only Inkwell's Alpine system. It opens books in Books and text files in its own viewer, and it can rename, copy, cut, paste, delete and make folders. Read-only file systems and `/proc`, `/sys` and `/dev` can only be looked at, mount points and `alpine.img` can't be deleted or moved, and deleting outside your own places asks twice. The ✕ at the top right goes straight back home. | `src/ui/files.c` |
| **Add-ons at boot.** `own-init` starts every executable in `/etc/own/rc.d` in the background (log: `/var/log/own-rc.log`). | `src/rootfs/sbin/own-init` |
| **Wi-Fi after sleep.** After a suspend, Wi-Fi could reconnect while `wlan0` stayed without an address until a reboot. `wifi-dhcp-watchdog` checks every 20 s and renews or restarts DHCP. | `src/rootfs/etc/own/rc.d/` |
| **More tools in the image**: htop, vim, nano, tmux, git, rsync and an SFTP server (for WinSCP, FileZilla or sshfs). | `build-rootfs.sh` |
| **No Doom and no Minecraft.** | `src/rootfs/etc/own/apps` |
| **Wi-Fi and SSH live in Settings.** Their buttons at the top of Settings open the same apps, and closing them comes back to Settings; the home screen no longer has their icons. | `src/ui/settings.c`, `src/rootfs/etc/own/apps` |
| **A pull-down shade on the home screen.** A swipe down from the top edge, or a tap on the status line, shows the Wi-Fi network and address, the battery, the frontlight slider (drag it) and a **Settings** button; a swipe up, a tap below it or the power button closes it. The slider and the Settings icon left the home screen for it. | `src/ui/home.c`, `src/rootfs/etc/own/apps` |
| **Build fixes.** `release.sh` no longer copies an example config that was never committed (every release build failed on it), scripts keep LF line endings on Windows, and `tools/docker-build.sh` builds the release with Docker only. | `tools/`, `.gitattributes` |

## Build

With Docker (any OS; on Windows run it from Git Bash):

```sh
tools/docker-build.sh 1.2.0-pw4
```

The release lands in `dist/Inkwell-1.2.0-pw4.zip`. Downloads are kept in the
Docker volume `inkwell-downloads`, so the first build takes the longest. On a
Mac or Linux machine with the tools, the README's `tools/fetch.sh` and
`tools/release.sh` work as before.

## Install or update

1. Boot the Kindle into the stock UI (the home screen's **Kindle UI** button, or
   press power at the Inkwell splash) and connect it by USB.
2. Unzip the release and copy `linux` and `documents` to the top of the
   Kindle's drive, merging with what is there.
3. Eject. On a first install tap **Inkwell: install** in the library; on an
   update just reboot into Inkwell.

An update replaces `linux/alpine.img`, the whole Alpine system. Anything you
changed or installed inside it is gone afterwards; files on the Kindle's drive
(`linux/wifi.conf`, `linux/authorized_keys`, notes, books) stay.

## USB networking on Windows

The Kindle is `192.168.15.244` on the USB link and hands the computer an
address. With `04E8:6881` in `linux/usb-ids`, Windows binds Samsung's signed
RNDIS driver, which comes with "Samsung USB Driver for Mobile Phones"; install
that once if the Kindle shows up without a network adapter. Linux and macOS
pick their driver by class and work with either ID. Delete `linux/usb-ids`
for the stock IDs.

## SSH

Over USB, as in the README: `./ssh.sh` once to make a key, copy
`keys/kindle_ed25519.pub` to the Kindle's drive as `linux/authorized_keys`,
then

```sh
ssh -i keys/kindle_ed25519 -p 2222 root@192.168.15.244
```

Over Wi-Fi, **Settings → SSH** starts a server on port 2022.

## Files: notes

- The Kindle's drive is `/mnt/us` in Linux mode and `/mnt/base-us` in Device mode.
- New names are typed in Latin letters, as Inkwell's keyboard has no other.
- `files [device|linux] [folder]` opens a mode and folder from the terminal.

## Branches and releases

| Branch | Role |
| --- | --- |
| `main` | a mirror of akaped/Inkwell, for pulling in the port's updates |
| `dev` | ongoing work |
| `release` | what is released; `v*-pw4` tags go here. Protected on GitHub against force pushes and deletion |

To publish a release, bring `release` up to `dev` and run the script:

```sh
git switch release && git merge --ff-only dev
tools/release-gh.sh 1.2.1-pw4            # or: tools/release-gh.sh 1.2.1-pw4 notes.md
git switch dev
```

It checks the branch, the tree and the tag, builds the zip with Docker, tags
and pushes, and publishes the GitHub release with the zip and `SHA256SUMS`
(`DRY_RUN=1` runs only the checks). An urgent fix can go straight onto
`release` and then be cherry-picked into `dev`.
