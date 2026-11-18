# Unattended test loop (bonito)

Nobody has to touch the phone between tests. Verified on the phone 2026-11-18.

    bonito/tools/mkios.sh KERNEL NAME "extra boot-args"      # builds bonito/shim/out/NAME.img (standard native config)
    bonito/tools/cycle.sh bonito/shim/out/NAME.img NAME [WAIT_S]   # boot it, wait, pull logs/NAME.txt, back to bootloader

`mkios.sh` always adds `sweetpanicreset=1 sweetwdt=25`. **Add `sweetreboot=SECS` to every normal test** (for example 90 for a plain
boot, 240 for the GPU probe stages), otherwise iOS keeps running and `cycle.sh` waits out its timeout and exits 1.

How the phone gets back to the log-reading recovery by itself (xnu patch 0150, bsd/dev/bonito_reset.c):

| case | mechanism | boot-arg | verified |
|---|---|---|---|
| test finished | timed reset (log flush wait of 4 s, then PS_HOLD write 0 at 0xc264000, like Android's msm restart) | `sweetreboot=SECS` | logs/reset1.txt |
| kernel panic | reset right after the panic text is on the eMMC (hook in osfmk/kern/debug.c after `sweet_log_panic_flush`) | `sweetpanicreset=1` | logs/reset2.txt |
| kernel hang | SoC watchdog (0x17980000, bite <= 30 s, petted every secs/3 by a kernel thread) resets the chip; plain reset, no download mode | `sweetwdt=SECS` | logs/reset3.txt |

Test hooks: `sweetpanictest=SECS` panics on purpose, `sweetwdttest=SECS` stops petting. `sweetresetmethod=0|1|2` picks PS_HOLD /
PSCI SYSTEM_RESET / both (default 0, the one proven here).

`cycle.sh` exit codes: 0 ok; 1 the phone never came back (a hang that no reset caught: needs a Power hold, the loop has to stop);
2 the bootloader could not be reached or the device is not a bonito; 3 the log could not be read. It checks the product name before
every boot, runs `fastboot set_active a` (resets the A/B retry counter) and never flashes anything.

What it cannot cover: a hang before the kernel starts its watchdog thread (shim or very early kernel), and the very rare chip state
where a reset does not restart at all.
