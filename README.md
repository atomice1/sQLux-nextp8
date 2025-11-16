sQLux-nextp8
============

This is a software model (emulator) for the nextp8 core, based on sQLux.

It is hacked from the sQLux Sinclar QL emulator.

# Usage

* Copy `sdspi.cpp`, `sdspi.h`, `sdspisim.cpp` and `sdspisim.h` from `nextp8-core/c_models`.
* Copy `p8_audio.c`, `p8_audio.h`, `p8_dsp.c`, `p8_dsp.h`, `queue.h` from `femto8`.
* Build sQLux according to the instructions in the [upstream README](README_upstream.md).
* Build the m68k-elf-gcc toolchain.
* Build nextp8-bsp.
* Build nextp8-loader.
* Build femto8-nextp8.
* Create a link to the bootloader in the roms directory:
```
ln -s ../../nextp8-loader/build/loader.bin roms/
```
* Make an SD card image with the femto8-nextp8 binary on it:
```
dd if=/dev/zero of=sdcard.img bs=1024 count=8192
mkfs.vfat sdcard.img
mkdir -p /tmp/sdcard
sudo mount -o loop,uid=${LOGNAME} sdcard.img /tmp/sdcard
cp ../femto8-nextp8/build-nextp8/femto8.bin /tmp/sdcard/nextp8/nextp8.bin
sudo umount /tmp/sdcard
```
* Start the model:
```
./build/sqlux --rom1 loader.bin
```
