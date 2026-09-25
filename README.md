# mac68k-disk - disk images for the classic 68k Macintosh

`mac68k-disk` creates and edits floppy disk images for the first Macs: **MFS**, the
400K format of the Macintosh 128K and 512K (64K ROM), and **HFS**, the format of the
800K floppy, the Mac Plus and everything after it (400K, 800K and 1440K images). It
puts files on a disk, lists them, copies them back and deletes them - one small C
program, no dependencies. The images work in Mini vMac, Basilisk II and other
emulators, and on real hardware via a floppy emulator or a disk writer.

For a long time `hfsutils` was the tool for this, but it has been removed from Debian 13
and it never handled 400K MFS disks. `mac68k-disk` covers both formats with a handful of
commands. It is also the natural companion of `mac68k-asm`, the 68k assembler
toolchain that writes applications as MacBinary files.

## Quick start

```
git clone https://github.com/bircher988/mac68k-disk.git
cd mac68k-disk
make                                  # needs only a C compiler and make
./mac68k-disk new Apps.dsk            # an empty 400K MFS disk named "Apps"
./mac68k-disk add Apps.dsk Hello.bin  # a MacBinary file from anywhere
./mac68k-disk ls -l Apps.dsk
```

Boot your System disk in the emulator with `Apps.dsk` as the second disk (for Mini
vMac: `minivmac System.dsk Apps.dsk`, or drag the image onto its window); the Finder
shows the disk and places the new icons itself.

## Installation

