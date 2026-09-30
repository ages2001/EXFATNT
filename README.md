# EXFATNT - exFAT File System Driver for Windows NT 3.51, NT 4.0, Windows 2000 and XP x86

An installable file system (IFS) driver that lets Windows NT 3.51, NT 4.0 and Windows 2000 mount exFAT volumes, the format of SDXC cards and most large USB flash drives. Microsoft only ever brought exFAT back as far as Windows XP / Server 2003 (KB955704).

## Status

Files and directories can be created, written, extended, truncated, renamed, moved and deleted; attributes, times and the volume label can be changed. Memory-mapped writes, the fast I/O write path and write-through work. Writing can be turned off in the registry (`EnableWriteSupport`).

Three tools come with it: `exfmt.exe` formats a volume as exFAT, `exfatchk.exe` checks and repairs one, and `exfachk.exe` does the same at boot time for volumes left dirty.

## Features

- **exFAT 1.00, following the Microsoft exFAT specification**
  - Boot region checksum, with fallback to the backup boot region
  - Up-case table (checksum verified, compressed form), used for all name comparisons
  - Allocation bitmap, next-fit allocation; streams stay contiguous (`NoFatChain`) as long as they can, and get a FAT chain once they cannot
  - Entry set checksums and name hashes, written and checked; damaged entry sets are skipped
  - `ValidDataLength` kept as exFAT keeps it: extending a file costs nothing until data is written past it
  - Timestamps with 10 ms increments and UTC offsets from the system time zone
  - Names up to 255 UTF-16 characters, surrogate pairs included
  - 512 to 4096-byte sectors, every cluster size from 512 bytes to 32 MB, files over 4 GB
  - `VolumeDirty` set before the first change and cleared once everything is on the disk (flush, lock, dismount, shutdown); a volume that was dirty at mount stays marked for a repair
- **A full IFS, not a converter**
  - Cache Manager integration for file data and metadata: cached reads and writes, fast I/O, memory-mapped files and executables, lazy writing
  - Non-cached I/O and direct volume (DASD) access
  - Directory listing with wildcards (`FileDirectoryInformation`, `FileFullDirectoryInformation`, `FileBothDirectoryInformation`, `FileNamesInformation`); subdirectories report `.` and `..`, the root does not
  - Set information: basic, disposition, rename/move (with replace), allocation, end of file
  - Change notification, byte-range locks, share access, delete-on-close
  - `FSCTL_IS_VOLUME_DIRTY`, `FSCTL_MARK_VOLUME_DIRTY`, flush of files and of the whole volume
  - Volume lock/unlock/dismount, so `exfmt`, `format` and disk tools can take the volume over; after a dismount with files still open, the next open mounts the volume again
  - Media change verification; removable and hot-plug media are written back when the last file closes
  - Write-protected media and TexFAT volumes are mounted read-only
  - Plug and Play removal on Windows 2000 (query-remove, surprise removal)
- **Tools** for NT 3.51, NT 4.0 and 2000 (see below)
  - `exfmt.exe`: format
  - `exfatchk.exe`: check and repair, like `chkdsk`
  - `exfachk.exe`: the boot-time check, like `autochk`; only volumes marked dirty are checked
- **One source tree, two binaries**
  - Windows NT 3.51 / NT 4.0 (x86): `exfatnt.sys` built with `EXF_NT4`
  - Windows 2000 and later (x86): `exfatnt.sys` built without it

## Architecture

```
            Application
                 ↓
          I/O Manager ─────────────── Cache Manager / Memory Manager
                 ↓                              ↑ paging I/O
        exfatnt.sys  ← This driver ─────────────┘
                 ↓
      Disk class driver (disk.sys) → ScsiPort / AHCINT / ATAPI ...
```

Metadata is cached through one internal stream per object: the FAT, the allocation bitmap and each directory. A cached page never mixes file data with metadata. File data is cached in the file's own section. Requests run synchronously in the caller's thread; only a close that arrives while the volume is busy is finished by a worker thread.

Locks are taken in one order: volume, file (deepest first), file paging I/O, allocation.

### Key Components

