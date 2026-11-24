# Wallpapers: why the iOS 16 default showed only a blue gradient (2026-11-24)

**Cause.** The default wallpaper (id 7400 "WWDC 2022", collection iOS_16~iphone, under `/Library/Wallpaper/Collections/`) is three CoreAnimation
packages (`*_Background/Floating/Foreground-375w-812h@3x~iphone.ca`): a `CAGradientLayer` plus layers whose artwork is HEIC files (Head, Head Mask, Leg, Arm, Body).
HEIC is HEVC-compressed. A real iPhone decodes it with its video decode hardware; this port has none (SpringBoard's IOKit matching for `AppleVXD375/390/393`
returns nothing, and no other decoder is found), so every artwork layer failed and only the gradient drew. A CPU HEVC decoder behind the same interface would be the
general fix (it would also fix HEIC photos); it has not been attempted.

**Workaround.** Convert each HEIC to PNG (`convert x.HEIC x.png`, no alpha channel is lost) and write the PNG over the HEIC under the same name: ImageIO identifies
the format by content, and the `.ca` asset manifests carry no hashes. Files are written with `hfsx_write.py replace` after a journal-flush boot (`set-phone-file.sh`
pattern), each verified byte for byte (the check must `sync` and retry; the first try right after a write gave false failures). 48 files, 19 MB of HEIC. Originals:
`/usb/work/standalone/backup-wallpaper/`. Result: the lock-screen render (`RuntimeSnapshot-lock.atx` of the poster configuration) holds the real iOS 16 artwork.

**Also needed first:** `NBFunction` must conform to `MTLFunction` (plugin commit f1ab13b), otherwise Metal rejects the compositor's blur and effect functions.

**Open.** The home screen shows a smooth blue gradient with soft glows (probably the genuine blurred home look; needs the owner's eyes). The Photos widget renders solid black.
The phone boots straight to the home screen, so the lock screen is never captured by the snapshot ring; decode the cached render instead:
`.atx` = ASTC 4x4 1080x2160, data = last 2332800 bytes of the 'astc' chunk (file offset 0x3ffc), header `13 ab a1 5c 04 04 01` + 3-byte dims; `astcenc -dl` (Debian package `astcenc`). Tiles come out scrambled, colours are real.
Screenshots: `seam-sweep.sh` ends on a Spotlight frame; `sweet-shot-pull.py OUT.png` in the recovery writes all frames (`OUT-s0N.png`), the early ones show the plain home screen.