**Debian / Ubuntu / Raspberry Pi OS.** Every
[release](https://github.com/bircher988/mac68k-disk/releases) has a `.deb` for arm64
(Raspberry Pi) and amd64 (PC). Download it and install it with apt:

```
wget https://github.com/bircher988/mac68k-disk/releases/download/v1.0/mac68k-disk_1.0_arm64.deb
sudo apt install ./mac68k-disk_1.0_arm64.deb     # or _amd64.deb on a PC
```

To build the package yourself: `packaging/debian/build-deb.sh`.

**From source (Linux, macOS, any Unix).** `make` and then `sudo make install` puts the
binary into `/usr/local/bin`; `make uninstall` removes it again. `PREFIX=...` changes
the location.

In every case `mac68k-disk version` shows what is installed.

## Commands

```
mac68k-disk new   <image> [--hfs] [--size 400k|800k|1440k] [--name <volume name>]
mac68k-disk add   <image> <file>... [--name <mac name>] [--type TTTT --creator CCCC] [--rsrc <file>] [-f]
mac68k-disk ls    <image> [-l] [-R] [<folder>]
mac68k-disk get   <image> <mac name> [-o <out>] [--data|--rsrc]
mac68k-disk rm    <image> <mac name>
mac68k-disk mkdir <image> <folder>
mac68k-disk info  <image>
mac68k-disk version | help
```

**new** creates an empty disk: MFS 400K by default, which every 68k Mac can read; with
`--hfs` an HFS disk of 800K (`--size 400k`, `800k` or `1440k`). The volume name is the
image file name without its extension unless `--name` gives one (at most 27
characters). New disks have no boot blocks - they are data disks. `new` never
overwrites an existing file.

**add** copies files onto the disk. Each `<file>` is either a MacBinary file (versions
I, II and III, recognised by their header - the CRC for II and III, the zero fields and
exact fork lengths for I) or anything else, which becomes the data fork of a new file
with type and creator `????`. The Mac name is the one stored in the MacBinary header,
or the file name without a `.bin` extension.

| Option | Effect |
|---|---|
| `--name <mac name>` | the name on the disk (one file only); on HFS it may contain a folder path, `Games:Hello`, and a name ending in `:` puts all files into that folder |
| `--type TTTT --creator CCCC` | set file type and creator (four characters each; either one alone works too) |
| `--rsrc <file>` | the resource fork, as a raw file (one file only; with `--name` and without a data file it makes a file that only has a resource fork) |
| `-f` | replace a file of the same name (otherwise that is an error) |

Files keep their Finder flags (bundle, invisible, ...), except the ones that describe
their place on another disk: "on desktop", "inited" (icon placed), "changed" and
"busy" are cleared, the icon position and window are set to zero, so the Finder
places the icon itself the first time it opens the disk. Creation and modification
dates come from the MacBinary header, or are "now" for raw files.

**ls** lists the root folder or the given folder, folders with a trailing `:`. `-l`
adds type, creator, the sizes of data and resource fork and the modification date;
`-R` lists all folders below, with their paths.

```
$ mac68k-disk ls -lR System.dsk
TYPE CREA       DATA       RSRC  MODIFIED          NAME
APPL ????          0        516  2026-09-25 09:16  Hello
folder       2 items             2026-09-25 00:28  System Folder:
FNDR MACS          0     109195  1991-08-30 12:12  System Folder:Finder
ZSYS MACS        860     525073  1991-12-16 16:17  System Folder:System
```

**get** copies a file off the disk as a MacBinary II file `<name>.bin` in the current
directory, or `-o` names the output; `--data` or `--rsrc` write just that fork, as it
is; `-o -` writes to standard output.

**rm** deletes a file, or an empty folder. **mkdir** creates a folder (HFS only - MFS
has no folders). **info** shows format, volume name, allocation blocks, free space,
number of files and folders, dates and whether the disk has boot blocks and a System
Folder.

Names on the disk are Mac Roman. Names on the command line are UTF-8 and are converted
("Über", "Café"); other bytes are taken as they are. Names are compared the way the
Mac does it, without regard to case: `get Apps.dsk hello` finds `Hello`. A file or
folder name has at most 31 characters on HFS and 63 on MFS, and no `:`. On HFS, `:`
separates folders; a leading `:` is allowed (`:Games:Hello`).

Errors name the image and the problem, for example
`Apps.dsk: disk full (Big File needs 300 KB, 120 KB free)`. Every command reads the
whole image, makes its changes in memory and writes the image back through a
temporary file that is renamed over the original, so a failed command - a full disk,
a duplicate name, a damaged image - never changes the image at all.

## Images

The images are raw sector images (`.dsk`, `.img`, ...): 512-byte sectors, no header,
400K = 409600 bytes, 800K = 819200, 1440K = 1474560. This is what Mini vMac, Basilisk II
and most floppy emulators use. A DiskCopy 4.2 image (84-byte header) is recognised and
refused with a clear message, as are partitioned hard-disk images and HFS+ volumes.

**MFS** (Inside Macintosh II, File Manager). New 400K volumes have Apple's geometry:
the master directory block and the 12-bit block map in blocks 2-3, a 12-block file
directory, 391 allocation blocks of 1 KB. Files are allocated contiguously where
possible; directory entries never cross a block boundary. The Finder's folders on MFS
disks are an illusion of the Finder (kept in its Desktop file); `ls` shows the flat
list of files and new files appear in the disk window.

**HFS**. New volumes use the layout of Apple's disk initialization: the volume bitmap
from sector 3, 512-byte allocation blocks, extents overflow and catalog file of 1/128
of the disk each, an alternate master directory block in the last-but-one sector.
Changes read the whole catalog, apply the change and write the catalog back as a
freshly packed B-tree (leaf nodes, index levels, node map, header) into the catalog
file. When the catalog needs more room it grows: in place if the blocks behind it are
free, else by another extent, else it moves into free space. Folder valences, the file
and folder counts, the next catalog node ID, the free block count, the bitmap and the
alternate MDB are kept consistent. New files are allocated contiguously, in at most
three pieces per fork. Existing volumes made by Apple's system software or by other
tools are edited in place: boot blocks, the System Folder, Finder information and all
other records stay as they are.

The catalog keeps names in the Macintosh's case-insensitive order. `mac68k-disk`
follows the character ordering of Inside Macintosh I (International Utilities):
lowercase equal to uppercase, the non-breaking space with the space, typographic quotes
with the ASCII quote, accented letters of the original character set right after their
base letter. Existing records keep their order; only new ones are placed.

## Testing

`make test` (needs python3) runs `tests/run.sh`: round trips on MFS and HFS disks
(MacBinary I, II and III, raw files, a file with only a resource fork, an empty file, a
300K file, `--type`/`--creator`/`--rsrc`), comparing name, type, creator, Finder flags
and both forks bit for bit; replace, remove and add again (the free space must come
back); folders; 150 files and more in one folder, which forces a three-level catalog
B-tree and a catalog that grows and moves; 400K, 800K and 1440K HFS; error paths that
must leave the image unchanged; install and uninstall. After every change the image is
checked by `tests/mactest.py`, an independent structure checker for MFS and HFS (block
chains and bitmaps, B-tree links, key order, node maps, valences, counts, alternate
MDB).

If `hfsutils` is installed, `tests/oracle.sh` also checks interoperability: images
written by `mac68k-disk` are read by `hls`/`hcopy` with identical content, images made
by `hformat` and `hcopy` (including files with extents overflow records) are read and
changed by `mac68k-disk`, and names with punctuation and accented letters added by
either tool are found by the other.

The images have also been checked on the machines themselves, in Mini vMac: a Mac Plus
with System 6.0.8 opens 400K MFS and 800K HFS disks written by `mac68k-disk` and runs
the applications on them, boots from a System disk that got a new file, and Apple's
Disk First Aid reports "No repair necessary" for disks with 150 to 270 files, folders
and a moved catalog; a Macintosh 512K with the 64K ROM (MFS only) runs applications from
a 400K disk made by `mac68k-disk`.

## Limitations

- New files are stored in at most three pieces per fork; `mac68k-disk` writes no
  extents overflow records (it reads them, and removes them with the file). On a badly
  fragmented disk, adding a large file fails with a clear message.
- A catalog that already has extents overflow records (large, fragmented hard-disk
  volumes) cannot grow; adding fails with "catalog full" when it is full.
- The order of names in the catalog is documented by Apple for ASCII and for the
  letter groups, not for the order among different accents on the same letter
  (for example "ä" and "ã") or for the backquote. Existing records are never
  reordered, but a new name that differs from an existing one in the same folder only
  there may be placed where the Mac does not expect it.
- New disks are not startup disks (no boot blocks). To make one, start from a copy of
  an existing startup disk and add to it.
- MFS: `new` makes 400K volumes only; other MFS images can be read and changed. The
  Finder's MFS folders are not shown.
- Get Info comments in MacBinary files are ignored; file locking is not shown.

## Licence

mac68k-disk is MIT-licensed, see `LICENSE`. It was written from Apple's documentation of
the file systems (Inside Macintosh) and contains no code from other disk tools.
