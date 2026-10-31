HDD Synth Pro benchmark image
=============================

This disk boots FreeDOS and runs HDDBENCH, which measures the disk through
the BIOS (INT 13h) and checks its data:

  Sequential read   16 MB from the pattern area
  Random read       500 single sectors anywhere on the disk
  Sequential write  8 MB to the scratch area
  Check written     the 8 MB just written, read back
  Check pattern     the whole pattern area (32 MB), read back

Then, after 10 s unless a key is pressed, a soak repeats the checks until a
key stops it. Every result is appended to C:\BENCH.LOG.

  HDDBENCH          the benchmark, then the soak
  HDDBENCH /NOSOAK  the benchmark only
  HDDBENCH /SOAK    the soak only

The pattern and scratch areas lie outside the FAT partition, so the tests
never touch your files. Each run gives the same reads and writes in the same
order on every machine, so results compare between machines.

FreeDOS is free software (GPL): https://www.freedos.org/
