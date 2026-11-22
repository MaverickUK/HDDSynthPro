/*
 * SCSI commands to a CD-ROM drive on macOS, for scsi_mac.py.
 *
 * macOS has no raw pass-through for a disc drive like Windows' SCSI
 * pass-through: the way in is IOKit's MMC device interface, the one disc
 * burning programs use. It needs exclusive access to the drive, which macOS
 * gives only while no volume on its disc is mounted (scsi_mac.py unmounts it
 * first; macOS mounts it again when this exits).
 *
 *     scsi_mac list        one line per drive: index|BSD name|vendor product revision
 *     scsi_mac open N      exclusive access to drive N, then commands over stdin/stdout
 *
 * After `open`, it prints one text line, "READY" or "ERROR <why>", then
 * answers binary requests until stdin closes (all integers little-endian):
 *
 *     request:  u8 cdb_len, u8 cdb[16], u8 direction (0 none, 1 in, 2 out),
 *               u32 length, u32 timeout_ms, then `length` bytes if out
 *     reply:    i32 IOReturn (0 = the command ran), u8 SCSI status,
 *               u8 sense[32], u32 bytes moved, u32 microseconds,
 *               then the bytes moved if in
 *
 * Built by scsi_mac.py on first use:
 *     clang -O2 -o scsi_mac scsi_mac.c -framework IOKit -framework CoreFoundation
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DATA (1024 * 1024)

/* Every authoring (MMC) device: CD, DVD and BD drives. */
static io_iterator_t find_drives(void) {
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR(kIOPropertySCSITaskDeviceCategory), CFSTR(kIOPropertySCSITaskAuthoringDevice));
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);
    io_iterator_t it = 0;
    /* 0 is the default main port, under its old name and its new one alike */
    if (IOServiceGetMatchingServices(0, match, &it) != KERN_SUCCESS)
        return 0;
    return it;
}

static void copy_string(CFTypeRef value, char *out, size_t size) {
    out[0] = 0;
    if (value && CFGetTypeID(value) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)value, out, (CFIndex)size, kCFStringEncodingUTF8);
}

static void describe(io_service_t drive, int index) {
    char vendor[64] = "", product[64] = "", revision[32] = "", bsd[32] = "";
    /* on the drive's object or one above it, depending on the macOS version */
    CFTypeRef chars = IORegistryEntrySearchCFProperty(drive, kIOServicePlane, CFSTR(kIOPropertyDeviceCharacteristicsKey),
                                                      kCFAllocatorDefault,
                                                      kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (chars && CFGetTypeID(chars) == CFDictionaryGetTypeID()) {
        CFDictionaryRef d = (CFDictionaryRef)chars;
        copy_string(CFDictionaryGetValue(d, CFSTR(kIOPropertyVendorNameKey)), vendor, sizeof vendor);
        copy_string(CFDictionaryGetValue(d, CFSTR(kIOPropertyProductNameKey)), product, sizeof product);
        copy_string(CFDictionaryGetValue(d, CFSTR(kIOPropertyProductRevisionLevelKey)), revision, sizeof revision);
    }
    if (chars)
        CFRelease(chars);
    /* the disc's BSD name (diskN) is on an IOMedia under the drive, while a disc is in */
    CFTypeRef name = IORegistryEntrySearchCFProperty(drive, kIOServicePlane, CFSTR("BSD Name"), kCFAllocatorDefault,
                                                     kIORegistryIterateRecursively);
    copy_string(name, bsd, sizeof bsd);
    if (name)
        CFRelease(name);
    printf("%d|%s|%s %s %s\n", index, bsd, vendor, product, revision);
}

static int list(void) {
    io_iterator_t it = find_drives();
    io_service_t drive;
    int n = 0;
    while (it && (drive = IOIteratorNext(it))) {
        describe(drive, n++);
        IOObjectRelease(drive);
    }
    if (it)
        IOObjectRelease(it);
    return 0;
}

static int read_all(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t got = read(0, p, n);
        if (got <= 0)
            return 0;
        p += got;
        n -= (size_t)got;
    }
    return 1;
}

static int write_all(const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t put = write(1, p, n);
        if (put <= 0)
            return 0;
        p += put;
        n -= (size_t)put;
    }
    return 1;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fail(const char *why, IOReturn err) {
    printf("ERROR %s (0x%08x)\n", why, (unsigned)err);
    fflush(stdout);
    return 1;
}