- **src\exfat.h** – Target switch (`EXF_NT4`), VCB/FCB/CCB structures, prototypes
- **src\exfdisk.h** – exFAT on-disk structures and constants
- **src\exfsup.c** – Checksums, up-case table, boot sector checks, entry set encoding and decoding, timestamps (no kernel calls)
- **src\exfinit.c** – `DriverEntry`, file system registration, shutdown notification
- **src\exfdisp.c** – IRP dispatch, exception handling, media change retry
- **src\exffsctl.c** – Mount, verify, lock/unlock/dismount, other FSCTLs, PnP
- **src\exfcreat.c** – Opening and creating files, directories and the volume; overwrite and supersede
- **src\exfdir.c** – Directory scanning, directory listing, change notification
- **src\exfdirw.c** – Writing entry sets: create, update, remove, move, directory growth, label
- **src\exfread.c** – Reads (cached, non-cached, paging, DASD)
- **src\exfwrite.c** – Writes (cached, non-cached, paging, DASD), `ValidDataLength`
- **src\exfsetin.c** – Set information: times and attributes, delete, rename, allocation, end of file
- **src\exfinfo.c**, **src\exfvol.c** – File and volume information, volume label
- **src\exfflush.c** – Flushing files and the volume, the dirty flag, shutdown
- **src\exfclose.c** – Cleanup, close, dismount and volume teardown
- **src\exfalloc.c** – Cluster chains, run lists, allocation bitmap, allocating and freeing clusters
- **src\exfio.c** – Metadata stream mapping, disk requests
- **src\exfstruc.c** – FCB/CCB management, internal streams, names, time conversion
- **src\exffast.c** – Fast I/O and Cache Manager callbacks
- **src\exfmisc.c** – Device control pass-through, byte-range locks
- **src\NT\** – `build.bat` for Windows NT 3.51 and 4.0
- **src\2K\** – `sources`, `makefile`, `exfatnt.rc` for the WDK
- **chk\** – `exfatchk.exe` and `exfachk.exe`: `exfchkc.c` checks and repairs (`exfupcw.c` brings in the up-case table of `fmt\`), `exfatchk.c` is the Windows front end, `exfachk.c` the native boot-time one; `NT\build.bat`, `2K\sources` and `2KBOOT\sources` build them
- **fmt\** – `exfmt.exe`: `exfmtc.c` lays out and writes the volume, `exfupc.c` holds the up-case table, `exfmt.c` is the Windows front end; `NT\build.bat` and `2K\sources` build it
- **bin\exfatnt.reg** – Service registration
- **test\** – User-mode test run (Linux)

## Building

### Windows NT 3.51 and 4.0

Needs Visual C++ 4.x, the Windows NT 4.0 DDK and the free `ntifs.h` (release 58) by Bo Brantén from <http://www.acc.umu.se/~bosse/>, since the NT4 DDK has no `ntifs.h`. An unchanged copy is included as `src\NT\ntifs.h` (GPL v2 or later, see its header).

1. Adjust `MSVCDIR` and `DDKDIR` at the top of `src\NT\build.bat`.
2. Run `build.bat` in `src\NT\`.

The same binary is meant for NT 3.51 and 4.0: it only imports kernel functions NT 3.51 already exports, and the 64-bit arithmetic helpers NT 3.51 lacks are linked in from `libcntpr.lib`. It has been tested on NT 3.51.

### Windows 2000 and XP x86

Needs WDK 6001.18002 (the Windows Server 2008 WDK).

1. Open the **Windows 2000 Free Build Environment** (or Checked, for debug output).
2. `cd src\2K`
3. `build -cZ`

The Windows 2000 binary also runs on Windows XP; see Installing for which XP versions need it.

### The tools

- **With Visual C++ 4.x** (runs on NT 3.51, NT 4.0, 2000 and later): adjust `MSVCDIR` in `fmt\NT\build.bat` and run it in `fmt\NT\`. No DDK needed.
- **With the WDK** (runs on 2000 and later): in the Windows 2000 build environment, `cd fmt\2K` and `build -cZ`. It uses the system `msvcrt.dll`, since the WDK's static C library needs functions Windows 2000 does not have.

`exfatchk.exe` and `exfachk.exe` are built the same way from `chk\`: `chk\NT\build.bat` builds both (it also needs the NT4 DDK for `ntdll.lib` and `libcntpr.lib`, because `exfachk.exe` is a native application without a C library), and with the WDK `chk\2K` builds `exfatchk.exe` and `chk\2KBOOT` builds `exfachk.exe`. The NT 4.0 builds of all three tools also run on 2000 and XP.

## Installing

### Which systems

| System | What to use |
|---|---|
| Windows NT 3.51, NT 4.0 | `exfatnt.sys` from `src\NT` |
| Windows 2000 | `exfatnt.sys` from `src\2K` |
| Windows XP without a service pack or with SP1 | `exfatnt.sys` from `src\2K` |
| Windows XP SP2 and later | Not needed: install Microsoft's own exFAT update (KB955704) |

Do not run EXFATNT and Microsoft's exFAT driver on the same system.

**Service packs:** if the driver does not work on NT 3.51, NT 4.0 or 2000 without the latest service pack (NT 3.51 SP5, NT 4.0 SP6a, 2000 SP4), install that service pack and try again.

### Steps

1. Copy `exfatnt.sys` to `%SystemRoot%\System32\drivers`.
2. Import `bin\exfatnt.reg` (double-click it, or `regedit /s exfatnt.reg`).
3. Restart.
4. Optionally copy `exfmt.exe` and `exfatchk.exe` to `%SystemRoot%\System32`, and run `exfatchk /INSTALL` from the folder that also holds `exfachk.exe` (see Checking).

### Settings

In `HKLM\SYSTEM\CurrentControlSet\Services\exfatnt`:

| Value | Type | Meaning |
|---|---|---|
| `EnableWriteSupport` | `REG_DWORD` | `1` (or missing): volumes can be written. `0`: every exFAT volume is mounted read-only; creating, writing, renaming and deleting fail with "The media is write protected". |

The value is read each time a volume is mounted, so a change applies to volumes mounted afterwards (after a restart, or when a removable disk is inserted again). Even with `0`, `exfmt` and `exfatchk /F` can still write to the volume, since they lock it and write to the disk directly.

exFAT partitions use partition type `07` on MBR disks, the same as NTFS, so drive letters are assigned as usual. To remove the driver, set `Start` to `4` under `HKLM\SYSTEM\CurrentControlSet\Services\exfatnt` (or delete the key) and restart.

Checked builds (and builds with `EXF_DEBUG` defined) print progress to the kernel debugger, prefixed with `[EXFATNT]`.

## Formatting

```
exfmt drive: [/V:label] [/A:size] [/F] [/X] [/Y]

  /V:label  Volume label, up to 11 characters
  /A:size   Cluster size, 512 to 32M; default 4K up to 256 MB, 32K up to 32 GB, 128K above
  /F        Full format: zero the whole volume, not just the metadata
  /X        Dismount the volume first if it is in use
  /Y        Do not ask for confirmation
```

For example `exfmt E: /V:USB /Y`. The drive needs a letter, so create the partition first (Disk Administrator on NT, Disk Management on 2000); any file system, or none, can be on it. `exfmt` locks the volume, writes the boot region and its backup, the FAT, the allocation bitmap, the up-case table and the root directory, sets the partition type to `07` and dismounts the old file system. The FAT and the cluster heap are aligned to the cluster size, and to 1 MB on volumes of 32 MB and more, counted from the start of the disk. The next access mounts the volume with `exfatnt.sys`.

The Explorer format dialog and `format /FS:exFAT` do not offer exFAT on NT 4.0 and 2000: they only format through a `U<name>.DLL` built on Microsoft's private C++ utility library (`ulib.dll`, `ifsutil.dll`), whose interface differs between releases. `exfmt` replaces them. For the same reason `chkdsk` and `autochk` do not know exFAT; `exfatchk` and `exfachk` replace them.

## Checking

```
exfatchk drive: [/F] [/X] [/V]
exfatchk /INSTALL | /UNINSTALL

  /F         Fix the problems found; the volume is locked meanwhile
  /X         Dismount the volume first if it is in use (implies /F)
  /V         Name every file and directory as it is checked
  /INSTALL   Check exFAT volumes marked dirty at every restart
  /UNINSTALL Stop checking at restart
```

Without `/F` nothing is written; on a volume in use, files being written at that moment can show up as problems. With `/F` the volume is locked, repaired through the volume handle, and mounted afresh afterwards. If it cannot be locked, `exfatchk` offers to mark it dirty so that it is repaired at the next restart. The exit code is `0` (no problems), `1` (all fixed), `2` (problems left) or `3` (not checked).

What is checked, and what `/F` does about it:

- Boot region and its backup: the damaged one is restored from the other; if both are damaged, the layout that matches the disk (FAT and root directory where it says) is kept and the region rebuilt around it
- FAT: its first two entries; every cluster chain for invalid cluster numbers, loops, chains that end too early or go on too long (cut at the last good cluster, the size cut with it)
- Cross-linked files: the file reached first keeps the clusters; the other ends where they begin. A stream that runs into another file's first cluster ends there, so a size made too large does not take the next file's data
- Entry sets: checksums, name hashes, names with characters not allowed or half surrogate pairs, duplicate names (the second is renamed), sizes, `ValidDataLength`, stream flags, timestamps; sets that cannot be read are deleted
- Directories: an end-of-directory mark followed by intact files (a sector of zeros) is turned into deleted entries so the files come back; entries after the real end are cleared; a "directory" that is not one becomes a file
- The up-case table (rewritten with the standard one if damaged) and the allocation bitmap (compared with every cluster in use, rewritten where they differ; made anew if its entry is lost)
- `PercentInUse` and the `VolumeDirty` flag, which is cleared once everything is fixed

Clusters that nothing refers to are freed; they are not saved into `FOUND.000` files. TexFAT volumes are only checked, not repaired.

`exfachk.exe` is the same check as a native application: `exfatchk /INSTALL` copies it to `System32` and adds `autocheck exfachk *` to `BootExecute` (under `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager`), after Windows' own `autochk`. At every start it reads the boot sector of each partition through the whole-disk device, so no file system gets mounted on the others, and checks and repairs only exFAT volumes whose `VolumeDirty` flag is set (left dirty by a crash, a power loss, a removed disk, or by `exfatchk` asking for a check at restart). Clean volumes are not scanned. `exfachk /p` checks every exFAT volume.

## Limitations

- exFAT has no 8.3 short names, so 16-bit programs may not see long-named files
- No opportunistic locks; network sharing works without them
- No security descriptors, extended attributes or named streams (exFAT has none)
- TexFAT volumes (two FATs) are mounted read-only
- `chkdsk`, `autochk` and the format dialog do not know exFAT; `exfatchk`, `exfachk` and `exfmt` replace them
- Checking a very large volume with small clusters needs memory: about 36 MB per 100 million clusters
- `exfmt` does not write a volume GUID entry or boot code: exFAT volumes are not bootable here

## Testing

`python3 test/runtests.py` (as root) builds the driver sources together with a small fake NT kernel (`test\ntifs.h`, `test\kern.c`) and runs them in user mode on Linux. The fake kernel has a real page cache, a lazy writer, memory pressure, worker threads and other threads that run at the worst moment (closes from the memory manager while a request is in the driver). It runs:

- **Reads** against images made with `mkfs.exfat` and the exfat-fuse driver: every cluster size from 512 B to 32 MB, 512 to 4096-byte sectors, fragmented files and directories, Unicode names, a 4.5 GB file, patched `ValidDataLength`, damaged boot regions and entry sets, lock/dismount/teardown, media change and PnP removal; again on a write-protected disk, where nothing may be written
- **Writes** (`test\wtest.c`): scripted cases and thousands of random operations (create, write cached and non-cached, fast I/O and mapped, extend, truncate, rename, delete, attributes, times, label, flush, remount, disk full) checked against a model; media change and surprise removal with dirty data in the cache; formatting a mounted volume with files open. Afterwards every image is checked with `test\exfcheck.py` (strict: boot region and backup, entry sets, chains, cross-links, the bitmap against the clusters in use), `fsck.exfat` and read back through exfat-fuse
- **exfmt**: every sector size and cluster size, then the write tests on the result
- **exfatchk** (`test\exfchkl.c`, `test\exfcorr.py`): every written image must check clean; images damaged in 27 known ways and at random (bytes changed in the boot region, FAT, bitmap and directories) must be repaired, check clean again, and pass `exfcheck.py`, `fsck.exfat` and exfat-fuse. The boot-time path (lock, repair through the volume, dismount) also runs against the driver
- **EnableWriteSupport = 0**: the read cases again, where nothing may be written

`python3 test/runtests.py --quick` runs a shorter set. It needs root, gcc, exfatprogs, exfat-fuse and dosfstools. This complements, but does not replace, testing on real Windows NT 4.0 and 2000.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

`src\NT\ntifs.h` is the free `ntifs.h` (release 58) by Bo Brantén, GPL v2 or later, included unchanged.
