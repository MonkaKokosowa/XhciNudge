/*
 * XhciNudge
 *
 * fixes the super laggy usb on my lenovo loq 15arp9 (ryzen 7 7435HS) in macOS 15.
 * the amd xhci controllers in there (1022:161d, 161e, 15d6, 15d7) only have ONE msi vector,
 * but apple's AppleUSBXHCI sets up three interrupters per controller and puts transfer events
 * on interrupter 1. the controller never fires an interrupt for that one, so events just sit
 * there until something else wakes the driver up. in practice that was the MFINDEX wrap event
 * every ~2 seconds. so keys showed up seconds late, the mouse was a slideshow and usb drives
 * were painfully slow.
 *
 * what this does: every few ms it sends an IPI with each controller's own msi vector to the cpu
 * the msi would go to. xnu handles that exactly like a real msi from the controller, so apple's
 * interrupt handler runs and picks up whatever is waiting on all interrupters.
 *
 * it only reads stuff (pci config space and USBSTS), it never writes to the controllers.
 * controllers that are powered down, halted or have no driver yet get skipped, and the list is
 * rebuilt once a second because the vectors can change between boots.
 *
 * boot-arg: xhcinudge=<ms> sets the interval (default 4, i use 1). xhcinudge=0 turns it off.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

static const UInt32 kDefaultMs = 4;
static const unsigned kMaxCtl  = 8;

struct Ctl {
    IOPCIDevice *dev;
    IOMemoryMap *map;
    volatile UInt8 *op;     /* operational registers, nullptr if we couldn't map/read them */
    UInt8 msiOff, pmOff;
};

struct Target {
    UInt8 vector, dest;
};

static Ctl gCtl[kMaxCtl];
static unsigned gCtlCount;
static Target gTarget[kMaxCtl];
static unsigned gTargetCount;
static UInt32 gIntervalMs = kDefaultMs;
static UInt32 gTicksPerCheck;
static UInt32 gTick;
static thread_call_t gCall;

static bool gX2apic;
static IOMemoryMap *gLapicMap;
static volatile UInt8 *gLapicVa;

/* ------------------------------------------------------------------ local APIC */

static inline UInt64 rdmsr64(UInt32 msr)
{
    UInt32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UInt64)hi << 32) | lo;
}

static inline void wrmsr64(UInt32 msr, UInt64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((UInt32)v), "d"((UInt32)(v >> 32)));
}

/* fixed delivery, physical destination, same sequence xnu uses itself.
   local interrupts are off while we poke the icr so an interrupt handler on this cpu
   can't sneak its own ipi in halfway through. */
static void sendVector(UInt8 dest, UInt8 vector)
{
    UInt64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (gX2apic) {
        wrmsr64(0x830, ((UInt64)dest << 32) | (1U << 14) | vector);
    } else {
        volatile UInt32 *icrLo = (volatile UInt32 *)(gLapicVa + 0x300);
        volatile UInt32 *icrHi = (volatile UInt32 *)(gLapicVa + 0x310);
        while (*icrLo & (1U << 12)) {
            __asm__ volatile("pause");
        }
        *icrHi = (UInt32)dest << 24;
        *icrLo = (1U << 14) | vector;
    }
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" : : : "memory");
    }
}

/* ------------------------------------------------------------------ controllers */

static bool known(IOPCIDevice *dev)
{
    for (unsigned i = 0; i < gCtlCount; i++) {
        if (gCtl[i].dev == dev) {
            return true;
        }
    }
    return false;
}

/* look for xhci controllers (class 0x0C0330) we haven't seen yet */
static void scanControllers()
{
    OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
    if (it == nullptr) {
        return;
    }
    OSObject *obj;
    while ((obj = it->getNextObject()) && gCtlCount < kMaxCtl) {
        IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, obj);
        if (dev == nullptr || known(dev) || (dev->configRead32(0x08) >> 8) != 0x0C0330) {
            continue;
        }
        Ctl &c = gCtl[gCtlCount];
        bzero(&c, sizeof(c));
        dev->retain();
        c.dev = dev;
        dev->findPCICapability(kIOPCICapabilityIDMSI, &c.msiOff);
        dev->findPCICapability(kIOPCICapabilityIDPowerManagement, &c.pmOff);
        IODeviceMemory *bar = dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        c.map = bar ? bar->map(kIOMapInhibitCache) : nullptr;
        if (c.map != nullptr && c.map->getLength() > 0x100) {
            volatile UInt8 *base = (volatile UInt8 *)c.map->getVirtualAddress();
            UInt8 capLen = base[0];
            if (capLen != 0xFF && capLen >= 0x20) {
                c.op = base + capLen;
            }
        }
        gCtlCount++;
    }
    it->release();
}

