 _   _  ____   ____      ____  __   __ _   _  _____  _   _
| | | ||  _ \ |  _ \    / ___| \ \ / /| \ | ||_   _|| | | |
| |_| || | | || | | |   \___ \  \ V / |  \| |  | |  | |_| |
|  _  || |_| || |_| |    ___) |  | |  | |\  |  | |  |  _  |
|_| |_||____/ |____/    |____/   |_|  |_| \_|  |_|  |_| |_|
                                       ____   ____    ___
                                      |  _ \ |  _ \  / _ \
                                      | |_) || |_) || | | |
                                      |  __/ |  _ < | |_| |
                                      |_|    |_| \_\ \___/

          An IDE hard disk, served from a microSD card.
                       Firmware {VERSION}

==============================================================================

WHY YOU'RE SEEING THIS

This is the drive's welcome disk. The drive serves it when it starts without
a microSD card, or when the card has no disk image it can use. Anything you
save here is kept only until the drive restarts.

GETTING STARTED

1. Turn the drive off.
2. On a computer, copy a disk image called HDDSYNTH.IMG to the root folder of
   a microSD card (FAT16, FAT32 or exFAT). An image is a raw copy of a whole
   hard disk, and its size (a multiple of 512 bytes) becomes the drive's.
3. Put the card in the drive and turn it on. The computer it's connected to
   now sees that disk instead of this one.

To serve an image with another name, or to change other settings, copy
HDDSYNTH.INI from this disk to the card's root folder and edit it.

THE SETTINGS FILE: HDDSYNTH.INI

Plain text, one setting per line as key = value. # starts a comment.

  device0 = HDDSYNTH.IMG The disk image to serve, from the card's root
                         folder. none: the built-in 64 MB test disk.
                         (Older files call it image, which still works.)
  device1 = none         A second drive, so the computer sees two hard
                         disks: an image, or a folder on the card served
                         read-only (experimental). Not the same image as
                         device0. cdrom, or an .ISO or .CUE file's name: a
                         CD-ROM drive instead (the bb48_hdcd firmware only).
                         none: no second drive.
  cdrom = HDDSYNTH.ISO   The CD-ROM drive's disc: an ISO (a data CD's
                         image), or a CUE sheet with its BIN (the first
                         track, its data; audio tracks are left out), on
                         the card, read-only. none: the built-in test disc.
                         Used by the CD-ROM firmware, and by bb48_hdcd with
                         device1 = cdrom.
  mode = auto            The transfer mode to offer the computer: auto (the
                         fastest both support), or pio0-pio4, mdma0-mdma2
                         or udma0-udma6 to allow at most that mode.
  dma = off              Whether auto may offer multiword DMA: on or off.
  write_behind = on      Acknowledge each written sector while storing it:
                         faster writes, but a power cut mid-store loses
                         that sector.
  read_ahead = on        Fetch the next sector while the computer reads
                         this one.
  activity_link = on     Tell an HDD Synth what the drive is doing.
  seek_timing = off      Hold each command as long as a modelled drive
                         would (seek, rotation, transfer): off; synth (the
                         drive the HDD Synth plays); or stepper85, voice91,
                         voice97 or fdb05.
  isa = off              on for a 16-bit ISA IDE card (286, 386, many
                         486s); off for anything later.
  early_answers = on     Have each register's value ready the moment it's
                         addressed, for controllers that don't wait for
                         the drive. off: faster on PCs that do wait (most
                         from the Pentium on), but not for ISA cards.
  iordy = on             off for a controller that checks IORDY before
                         each access (an Amiga 1200), with early_answers.
  geometry = auto        The cylinders/heads/sectors the BIOS sees: auto
                         (from the image's partition table), or e.g.
                         756/13/51 to match an old BIOS's setting.

The on/off settings also take yes/no, true/false and 1/0.

Changes take effect when the drive restarts. A mistake in the file never
stops the drive: it's reported on the serial console and skipped.

WHICH SETTINGS FOR YOUR COMPUTER

  286, 386, or a 486 with an ISA IDE card    the defaults, or isa = on
  A PCI controller (Pentium and later)       the defaults
  ... faster, if the controller waits        early_answers = off
      for the drive (never with ISA cards)
  A USB-IDE adapter that can do DMA          dma = on (or mode = mdma2)
  The CD-ROM through a USB-IDE adapter       mode = mdma2 (it asks for DMA)
  Amiga 1200 (not working yet)               iordy = off

Start with the defaults: every PC tried so far worked with them. If a
change doesn't suit the computer, the drive may not be found, or reads may
fail; put the setting back and restart.

WHICH DISK THE DRIVE SERVES

1. The image HDDSYNTH.INI names (HDDSYNTH.IMG if there's no settings file).
2. If that image isn't on the card: HDDSYNTH.IMG.
3. If there's no card, or no image can be used: this welcome disk.

LOOKING AFTER YOUR CARD

- The drive never formats the card. It writes only to the image it serves,
  and to HDDSYNTH.INI when you change settings from the serial console.
- Eject the disk on the computer before pulling the card out, as you would
  before unplugging any disk.

THE SERIAL CONSOLE

Connect a serial terminal to the drive's USB port at 115200 baud and press h
for help. S shows the card and its settings, and ! restarts the drive.

MORE

https://github.com/MaverickUK/HDDSynthPro
