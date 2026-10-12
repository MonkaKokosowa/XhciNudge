/* the bit xcode normally generates for a kext: kmod_info plus the _realmain/_antimain hooks that
 * libkmod's _start/_stop look for. c++ kexts leave them NULL, the kernel runs the constructors and
 * iokit matching by itself. */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(local.XhciNudge, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = 1;