/* rebuild the list of controllers that can actually take an interrupt right now */
static void refreshTargets()
{
    unsigned n = 0;
    for (unsigned i = 0; i < gCtlCount; i++) {
        Ctl &c = gCtl[i];
        if (c.msiOff == 0 || c.op == nullptr) {
            continue;
        }
        if ((c.dev->configRead32(0x00) & 0xFFFF) == 0xFFFF) {
            continue;   /* gone */
        }
        if (c.pmOff && (c.dev->configRead16(c.pmOff + 4) & 3) != 0) {
            continue;   /* not in D0, leave it alone */
        }
        if (!(c.dev->configRead16(0x04) & 0x2)) {
            continue;   /* memory decoding off */
        }
        UInt16 ctl = c.dev->configRead16(c.msiOff + 2);
        if (!(ctl & 1)) {
            continue;   /* msi off, so apple's driver isn't attached yet */
        }
        UInt32 sts = *(volatile UInt32 *)(c.op + 4);
        if (sts == 0xFFFFFFFF || (sts & 1)) {
            continue;   /* reads all ones or the controller is halted */
        }
        UInt32 addr = c.dev->configRead32(c.msiOff + 4);
        UInt16 data = c.dev->configRead16(c.msiOff + ((ctl & (1U << 7)) ? 0xC : 8));
        UInt8 vec = data & 0xFF;
        if (vec < 0x20 || (addr & 0xFFF00000) != 0xFEE00000) {
            continue;
        }
        Target t = {vec, (UInt8)((addr >> 12) & 0xFF)};
        if (gTarget[n].vector != t.vector || gTarget[n].dest != t.dest || n >= gTargetCount) {
            IOLog("XhciNudge: %s vector 0x%02x -> APIC %u\n", c.dev->getName(), t.vector, t.dest);
        }
        gTarget[n++] = t;
    }
    gTargetCount = n;
}

static void tick(thread_call_param_t, thread_call_param_t)
{
    if (gTick % gTicksPerCheck == 0) {
        scanControllers();
        refreshTargets();
    }
    gTick++;
    for (unsigned i = 0; i < gTargetCount; i++) {
        sendVector(gTarget[i].dest, gTarget[i].vector);
    }
    uint64_t deadline;
    clock_interval_to_deadline(gIntervalMs, kMillisecondScale, &deadline);
    thread_call_enter_delayed(gCall, deadline);
}

/* ------------------------------------------------------------------ driver */

class XhciNudge : public IOService {
    OSDeclareDefaultStructors(XhciNudge)
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(XhciNudge, IOService)

bool XhciNudge::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    UInt32 arg;
    if (PE_parse_boot_argn("xhcinudge", &arg, sizeof(arg))) {
        gIntervalMs = arg;
    }
    if (gIntervalMs == 0) {
        IOLog("XhciNudge: disabled by boot-arg xhcinudge=0\n");
        return true;
    }
    gTicksPerCheck = 1000 / gIntervalMs ? 1000 / gIntervalMs : 1;
    gX2apic = (rdmsr64(0x1B) & (1ULL << 10)) != 0;
    if (!gX2apic) {
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(0xFEE00000, 0x1000, kIODirectionInOut);
        gLapicMap = md ? md->map(kIOMapInhibitCache) : nullptr;
        if (md) {
            md->release();
        }
        if (gLapicMap == nullptr) {
            IOLog("XhciNudge: cannot map the local APIC, disabled\n");
            return true;
        }
        gLapicVa = (volatile UInt8 *)gLapicMap->getVirtualAddress();
    }
    gCall = thread_call_allocate(tick, nullptr);
    if (gCall == nullptr) {
        IOLog("XhciNudge: thread_call_allocate failed, disabled\n");
        return true;
    }
    IOLog("XhciNudge: 1.0 nudging XHCI controllers every %u ms (%s)\n", gIntervalMs, gX2apic ? "x2apic" : "xapic");
    thread_call_enter(gCall);
    registerService();
    return true;
}
