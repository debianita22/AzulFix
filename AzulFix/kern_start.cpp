//
//  kern_start.cpp
//  AzulFix
//
//  Lilu plugin for AppleIntelFramebufferAzul (Haswell) on firmware that drives the built-in
//  panel through a DisplayPort port (fPortType 2, not eDP) and hides the ME (HECI) device,
//  as ouroBIOS/coreboot does on the ThinkPad W541. Both fixes are applied in memory when the
//  framebuffer loads, so the kext on disk (restored by OCLP) stays untouched.
//
//  ASR  AppleIntelAzulController::getFeatureControl copies FeatureControl > ASRDisable from the
//       controller properties (kext personality) to controller+0x1a5e. With 0, LinkTraining sets
//       Alternate Scrambler Reset in DP_TP_CTL when the panel reports it in DPCD 0x00D, but enables
//       it on the sink (DPCD 0x10A) only for eDP ports: source and sink scramble differently and
//       the panel shows white flashes instead of a picture. ASRDisable = 1 is put in FeatureControl
//       right before the original reads it.
//
//  ME   AppleIntelAzulController::findMEDevice waits 10 s in waitForMatchingService for each of
//       pci8086,8c3a/9c3a/8cba/9cba: 40 s at boot and again for every framebuffer at wake (green
//       screen until the gamma is restored). The wait becomes 1 ms.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

static const char *bootargOff[] {
	"-azulfixoff"
};

static const char *bootargDebug[] {
	"-azulfixdbg"
};

static const char *bootargBeta[] {
	"-azulfixbeta"
};

static const char *pathAzul[] {
	"/System/Library/Extensions/AppleIntelFramebufferAzul.kext/Contents/MacOS/AppleIntelFramebufferAzul",
	"/Library/Extensions/AppleIntelFramebufferAzul.kext/Contents/MacOS/AppleIntelFramebufferAzul",
};

static KernelPatcher::KextInfo kextAzul {
	"com.apple.driver.AppleIntelFramebufferAzul", pathAzul, arrsize(pathAzul), {}, {}, KernelPatcher::KextInfo::Unloaded
};

// findMEDevice (18.0.8, 0x262b9): mov rbx, rax; mov rsi, 10000000000 (ns); mov rdi, rax
static const uint8_t meWaitFind[] {
	0x48, 0x89, 0xC3, 0x48, 0xBE, 0x00, 0xE4, 0x0B, 0x54, 0x02, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC7
};

// mov rsi, 1000000 (1 ms)
static const uint8_t meWaitRepl[] {
	0x48, 0x89, 0xC3, 0x48, 0xBE, 0x40, 0x42, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC7
};

static_assert(sizeof(meWaitFind) == sizeof(meWaitRepl), "find and replace differ in size");

static mach_vm_address_t orgGetFeatureControl {};
static bool asrInjected {false};

// Shown by `ioreg -rc AzulFix -d1`
static const char *asrStatus {"Azul not loaded"};
static const char *meStatus {"Azul not loaded"};

static void publishStatus() {
	auto self = ADDPR(selfInstance);
	if (self) {
		self->setProperty("ASRDisable", asrStatus);
		self->setProperty("MEWait", meStatus);
	}
}

static void wrapGetFeatureControl(IOService *controller) {
	auto features = OSDynamicCast(OSDictionary, controller->getProperty("FeatureControl"));
	auto current = features ? OSDynamicCast(OSNumber, features->getObject("ASRDisable")) : nullptr;
	if (!features) {
		// Without the dictionary Azul takes other defaults for every key: leave it alone
		asrStatus = "no FeatureControl in the controller, not changed";
	} else if (current && current->unsigned32BitValue() != 0) {
		if (!asrInjected)
			asrStatus = "1 already in the kext Info.plist";
	} else {
		auto patched = OSDictionary::withDictionary(features);
		auto one = OSNumber::withNumber(1, 32);
		if (patched && one && patched->setObject("ASRDisable", one) && controller->setProperty("FeatureControl", patched)) {
			asrInjected = true;
			asrStatus = "set to 1";
		} else {
			asrStatus = "FeatureControl update failed";
		}
		OSSafeReleaseNULL(one);
		OSSafeReleaseNULL(patched);
	}

	SYSLOG("azul", "FeatureControl ASRDisable %s", asrStatus);
	publishStatus();
	FunctionCast(wrapGetFeatureControl, orgGetFeatureControl)(controller);
}

static void processAzul(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-azulfixnoasr")) {
		asrStatus = "off (-azulfixnoasr)";
	} else {
		KernelPatcher::RouteRequest request("__ZN24AppleIntelAzulController17getFeatureControlEv", wrapGetFeatureControl, orgGetFeatureControl);
		patcher.clearError();
		if (patcher.routeMultiple(index, &request, 1, address, size)) {
			asrStatus = "waiting for getFeatureControl";
		} else {
			asrStatus = "getFeatureControl not routed";
			SYSLOG("azul", "failed to route getFeatureControl, error %d", static_cast<int>(patcher.getError()));
		}
		patcher.clearError();
	}

	if (checkKernelArgument("-azulfixnome")) {
		meStatus = "off (-azulfixnome)";
	} else {
		KernelPatcher::LookupPatch patch {&kextAzul, meWaitFind, meWaitRepl, sizeof(meWaitFind), 1};
		patcher.clearError();
		patcher.applyLookupPatch(&patch);
		switch (patcher.getError()) {
			case KernelPatcher::Error::NoError:
				meStatus = "10 s -> 1 ms";
				break;
			case KernelPatcher::Error::MemoryIssue:
				meStatus = "bytes not found (other Azul build, or already patched on disk)";
				break;
			default:
				meStatus = "patch failed";
				break;
		}
		patcher.clearError();
	}

	SYSLOG("azul", "getFeatureControl: %s, findMEDevice wait: %s", asrStatus, meStatus);
	publishStatus();
}

static void pluginStart() {
	lilu.onKextLoadForce(&kextAzul, 1,
	[](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (index == kextAzul.loadIndex)
			processAzul(patcher, index, address, size);
	});
}

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Mavericks,
	KernelVersion::Tahoe,
	pluginStart
};
