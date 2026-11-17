/*
 * IDE Synth Bay: the architecture as C4 diagrams (Structurizr DSL).
 * How to view and export them: README.md beside this file.
 * Keep it to what the code does; the details live in CLAUDE.md and docs/.
 */
workspace "IDE Synth Bay" "An IDE hard disk and CD-ROM drive emulated by an RP2350, served from a microSD card." {

    model {
        owner = person "Owner" "Sets the drive up (HDDSYNTH.INI, serial console), builds the firmware, runs the test rig."

        host = softwareSystem "IDE host" "What the drive is plugged into: a retro PC's IDE controller (16-bit ISA card, Intel PIIX, VIA) or a USB-IDE adapter (JMicron, Innostor). Drives the 40-pin IDE bus: register reads and writes, PIO and multiword DMA data, RESET#." "External"
        synth = softwareSystem "HDD Synth" "Plays hard disk sounds from what the drive is doing (HDDSynth_Native)." "External"
        devMachine = softwareSystem "Development computer" "Mac or Windows PC: PlatformIO, the USB adapter the rig tests drive through, USB serial to the board." "External"

        bay = softwareSystem "IDE Synth Bay" "Emulates an IDE hard disk (and an ATAPI CD-ROM drive) on an RP2350B: answers the IDE bus in real time and serves sectors from disk images on a microSD card." {

            core1 = container "Core 1: bus loop" "The real-time half. Answers every register access the PIO hands it, runs the ATA/ATAPI command state machines, starts DMA bursts. Never waits: everything it runs per access is in RAM (CORE1_RAM), never touches USB or the card." "C++ (Arduino-Pico), RAM-resident" "Firmware" {
                ideBus = component "ide_bus" "Loads and serves the bus program: ide_bus, early answers, the data lane, ide_bus_noiordy. Switches early answers <-> the lane live (bus = auto), holds IORDY while core 1 answers, drives INTRQ. The data lane streams data-in words by DMA, without core 1." "ide_bus.cpp"
                ideTable = component "ide_table" "The register table: every register read answered by the PIO from values core 1 keeps ready, for hosts that ignore IORDY." "ide_table.cpp, ide_table_write.cpp"
                ideDma = component "ide_dma" "Multiword DMA's data phase: hands DD0-15 to its PIO block for a burst, feeds it by a DMA channel, drives DMARQ." "ide_dma.cpp"
                ataDevice = component "ata::Device" "The hard disk: task file, status, IDENTIFY (patched live when DMA is added), READ/WRITE SECTORS, MULTIPLE and DMA, read-ahead, write-behind, two units." "ata_device.cpp, identify.cpp"
                atapiDevice = component "ATAPI CD-ROM" "PACKET commands, MMC (READ(10), TOC, GET EVENT), disc changes; hdcd::Device puts it beside the hard disk." "atapi_device.cpp, mmc.cpp, hdcd_device.cpp"
                busAutoStep = component "busAutoStep" "bus = auto on core 1: applies the DMA patch between commands and switches the program at a quiet moment or while RESET# is held." "main.cpp (CORE1_RAM_RARE)"
                traceRing = component "Trace ring" "Commands, completions and (on request) every bus access, recorded for core 0 to print. Lock-free SPSC ring." "spsc_ring.h, access_log"
                fetchMailbox = component "Fetch mailbox" "The handshake between the cores: core 1 posts a fetch (LBA, buffer, read or write), core 0 posts the result. Shared memory." "fetch_mailbox.h"
            }

            core0 = container "Core 0: services" "Everything slow: the card, USB serial, the activity link, the diagnostics' reports, the bus = auto advice. Keeps up with core 1's fetches before anything else." "C++ (Arduino-Pico)" "Firmware" {
                fetchService = component "Fetch service" "serveFetches(): takes core 1's fetches from the mailbox, reads or writes the block source, holds results for seek timing. Per-sector code in RAM (CORE0_RAM)." "main.cpp"
                blockSources = component "Block sources" "The disks served: an image on the card (ImageDisk, split images), a folder as a read-only FAT16 disk, the welcome disk, the generated test disk, with a RAM overlay for writes; ISO and BIN/CUE discs for the CD-ROM." "block_source.h, gen_disk, welcome_disk, folder_disk, image_parts, cue_sheet"
                sdCard = component "SD card driver" "SdFat over SPI (BB48) or 4-bit SDIO (PCB), high-speed mode with calibration, retries, card removal and return." "sd_card.cpp, sdio_card.cpp, card_retry, media_watch"
                driveConfig = component "Settings and disks" "Reads HDDSYNTH.INI, chooses the image and the bus program, console E/C edits the file." "drive_config.cpp, disks.cpp"
                console = component "Serial console" "Commands (h, s, S, P, M, T, J...), the trace and report lines (CMD, DONE, AUTOBUS, SLOWREAD...)." "console.cpp, trace_format.h"
                autoBus = component "auto_bus advisor" "From the diagnostics' counters: is the host an ISA card, does it wait for IORDY, is detection over? Advises early answers or the lane; once learned, adds DMA (dma = auto)." "auto_bus.cpp, offerDmaToLearnedHost()"
                pioPlan = component "pio_plan" "Shares the three PIO blocks out: the bus program, the card (SDIO), then DMA or IOCS16# and the diagnostics." "pio_plan.h"
                diagnostics = component "Diagnostics" "IORDY timing (slow reads, reads the host didn't wait for), the bus observer checking data and IDENTIFY on the cable, the read timing probe, stack watermarks." "iordy_timer, bus_observer, sector_check, bus_timing, stack_watch"
                activityLink = component "Activity link" "Classifies the recent commands (idle, sequential, random), models seek and rotation timing, sends $HDA/$HDE to the synth and takes $HDM back." "activity.cpp, activity_link.cpp, link.cpp, seek_timing.cpp"
            }

            pio = container "PIO programs" "State machines that meet the IDE bus's nanosecond timing: sample every access, pull IORDY low within 35 ns, drive DD for reads, move DMA data. Three blocks of 32 instructions." "RP2350 PIO assembly (src/*.pio)" "PIO" {
                pioBus = component "Bus program" "One of ide_bus, ide_bus_early, ide_bus_data + ide_data_in, ide_bus_noiordy, or ide_table (+ ide_table_write), with iordy_watchdog for ISA cards." "ide_bus.pio, ide_table.pio"
                pioDma = component "ide_dma" "Multiword DMA read and write bursts." "ide_dma.pio"
                pioIocs16 = component "iocs16" "Pulls IOCS16# low for the data register, for 16-bit ISA cards." "iocs16.pio"
                pioDiag = component "Diagnostic programs" "iordy_timer, iordy_check, bus_observer, bus_timing." "*.pio"
                pioSdio = component "SDIO" "The card's 4-bit bus (PCB)." "sdio programs"
            }

            card = container "microSD card" "FAT32: disk images (HDDSYNTH.IMG and others), ISOs and BIN/CUEs, HDDSYNTH.INI, the scratch HDDBENCH.IMG. Holds the owner's data: the firmware only touches the image served, HDDBENCH.IMG and HDDSYNTH.INI." "FAT32" "Storage"

            rig = container "Rig and tools" "Host-side tests and tools: pytest slices through the USB adapter, the PIO simulator, the trace parser, the benchmark disk image, rig.sh (build, flash, test)." "Python, Bash" "Tools" {
                rigTests = component "Rig tests" "pytest, one marker per slice: detection, reads, writes, speed, soak, CD-ROM, the link." "host/tests"
                pioSim = component "PIO simulator and host models" "Runs the real pioasm output against PIO-mode and MDMA host models, holding the programs to ATA's timings (pytest -m sim, no hardware)." "pio_sim.py, ide_host_model.py, mdma_host_model.py"
                driveLog = component "Drive log and trace parser" "Reads the board's serial console, parses CMD/DONE/EV lines." "drive_log.py, trace.py"
                hostIo = component "Host disk access" "Raw reads and writes to the emulated disk through the adapter, per OS." "host.py, host_macos.py, host_windows.py"
                benchImage = component "Benchmark image" "DOSBENCH.IMG: FreeDOS and HDDBENCH.COM for retro PCs." "tools/bench-image"
            }
        }

        # System context
        owner -> bay "Configures and watches" "HDDSYNTH.INI, USB serial console"
        host -> bay "Reads and writes sectors, sends ATA and ATAPI commands" "40-pin IDE (ATA PIO 0-3, MDMA 0-2)"
        bay -> synth "Tells what the drive is doing" "UART, $HDA/$HDE sentences"
        synth -> bay "Says which drive it models" "UART, $HDM"
        owner -> devMachine "Builds, flashes and tests on"
        devMachine -> bay "Flashes, reads the console, drives tests through the USB adapter" "USB, UF2"

        # Containers
        host -> pio "Strobes registers and data" "IDE bus"
        pio -> host "Answers reads, holds IORDY, DMARQ" "IDE bus"
        pio -> core1 "Hands each access over, asks for read answers" "PIO FIFOs"
        core1 -> pio "Answers reads, loads and switches programs" "PIO FIFOs, registers"
        core1 -> core0 "Posts fetches, records trace events" "Shared RAM (mailbox, SPSC ring)"
        core0 -> core1 "Posts fetch results, wanted bus program, DMA patch" "Shared RAM"
        core0 -> card "Reads and writes images, reads settings" "SPI or 4-bit SDIO"
        core0 -> pio "Runs the diagnostics and the card's programs" "PIO FIFOs, DMA"
        rig -> core0 "Console commands, reads trace lines" "USB serial"
        rig -> host "Raw sector reads and writes" "USB mass storage (through the adapter)"

        # Core 1's components
        pioBus -> ideBus "Samples: each access, answer requests" "RX FIFO"
        ideBus -> pioBus "Register values to drive onto DD" "TX FIFO"
        ideTable -> pioBus "Register values kept in the table" "PIO registers"
        ideBus -> ataDevice "Register reads and writes, data words"
        ideBus -> atapiDevice "Register reads and writes, PACKET data"
        ideDma -> pioDma "Burst counts, DD hand-over" "PIO, DMA channel"
        ataDevice -> ideDma "DMA bursts (a sector each)"
        ataDevice -> fetchMailbox "Posts a fetch, takes the result"
        atapiDevice -> fetchMailbox "Posts a fetch, takes the result"
        ataDevice -> traceRing "CMD and DONE events"
        busAutoStep -> ideBus "Switches the program"
        busAutoStep -> ataDevice "Adds DMA to IDENTIFY (offerDma)"

        # Core 0's components
        fetchService -> fetchMailbox "Takes fetches, posts results"
        fetchService -> blockSources "Reads and writes sectors"
        blockSources -> sdCard "Reads and writes the image's clusters"
        sdCard -> card "Commands and data" "SPI or SDIO"
        sdCard -> pioSdio "Runs the card's bus" "PIO"
        driveConfig -> card "Reads HDDSYNTH.INI, edits it on E"
        driveConfig -> blockSources "Chooses the disks served"
        console -> traceRing "Prints its events" "SPSC ring"
        console -> autoBus "Feeds the counters, prints AUTOBUS"
        autoBus -> diagnostics "Reads its evidence: DIOR# with no chip select, reads not waited for, long reads"
        autoBus -> busAutoStep "Wanted program; the DMA patch" "Shared flags"
        autoBus -> pioPlan "Frees the diagnostics' or IOCS16#'s block for ide_dma"
        autoBus -> ideDma "Starts it (dma = auto)"
        pioPlan -> pioDiag "Starts what fits"
        pioPlan -> pioIocs16 "Starts it when DMA isn't offered"
        diagnostics -> pioDiag "Reads timings and bus samples" "PIO FIFOs, DMA ring"
        activityLink -> traceRing "Takes CMD and DONE events"
        activityLink -> fetchService "Holds results for seek timing"
        activityLink -> synth "$HDA, $HDE" "UART"
        synth -> activityLink "$HDM" "UART"

        # The rig's components
        rigTests -> driveLog "Console commands, trace lines"
        rigTests -> hostIo "Sector reads and writes"
        driveLog -> console "Commands and lines" "USB serial"
        hostIo -> host "Raw I/O" "USB mass storage"
        pioSim -> pioBus "Runs the assembled programs against host models" "pioasm output"
        benchImage -> host "Booted by a retro PC from the drive"

        # Deployment
        pcb = deploymentEnvironment "PCB" {
            deploymentNode "IDE Synth Bay PCB" "Rev 0.3/0.4: RP2350B, 3.3 V-5 V bus switches, microSD on SDIO, 40-pin IDE header" "Hardware" {
                deploymentNode "RP2350B" "Two Cortex-M33 cores at 200 MHz, 520 KB RAM, three PIO blocks" "Microcontroller" {
                    containerInstance core1
                    containerInstance core0
                    containerInstance pio
                }
                deploymentNode "microSD socket" "4-bit SDIO, 20-40 MHz" "Socket" {
                    containerInstance card
                }
            }
            deploymentNode "Retro PC or USB-IDE adapter" "" "IDE host" {
                softwareSystemInstance host
            }
            deploymentNode "Dev computer" "macOS or Windows" "Computer" {
                containerInstance rig
            }
        }
        bb48 = deploymentEnvironment "BB48" {
            deploymentNode "Olimex RP2350-PICO2-BB48" "The proof of concept: RP2350B on a breadboard, hand-wired to an IDE cable" "Hardware" {
                deploymentNode "RP2350B" "Two Cortex-M33 cores at 200 MHz, 520 KB RAM, three PIO blocks" "Microcontroller" {
                    containerInstance core1
                    containerInstance core0
                    containerInstance pio
                }
                deploymentNode "microSD breakout" "SPI, 25 MHz" "Socket" {
                    containerInstance card
                }
            }
            deploymentNode "USB-IDE adapter or retro PC" "" "IDE host" {
                softwareSystemInstance host
            }
            deploymentNode "Dev computer" "macOS or Windows" "Computer" {
                containerInstance rig
            }
        }
    }

    views {
        systemContext bay "Context" "Who and what IDE Synth Bay talks to." {
            include *
            autoLayout lr
        }

        container bay "Containers" "The two cores, the PIO programs, the card and the rig." {
            include *
            autoLayout lr
        }

        component core1 "Core1" "Core 1: the real-time bus loop." {
            include *
            autoLayout lr
        }

        component core0 "Core0" "Core 0: the card, the console, the advice, the link." {
            include *
            autoLayout lr
        }

        component pio "PIO" "The PIO programs, shared out by pio_plan." {
            include *
            autoLayout lr
        }

        component rig "Rig" "The host-side tests and tools." {
            include *
            autoLayout lr
        }

        dynamic bay "SectorRead" "A READ SECTORS on the hard disk: core 1 never waits for the card; BSY covers the fetch, never IORDY (CLAUDE.md, Drive data path)." {
            host -> pio "Writes the task file and the Command register"
            pio -> core1 "Each write's sample"
            core1 -> core0 "ata::Device sets BSY and posts a fetch (LBA, buffer) to the mailbox"
            core0 -> card "Reads the sector from the image (read-ahead: the next one too)"
            core0 -> core1 "Posts the result; fetchDone() sets DRQ and raises INTRQ"
            host -> pio "Reads Status, then 256 data words"
            pio -> core1 "Each read (the data lane streams the words by DMA instead)"
            core1 -> pio "Drives each word onto DD, releases IORDY"
            core1 -> core0 "DONE in the trace ring: the console and the activity link"
            autoLayout lr
        }

        dynamic bay "AutoBus" "bus = auto with dma = auto: the drive learns the PC, moves to the data lane, then offers DMA." {
            host -> pio "BIOS and DOS read on early answers; status reads held 1.5 us now and then (the IORDY probe)"
            pio -> core1 "Every access"
            core1 -> core0 "Fetches; the diagnostics count long reads, reads not waited for, DIOR# with no chip select"
            core0 -> core1 "1,000 sectors and 20 waited reads later: 'lane'; then the DMA patch"
            core1 -> pio "Switches to the data lane between accesses; patches IDENTIFY"
            host -> pio "Windows' driver reads IDENTIFY again: multiword DMA 2"
            autoLayout lr
        }

        deployment bay pcb "DeploymentPCB" "The drive on its own PCB." {
            include *
            autoLayout lr
        }

        deployment bay bb48 "DeploymentBB48" "The proof of concept on the Olimex BB48." {
            include *
            autoLayout lr
        }

        styles {
            element "Person" {
                shape Person
                background #08427b
                color #ffffff
            }
            element "Software System" {
                background #1168bd
                color #ffffff
            }
            element "External" {
                background #999999
                color #ffffff
            }
            element "Container" {
                background #438dd5
                color #ffffff
            }
            element "PIO" {
                background #2e7d5b
            }
            element "Storage" {
                shape Cylinder
            }
            element "Tools" {
                background #6a6a8a
            }
            element "Component" {
                background #85bbf0
                color #000000
            }
        }
    }

}
