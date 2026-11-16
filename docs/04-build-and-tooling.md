# Build & tooling

## Environment (verified present)
- `arm-none-eabi-gcc 13.2.1` (build), `arm-none-eabi-objdump`, host `gcc`.
- Python: `capstone 5.0.7` (RE), `numpy` (card-flags tool), `docutils` (module build — install with `pip install --break-system-packages docutils`).
- No Ghidra / JDK17 needed (we use capstone + objdump).
- `git`, `gh` (authenticated). qemu-eos NOT cloned yet.

## Build a platform
```
cd magiclantern_simplified/platform/M50.110      # or M50mk2.101
make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi
# -> build/autoexec.bin , build/magiclantern.zip , build/<MODEL>_<FW>.sym
```
Toolchain comes from Makefile.globals: `CROSS_COMPILE=$(ARM_BINPATH)/arm-$(ARM_ABI)-` (defaults /usr/bin, none-eabi).

## RE / stub-finding (capstone)
Load `gang100.bin` at base `0xe0000000` (main fw at `0xe0040000`). Disassemble Thumb; find LDR-literal
xrefs to anchor strings; match structurally against `platform/M50.110/{stubs.S,consts.h}` (documented
find-methods in comments). Bootloader entry (`0xe0000000`) is ARM; main fw is Thumb.

## Card-flags tool
`tools/card-flags/edit_card_flags.py <partition> --eos-develop --bootdisk --script` (needs numpy).
Flag offsets FAT32: EOS_DEVELOP@71, BOOTDISK@92, SCRIPT@496(0x1F0).
