# UEFI firmware

`QEMU_EFI.fd` and `QEMU_VARS.fd` are unmodified RELEASE builds of EDK2's
ArmVirtQemu platform for AArch64 (TianoCore, BSD-2-Clause-Patent, see
`EDK2-LICENSE.txt`), taken from https://github.com/retrage/edk2-nightly at commit
41d353d7c9a1b1c83ddc4392d427c79e357bf18a (built Mon Nov 9 05:53:15 2026 +0000).

| File | SHA-256 |
|---|---|
| QEMU_EFI.fd | 182b38555de0bb0363122f119d8f7839406ee6cd246238d83f48a2ffd0da2667 |
| QEMU_VARS.fd | 8b634c1e6bd11607850b69111f6c4dbd1583dbbd1460dac72dbacbdc1a4a130a |

WindDroid loads `QEMU_EFI.fd` into flash bank 0 and a copy of `QEMU_VARS.fd` into
flash bank 1, exactly like QEMU's `-machine virt` with two pflash drives.
