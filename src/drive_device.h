// The device core 1 runs: hdcd::Device, which by HDDSYNTH.INI is a hard disk
// (ata::Device) as device 0, with a second disk, a CD-ROM drive
// (atapi::Device, docs/atapi.md) or nothing as device 1; or a CD-ROM drive
// alone as device 0. One build serves them all (0.32.0; before, bb48_cdrom
// and bb48_hdcd were builds of their own). ide_bus and loop1 use it through
// these names, the interface ata::Device and atapi::Device share.
#pragma once

namespace hdcd { class Device; struct CommandInfo; }
using DriveDevice = hdcd::Device;
using DriveCommandInfo = hdcd::CommandInfo;
