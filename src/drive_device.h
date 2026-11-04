// The device core 1 runs, chosen per build: the hard disk (ata::Device), or
// in the CD-ROM build (HDDPRO_CDROM, platformio.ini's bb48_cdrom) the CD-ROM
// drive (atapi::Device, docs/atapi.md). Both offer ide_bus and loop1 the same
// interface, so the bus code is written once and the hard disk build compiles
// to exactly what it did before the CD-ROM existed.
#pragma once

#ifndef HDDPRO_CDROM
#define HDDPRO_CDROM 0
#endif

// The bb48_hdcd build (HDDPRO_HDCD): a hard disk as device 0 and, with
// `device1 = cdrom`, a CD-ROM drive as device 1 (hdcd::Device). Without a
// CD-ROM it serves what the hard disk build does.
#ifndef HDDPRO_HDCD
#define HDDPRO_HDCD 0
#endif

#if HDDPRO_CDROM
namespace atapi { class Device; struct CommandInfo; }
using DriveDevice = atapi::Device;
using DriveCommandInfo = atapi::CommandInfo;
#elif HDDPRO_HDCD
namespace hdcd { class Device; struct CommandInfo; }
using DriveDevice = hdcd::Device;
using DriveCommandInfo = hdcd::CommandInfo;
#else
namespace ata { class Device; struct CommandInfo; }
using DriveDevice = ata::Device;
using DriveCommandInfo = ata::CommandInfo;
#endif