static int serve(int wanted) {
    io_iterator_t it = find_drives();
    io_service_t drive = 0, d;
    int n = 0;
    while (it && (d = IOIteratorNext(it))) {
        if (n++ == wanted)
            drive = d;
        else
            IOObjectRelease(d);
    }
    if (it)
        IOObjectRelease(it);
    if (!drive)
        return fail("no such drive", 0);

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn err = IOCreatePlugInInterfaceForService(drive, kIOMMCDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                                                     &plugin, &score);
    IOObjectRelease(drive);
    if (err != kIOReturnSuccess || !plugin)
        return fail("no MMC interface for the drive", err);
    MMCDeviceInterface **mmc = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID), (LPVOID *)&mmc);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || !mmc)
        return fail("QueryInterface for the MMC interface failed", (IOReturn)hr);
    SCSITaskDeviceInterface **dev = (*mmc)->GetSCSITaskDeviceInterface(mmc);
    if (!dev) {
        (*mmc)->Release(mmc);
        return fail("no SCSI task interface", 0);
    }
    err = (*dev)->ObtainExclusiveAccess(dev);
    if (err != kIOReturnSuccess) {
        (*dev)->Release(dev);
        (*mmc)->Release(mmc);
        return fail(err == kIOReturnBusy ? "the drive is busy: its disc is still mounted (diskutil unmountDisk)"
                                         : "no exclusive access to the drive",
                    err);
    }
    printf("READY\n");
    fflush(stdout);

    static uint8_t data[MAX_DATA];
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint8_t req[26];
    while (read_all(req, sizeof req)) {
        uint8_t cdb_len = req[0] > 16 ? 16 : req[0];
        uint8_t direction = req[17];
        uint32_t length = get32(req + 18), timeout_ms = get32(req + 22);
        uint8_t reply[45];
        memset(reply, 0, sizeof reply);
        if (length > MAX_DATA) {
            put32(reply, (uint32_t)kIOReturnBadArgument);
            if (direction == 2) { /* swallow what was sent */
                for (uint32_t left = length; left;) {
                    uint32_t chunk = left > MAX_DATA ? MAX_DATA : left;
                    if (!read_all(data, chunk))
                        goto done;
                    left -= chunk;
                }
            }
            if (!write_all(reply, sizeof reply))
                break;
            continue;
        }
        if (direction == 2 && length && !read_all(data, length))
            break;

        SCSITaskInterface **task = (*dev)->CreateSCSITask(dev);
        if (!task) {
            put32(reply, (uint32_t)kIOReturnNoMemory);
            if (!write_all(reply, sizeof reply))
                break;
            continue;
        }
        SCSITaskSGElement range;
        range.address = (mach_vm_address_t)(uintptr_t)data;
        range.length = length;
        UInt8 dir = direction == 1 ? kSCSIDataTransfer_FromTargetToInitiator
                    : direction == 2 ? kSCSIDataTransfer_FromInitiatorToTarget
                                     : kSCSIDataTransfer_NoDataTransfer;
        SCSI_Sense_Data sense;
        memset(&sense, 0, sizeof sense);
        SCSITaskStatus status = kSCSITaskStatus_No_Status;
        UInt64 moved = 0;
        uint64_t start = mach_absolute_time();
        err = (*task)->SetCommandDescriptorBlock(task, req + 1, cdb_len);
        if (err == kIOReturnSuccess)
            err = (*task)->SetScatterGatherEntries(task, &range, length ? 1 : 0, length,
                                                   length ? dir : kSCSIDataTransfer_NoDataTransfer);
        if (err == kIOReturnSuccess)
            err = (*task)->SetTimeoutDuration(task, timeout_ms);
        if (err == kIOReturnSuccess)
            err = (*task)->ExecuteTaskSync(task, &sense, &status, &moved);
        uint64_t ns = (mach_absolute_time() - start) * tb.numer / tb.denom;
        (*task)->Release(task);

        put32(reply, (uint32_t)err);
        reply[4] = (uint8_t)status;
        if (status == kSCSITaskStatus_CHECK_CONDITION)
            memcpy(reply + 5, &sense, sizeof sense < 32 ? sizeof sense : 32);
        uint32_t back = (direction == 1) ? (uint32_t)(moved > length ? length : moved) : 0;
        put32(reply + 37, back);
        put32(reply + 41, (uint32_t)(ns / 1000));
        if (!write_all(reply, sizeof reply) || (back && !write_all(data, back)))
            break;
    }
done:
    (*dev)->ReleaseExclusiveAccess(dev);
    (*dev)->Release(dev);
    (*mmc)->Release(mmc);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "list") == 0)
        return list();
    if (argc >= 3 && strcmp(argv[1], "open") == 0)
        return serve(atoi(argv[2]));
    fprintf(stderr, "usage: scsi_mac list | scsi_mac open N\n");
    return 2;
}
