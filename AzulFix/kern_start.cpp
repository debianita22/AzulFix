//
//  kern_start.cpp
//  AzulFix
//
//  Lilu plugin for AppleIntelFramebufferAzul (Haswell) on firmware that drives the built-in
//  panel through a DisplayPort port (fPortType 2, not eDP) and hides the ME (HECI) device,
//  as ouroBIOS/coreboot does on the ThinkPad W541. The fixes are applied in memory when the
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
//  Panel  To Azul the panel is a DP display, so it never drives the PCH panel power sequencer
//       (PP_CONTROL), which coreboot uses for it (GFX_GMA_PANEL_1_PORT "DP3"): with the firmware
//       leaving the display to the OS at S3 resume (VDD only, PP_CONTROL 0x8) the link trains and
//       the picture is sent, but the backlight stays off. On the panel framebuffer (port-number 7,
//       DDI D), the panel power sequence is turned on when the framebuffer wakes
//       (doSetPowerState), before Azul wakes the sink over AUX and trains the link: powering the
//       panel up later resets it (striped or black picture at wake). The backlight goes on after
//       LightUpDP, as libgfxinit and i915 do; before DisableDisplay the backlight is turned off,
//       with the backlight off delay of the panel before the video goes. The panel keeps its
//       power, so that HPD stays high for the next probe. Nothing is done if the sequencer is
//       already in the requested state.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOLib.h>
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

// Panel: port-number of DDI D, and the PCH panel power sequencer (i915_reg.h)
static constexpr uint32_t PanelPort {7};
static constexpr unsigned long PCH_PP_STATUS {0xC7200};
static constexpr unsigned long PCH_PP_CONTROL {0xC7204};
static constexpr unsigned long PCH_PP_OFF_DELAYS {0xC720C};
static constexpr uint32_t PP_ON {1U << 31};
static constexpr uint32_t PP_SEQUENCE_MASK {3U << 28};
static constexpr uint32_t PANEL_POWER_ON {1U << 0};
static constexpr uint32_t PANEL_POWER_RESET {1U << 1};
static constexpr uint32_t EDP_BLC_ENABLE {1U << 2};

// AppleIntelAzulController::ReadRegister32(unsigned long) and WriteRegister32(unsigned long, unsigned int)
using t_readRegister32 = uint32_t (*)(void *, unsigned long);
using t_writeRegister32 = void (*)(void *, unsigned long, uint32_t);
static t_readRegister32 orgReadRegister32 {};
static t_writeRegister32 orgWriteRegister32 {};
static mach_vm_address_t orgLightUpDP {};
static mach_vm_address_t orgDisableDisplay {};
static mach_vm_address_t orgDoSetPowerState {};
// The controller, for the framebuffer hook: Azul has one, and calls getFeatureControl in its start
static void *panelController {};

// Shown by `ioreg -rc AzulFix -d1`
static const char *asrStatus {"Azul not loaded"};
static const char *meStatus {"Azul not loaded"};
static const char *panelStatus {"Azul not loaded"};

static void publishStatus() {
	auto self = ADDPR(selfInstance);
	if (self) {
		self->setProperty("ASRDisable", asrStatus);
		self->setProperty("MEWait", meStatus);
		self->setProperty("Panel", panelStatus);
	}
}

