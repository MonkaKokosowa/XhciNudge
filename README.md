# XhciNudge

A tiny macOS kext that fixes the horrible USB lag I had on my AMD laptop (Lenovo LOQ 15ARP9, Ryzen 7 7435HS)
running macOS 15 with OpenCore.

Without it, keyboard presses showed up seconds late, the mouse moved in jumps and USB drives were super slow.
With it, everything feels normal.

## what was going on

The USB (XHCI) controllers in this laptop are AMD `1022:161D`, `161E`, `15D6` and `15D7`. Each of them only has
one MSI interrupt vector.

Apple's driver (AppleUSBXHCI) sets up three interrupters per controller and puts a lot of the events, like transfer
completions, on interrupter 1. These controllers never raise an interrupt for that one. So the events just sit in
memory until the driver wakes up for some other reason. On my laptop that only happened about every 2 seconds,
when the controller's frame counter wrapped around.

I found this by writing a debug kext that counted interrupts and watched the controller registers. If you want the
deep dive, the old AppleUSBXHCI source Apple published (IOUSBFamily 560) shows the interrupter setup pretty clearly.

## what the kext does

Every few milliseconds it sends an inter-processor interrupt to the CPU, using each controller's own MSI vector.
XNU can't tell the difference between that and a real MSI from the controller, so Apple's interrupt handler runs
and collects everything that's waiting.

Some details:

- once a second it looks for XHCI controllers (PCI class `0x0C0330`) and rebuilds its list
- a controller only gets nudged when it's in D0, has memory decoding on, has MSI enabled (so Apple's driver is
  attached) and isn't halted. Powered down controllers get skipped
- it reads PCI config space and `USBSTS`, and never writes anything to the controllers
- works with xAPIC and x2APIC
- no Lilu needed

It's a workaround. The proper fix would be Apple's driver using one interrupter on these controllers, but I can't
change Apple's driver, so here we are. It costs one extra interrupt per controller per interval, which I didn't
notice at all.

## boot-args

| boot-arg | what it does |
|---|---|
| none | nudge every 4 ms |
| `xhcinudge=<ms>` | nudge every `<ms>` milliseconds. I use `xhcinudge=1`, USB storage is a lot faster with it |
| `xhcinudge=0` | kext loads but does nothing |

## installing

Grab `release/XhciNudge-1.0.0.zip`, put `XhciNudge.kext` in `EFI/OC/Kexts` and add it to `Kernel -> Add`:

| key | value |
|---|---|
| BundlePath | `XhciNudge.kext` |
| ExecutablePath | `Contents/MacOS/XhciNudge` |
| PlistPath | `Contents/Info.plist` |
| Arch | `x86_64` |
| Enabled | `true` |

Bundle id is `local.XhciNudge`. I've only tested it on macOS 15 (Sequoia) on my own laptop. If your machine has
the same kind of single-vector AMD XHCI controllers it might help you too, but no promises.

If you also get USB drives disconnecting under heavy use or when memory gets full, that's a different problem
with the same controllers. Check out `KernReserveLow`.

## building

You don't need a Mac. I built it on Linux with:

- clang (21 in my case)
- `ld64` from [cctools-port](https://github.com/tpoechtrager/cctools-port)
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)

```sh
make SDK=/path/to/MacKernelSDK LD64=/path/to/x86_64-apple-darwin-ld
# result: build/XhciNudge.kext
```

Newer clang wants to import `___cxa_atexit`, which the kernel doesn't export, so the Makefile passes
`-fno-use-cxa-atexit -mllvm -disable-atexit-based-global-dtor-lowering`. If you see that symbol in
`nm -u`, that's why.

## files

- `XhciNudge.cpp`: the whole thing
- `kmod_info.c`: the kmod info block xcode would normally generate
- `Info.plist`: matches on `IOResources` so it starts early without needing any hardware match
- `release/`: the built kext I actually use
