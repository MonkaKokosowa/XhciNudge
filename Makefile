# builds XhciNudge.kext on linux (no mac or xcode needed)
#
# you need: clang (i used 21), ld64 from cctools-port, and acidanthera's MacKernelSDK.
#   make SDK=/path/to/MacKernelSDK LD64=/path/to/x86_64-apple-darwin-ld
# output lands in build/XhciNudge.kext (change with OUT=...)

OUT     ?= build
SDK     ?= MacKernelSDK
LD64    ?= x86_64-apple-darwin-ld
RES     := $(shell clang -print-resource-dir)/include

FLAGS   = -target x86_64-apple-macos10.13 -nostdinc -isystem $(SDK)/Headers -isystem $(RES) \
          -mkernel -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -Wall -Wextra \
          -Wno-unused-parameter -fno-stack-protector -fno-common
# the atexit flags stop new clang from importing ___cxa_atexit, which the kernel doesn't export
CXXFLAGS = $(FLAGS) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-use-cxa-atexit -mllvm -disable-atexit-based-global-dtor-lowering -Wno-inconsistent-missing-override
CFLAGS   = $(FLAGS) -std=gnu11

KEXT = $(OUT)/XhciNudge.kext

all: $(KEXT)/Contents/MacOS/XhciNudge $(KEXT)/Contents/Info.plist

$(OUT)/XhciNudge.o: XhciNudge.cpp
	mkdir -p $(OUT)
	clang++ $(CXXFLAGS) -c $< -o $@

$(OUT)/kmod_info.o: kmod_info.c
	mkdir -p $(OUT)
	clang $(CFLAGS) -c $< -o $@

$(KEXT)/Contents/MacOS/XhciNudge: $(OUT)/XhciNudge.o $(OUT)/kmod_info.o
	mkdir -p $(KEXT)/Contents/MacOS
	$(LD64) -kext -arch x86_64 -platform_version macos 10.13 15.0 -o $@ $^ -L$(SDK)/Library/x86_64 -lkmod

$(KEXT)/Contents/Info.plist: Info.plist
	mkdir -p $(KEXT)/Contents
	cp $< $@

clean:
	rm -rf $(OUT)/XhciNudge.kext $(OUT)/XhciNudge.o $(OUT)/kmod_info.o