static void wrapGetFeatureControl(IOService *controller) {
	panelController = controller;
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

static bool isPanel(IOService *framebuffer) {
	auto port = framebuffer ? OSDynamicCast(OSNumber, framebuffer->getProperty("port-number")) : nullptr;
	return port && port->unsigned32BitValue() == PanelPort;
}

// Panel.On (libgfxinit): before Azul talks to the panel at all, as the panel loses its state when
// the sequencer powers it up (the link at 1.1.1, the sink power state written over AUX at
// 1.1.2: striped or black picture at wake); wait for the sequencer
static void panelPowerOn(void *controller) {
	auto control = orgReadRegister32(controller, PCH_PP_CONTROL);
	if (control == 0xFFFFFFFF || (control & PANEL_POWER_ON))
		return;
	orgWriteRegister32(controller, PCH_PP_CONTROL, control | PANEL_POWER_ON | PANEL_POWER_RESET);
	// Power-up delay of the panel, 200 ms on the W541; give up after 500 ms
	for (int i = 0; i < 50; i++) {
		if ((orgReadRegister32(controller, PCH_PP_STATUS) & (PP_ON | PP_SEQUENCE_MASK)) == PP_ON)
			break;
		IOSleep(10);
	}
	DBGLOG("azul", "panel power on, PP_STATUS %08X", orgReadRegister32(controller, PCH_PP_STATUS));
}

// Backlight_On (libgfxinit): once the link carries the picture
static void backlightOn(void *controller) {
	auto control = orgReadRegister32(controller, PCH_PP_CONTROL);
	if (control != 0xFFFFFFFF && !(control & EDP_BLC_ENABLE)) {
		orgWriteRegister32(controller, PCH_PP_CONTROL, control | EDP_BLC_ENABLE);
		DBGLOG("azul", "backlight on, PP_CONTROL %08X", control | EDP_BLC_ENABLE);
	}
}

// Backlight_Off, then the backlight off to video off delay (PP_OFF_DELAYS 12:0, 100 us units)
static void backlightOff(void *controller) {
	auto control = orgReadRegister32(controller, PCH_PP_CONTROL);
	if (control != 0xFFFFFFFF && (control & EDP_BLC_ENABLE)) {
		orgWriteRegister32(controller, PCH_PP_CONTROL, control & ~EDP_BLC_ENABLE);
		auto delay = (orgReadRegister32(controller, PCH_PP_OFF_DELAYS) & 0x1FFF) * 100;
		if (delay)
			IOSleep((delay + 999) / 1000);
		DBGLOG("azul", "backlight off, PP_CONTROL %08X, waited %u us", control & ~EDP_BLC_ENABLE, delay);
	}
}

// Return values are passed through whole: only the panel framebuffer is touched
static uint64_t wrapLightUpDP(void *controller, IOService *framebuffer, void *path, const void *timing) {
	bool panel = isPanel(framebuffer);
	if (panel)
		panelPowerOn(controller);
	auto ret = FunctionCast(wrapLightUpDP, orgLightUpDP)(controller, framebuffer, path, timing);
	if (panel)
		backlightOn(controller);
	return ret;
}

static uint64_t wrapDisableDisplay(void *controller, IOService *framebuffer, void *path, bool arg) {
	if (isPanel(framebuffer))
		backlightOff(controller);
	return FunctionCast(wrapDisableDisplay, orgDisableDisplay)(controller, framebuffer, path, arg);
}

// AppleIntelFramebuffer::doSetPowerState(unsigned int): state 2 is on. At wake the original wakes
// the sink over AUX and then lights the display up: power the panel up before all of it. Not in a
// dark wake, where the framebuffers stay off.
static uint64_t wrapDoSetPowerState(IOService *framebuffer, uint32_t state) {
	if (state == 2 && panelController && isPanel(framebuffer))
		panelPowerOn(panelController);
	return FunctionCast(wrapDoSetPowerState, orgDoSetPowerState)(framebuffer, state);
}

static void processPanel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-azulfixnopanel")) {
		panelStatus = "off (-azulfixnopanel)";
		return;
	}

	patcher.clearError();
	orgReadRegister32 = reinterpret_cast<t_readRegister32>(patcher.solveSymbol(index, "__ZN24AppleIntelAzulController14ReadRegister32Em", address, size));
	orgWriteRegister32 = reinterpret_cast<t_writeRegister32>(patcher.solveSymbol(index, "__ZN24AppleIntelAzulController15WriteRegister32Emj", address, size));
	patcher.clearError();
	if (!orgReadRegister32 || !orgWriteRegister32) {
		panelStatus = "register accessors not found";
		SYSLOG("azul", "ReadRegister32/WriteRegister32 not found, panel backlight not handled");
		return;
	}

	KernelPatcher::RouteRequest requests[] {
		{"__ZN24AppleIntelAzulController9LightUpDPEP21AppleIntelFramebufferP11DISPLAYPATHPK29IODetailedTimingInformationV2", wrapLightUpDP, orgLightUpDP},
		{"__ZN24AppleIntelAzulController14DisableDisplayEP21AppleIntelFramebufferP11DISPLAYPATHb", wrapDisableDisplay, orgDisableDisplay},
		{"__ZN21AppleIntelFramebuffer15doSetPowerStateEj", wrapDoSetPowerState, orgDoSetPowerState},
	};
	if (patcher.routeMultiple(index, requests, arrsize(requests), address, size)) {
		panelStatus = "power with doSetPowerState, backlight with LightUpDP and DisableDisplay";
	} else {
		panelStatus = "LightUpDP/DisableDisplay/doSetPowerState not routed";
		SYSLOG("azul", "failed to route LightUpDP/DisableDisplay/doSetPowerState, error %d", static_cast<int>(patcher.getError()));
	}
	patcher.clearError();
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

	processPanel(patcher, index, address, size);

	SYSLOG("azul", "getFeatureControl: %s, findMEDevice wait: %s, panel: %s", asrStatus, meStatus, panelStatus);
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
