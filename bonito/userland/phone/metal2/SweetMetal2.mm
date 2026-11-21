/*
 * SweetMetal2.bundle: the Metal driver for the Adreno 640 on metal2vulkan + Turnip (boot-arg sweetmtl=2: the kernel's
 * SweetAccelerator names this bundle and class SweetMTL2Device). Metal.framework loads it in every process that asks
 * for a device; -initWithAcceleratorPort: hands back the frontend's device (NativeBridgeMetalFrontend.mm, UniXNU's,
 * over the bridge in bridge/, which drives Turnip, the libvulkan_freedreno.dylib inside this bundle, through the
 * kernel's v2 user client). Which processes get one: backboardd always, others when /var/local/mtl2-clients lists
 * them ("*" = all, "-NAME" excludes NAME); the rest get none, so their frameworks take their CPU paths.
 * Log: /dev/console and /var/local/mtl2.log (when it exists, mode 666), lines "MTL2[pid name]: ...".
 */
#include <mach/mach.h>
#include <mach/vm_map.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

extern "C" void sm2_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "MTL2[%d %s]: %s\n", getpid(), getprogname(), s);
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;
	static int console = -2, file = -2;
	if (console == -2)
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (file == -2)
		file = open("/var/local/mtl2.log", O_WRONLY | O_APPEND);
	if (console >= 0)
		write(console, buf, (size_t)n);
	if (file >= 0)
		write(file, buf, (size_t)n);
}

static void sm2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm2_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sm2_logs(buf);
}

/*
 * In-process sampling profiler (performance work of 2026-11-21), only when /var/local/mtl2-prof starts with 1: every 5 ms the running threads of the process are
 * suspended, their pc and frame-pointer chain (4 callers, read with vm_read_overwrite so a bad pointer cannot fault) are looked at, and a sample is kept when at
 * least one of the 5 addresses lies inside SweetMetal2 or libvulkan_freedreno (threads waiting in system libraries are not counted). Two tables are
 * kept: the leaf address (pc) and every address of the chain; every 30 s the 150 most frequent of each go to /var/local/mtl2.log as "HL count pc" and "HA count
 * addr" (cumulative; raw addresses: subtract the image bases of the "images:" line and look them up in the unstripped binaries), with "H samples kept/seen".
 */
static uint64_t sm2_prof_lo, sm2_prof_hi;
/*
 * The render server's busiest calling thread (set by the frontend when it binds it to a gold core): the profiler samples it every 5 ms whatever its state, running or blocked, because the render
 * thread commits a frame only every 106 ms though its own work is about 16 ms, and the running-only sampling cannot say what it waits for. "HR count state: frames" rows every 30 s, the frames named
 * with dladdr (exported symbols only: the dyld shared cache has no local ones).
 */
extern "C" { uint64_t sm2_render_tid = 0; }
struct sm2_rrec { int state; uint64_t pc[5]; unsigned n; };
static sm2_rrec sm2_rrecs[800];
static int sm2_nrr;
static unsigned long sm2_rstate[8];

static void sm2_rname(uint64_t a, char *out, size_t size)
{
	Dl_info di;
	if (a && dladdr((void *)(uintptr_t)a, &di) && di.dli_fname) {
		const char *base = strrchr(di.dli_fname, '/');
		snprintf(out, size, "%s`%s+%#lx", base ? base + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?", (unsigned long)(a - (uint64_t)(uintptr_t)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)));
	} else {
		snprintf(out, size, "%llx", (unsigned long long)a);
	}
}

static void sm2_rdump(void)
{
	sm2_log("HR render thread %#llx samples: running %lu waiting %lu uninterruptible %lu other %lu", (unsigned long long)sm2_render_tid, sm2_rstate[TH_STATE_RUNNING], sm2_rstate[TH_STATE_WAITING],
		sm2_rstate[TH_STATE_UNINTERRUPTIBLE], sm2_rstate[0]);
	for (int top = 0; top < 30; top++) {
		int best = -1;
		for (int k = 0; k < sm2_nrr; k++)
			if (sm2_rrecs[k].n && !(sm2_rrecs[k].n & 0x80000000u) && (best < 0 || sm2_rrecs[k].n > sm2_rrecs[best].n))
				best = k;
		if (best < 0)
			break;
		char n[5][96];
		for (int d = 0; d < 5; d++)
			sm2_rname(sm2_rrecs[best].pc[d], n[d], sizeof n[d]);
		sm2_log("HR %u state %d: %s < %s < %s < %s < %s", sm2_rrecs[best].n, sm2_rrecs[best].state, n[0], n[1], n[2], n[3], n[4]);
		sm2_rrecs[best].n |= 0x80000000u;
	}
	for (int k = 0; k < sm2_nrr; k++)
		sm2_rrecs[k].n &= 0x7fffffffu;
}

struct sm2_prof_ent { uint64_t addr; uint32_t n; };

static void sm2_prof_add(sm2_prof_ent *t, int *nt, int cap, uint64_t a)
{
	for (int k = 0; k < *nt; k++)
		if (t[k].addr == a) {
			t[k].n++;
			return;
		}
	if (*nt < cap) {
		t[*nt].addr = a;
		t[*nt].n = 1;
		(*nt)++;
	}
}

static void sm2_prof_dump(const char *tag, sm2_prof_ent *t, int nt)
{
	for (int top = 0; top < 150; top++) {
		int best = -1;
		for (int k = 0; k < nt; k++)
			if (t[k].n && !(t[k].n & 0x80000000u) && (best < 0 || t[k].n > t[best].n))
				best = k;
		if (best < 0)
			break;
		sm2_log("%s %u %llx", tag, t[best].n, (unsigned long long)t[best].addr);
		t[best].n |= 0x80000000u;
	}
	for (int k = 0; k < nt; k++)
		t[k].n &= 0x7fffffffu;
}

static void *sm2_profiler(void *arg)
{
	(void)arg;
	static sm2_prof_ent leaf[6000], all[12000];
	int nleaf = 0, nall = 0;
	unsigned long kept = 0, seen = 0, ticks = 0;
	const mach_port_t me = mach_thread_self();

	for (;;) {
		usleep(5000);
		ticks++;
		thread_act_array_t th = NULL;
		mach_msg_type_number_t cnt = 0;
		if (task_threads(mach_task_self(), &th, &cnt) == KERN_SUCCESS) {
			for (mach_msg_type_number_t i = 0; i < cnt; i++) {
				if (th[i] != me && sm2_render_tid) {
					thread_identifier_info_data_t ti;
					mach_msg_type_number_t tn = THREAD_IDENTIFIER_INFO_COUNT;
					if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ti, &tn) == KERN_SUCCESS && ti.thread_id == sm2_render_tid) {
						thread_basic_info_data_t b2;
						mach_msg_type_number_t bn2 = THREAD_BASIC_INFO_COUNT;
						const int state = thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&b2, &bn2) == KERN_SUCCESS ? b2.run_state : 0;
						if (thread_suspend(th[i]) == KERN_SUCCESS) {
							arm_thread_state64_t st;
							mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
							sm2_rrec r = { .state = state };
							const bool ok = thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS;
							if (ok) {
								r.pc[0] = arm_thread_state64_get_pc(st);
								r.pc[1] = arm_thread_state64_get_lr(st);
								uint64_t fp = arm_thread_state64_get_fp(st);
								for (int d = 2; d < 5 && fp; d++) {
									uint64_t pair[2];
									vm_size_t got = 0;
									if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS || got != sizeof(pair))
										break;
									r.pc[d] = pair[1];
									fp = pair[0];
								}
							}
							thread_resume(th[i]);
							if (ok) {
								sm2_rstate[(state >= 1 && state <= 5) ? state : 0]++;
								int at = -1;
								for (int k = 0; k < sm2_nrr; k++)
									if (sm2_rrecs[k].state == r.state && !memcmp(sm2_rrecs[k].pc, r.pc, sizeof r.pc)) {
										at = k;
										break;
									}
								if (at < 0 && sm2_nrr < 800) {
									at = sm2_nrr++;
									sm2_rrecs[at] = r;
									sm2_rrecs[at].n = 0;
								}
								if (at >= 0)
									sm2_rrecs[at].n++;
							}
						}
					}
				}
				if (th[i] != me) {
					thread_basic_info_data_t bi;
					mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT;
					if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bn) == KERN_SUCCESS && bi.run_state == TH_STATE_RUNNING &&
					    !(bi.flags & TH_FLAGS_IDLE) && thread_suspend(th[i]) == KERN_SUCCESS) {
						arm_thread_state64_t st;
						mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
						uint64_t pc[5] = { 0 };
						bool ok = thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS;
						if (ok) {
							pc[0] = arm_thread_state64_get_pc(st);
							pc[1] = arm_thread_state64_get_lr(st);
							uint64_t fp = arm_thread_state64_get_fp(st);
							for (int d = 2; d < 5 && fp; d++) {
								uint64_t pair[2];
								vm_size_t got = 0;
								if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS ||
								    got != sizeof(pair))
									break;
								pc[d] = pair[1];
								fp = pair[0];
							}
						}
						thread_resume(th[i]);
						if (ok) {
							seen++;
							bool ours = false;
							for (int d = 0; d < 5; d++)
								if (pc[d] >= sm2_prof_lo && pc[d] < sm2_prof_hi)
									ours = true;
							if (ours) {
								kept++;
								sm2_prof_add(leaf, &nleaf, 6000, pc[0]);
								for (int d = 0; d < 5; d++)
									if (pc[d])
										sm2_prof_add(all, &nall, 12000, pc[d]);
							}
						}
					}
				}
				mach_port_deallocate(mach_task_self(), th[i]);
			}
			vm_deallocate(mach_task_self(), (vm_address_t)th, cnt * sizeof(th[0]));
		}
		if (ticks % 6000 == 0) {        /* every 30 s */
			sm2_log("H samples kept %lu seen %lu leaves %d addresses %d", kept, seen, nleaf, nall);
			sm2_prof_dump("HL", leaf, nleaf);
			sm2_prof_dump("HA", all, nall);
			if (sm2_render_tid)
				sm2_rdump();
		}
	}
	return NULL;
}

/*
 * The A640 does not snoop the CPU caches: libSystem's sys_dcache_flush (a no-op where the GPU is coherent) must
 * really clean and invalidate here. User space may (SCTLR_EL1.UCI). This definition wins at link time, for the
 * bridge and the frontend alike (IOSurface imports).
 */
extern "C" void sys_dcache_flush(void *start, size_t len)
{
	/*
	 * Only what is mapped and readable is cleaned: a range that runs past the end of its mapping (the bridge and the frontend round lengths up to rows,
	 * pages and padded pitches, and the caller's memory may stop short of that) used to fault in `dc civac` at the first byte after the mapping and kill
	 * backboardd (SIGSEGV, five crashes in six boots). One vm_region_64 per mapping the range touches (about 3 us).
	 */
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	while (a < end) {
		vm_address_t addr = (vm_address_t)a;
		vm_size_t size = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
			break;                                  /* nothing mapped above a */
		if (addr > a) {                                 /* a is in a hole: continue at the next mapping */
			a = addr;
			continue;
		}
		uintptr_t rend = (uintptr_t)addr + size;
		if (rend > end)
			rend = end;
		if (info.protection & VM_PROT_READ)
			for (; a < rend; a += 64)
				__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
		a = rend;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Metal validation failures abort by default; frameworks with Apple-GPU-only kernels (MPS) report one on any other device. */
__attribute__((constructor)) static void sm2_error_mode(void)
{
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);	/* 5 = NSLog */
}

/* Selectors the frontend lacks, each logged once (the frontend then raises the usual unrecognised-selector exception) */
static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm2_log("MISSING %s -%s", cls, sel);
}

@interface NBDeviceMethods : NSObject
@end

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines itself. */
@interface NBDeviceMethods (SweetMetal2)
@end
@implementation NBDeviceMethods (SweetMetal2)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	if (error)
		*error = [NSError errorWithDomain:@"SweetMetal2" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
@end

static bool listed(void)
{
	if (!strcmp(getprogname(), "backboardd"))
		return true;
	FILE *f = fopen("/var/local/mtl2-clients", "r");
	if (!f)
		return false;
	char text[2048] = { 0 };
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	/* names, "*" = all, "-NAME" = never NAME (wins over "*") */
	char *save = NULL;
	bool yes = false;
	for (char *name = strtok_r(text, " ,\t\r\n", &save); name; name = strtok_r(NULL, " ,\t\r\n", &save)) {
		if (name[0] == '-' && !strcmp(name + 1, getprogname()))
			return false;
		if (!strcmp(name, "*") || !strcmp(name, getprogname()))
			yes = true;
	}
	return yes;
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTL2Device : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTL2Device
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	/* this object is dropped for the frontend's device: initialised, so its -[_MTLDevice dealloc] has queues to release */
	if (!(self = [super init]))
		return nil;
	static id device;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		if (!listed()) {
			sm2_log("not listed (/var/local/mtl2-clients): no Metal device");
			return;
		}
		NSString *bundle = [[NSBundle bundleForClass:[SweetMTL2Device class]] bundlePath];
		NSString *turnip = [bundle stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
		char cache[256];
		mkdir("/var/local/m2v-cache", 0777);
		chmod("/var/local/m2v-cache", 0777);
		snprintf(cache, sizeof(cache), "/var/local/m2v-cache/%s", getprogname());
		mkdir(cache, 0777);
		if (access(cache, W_OK) != 0) {
			/* a sandboxed process: its own caches directory */
			char dir[256];
			if (confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir) - 8)) {
				snprintf(cache, sizeof(cache), "%sm2v", dir);
				mkdir(cache, 0700);
			}
		}
		/* plain rows everywhere: no Apple framebuffer compression, no 10-bit deep framebuffer */
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		/* the runtime's VkPipeline makings count into the telemetry (V row) */
		setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
		/* debugging: KEY=VALUE lines of /var/local/mtl2-env into this process's environment */
		if (FILE *ef = fopen("/var/local/mtl2-env", "r")) {
			char line[256];
			while (fgets(line, sizeof(line), ef)) {
				line[strcspn(line, "\r\n")] = 0;
				char *eq = strchr(line, '=');
				if (line[0] != '#' && eq) {
					*eq = 0;
					setenv(line, eq + 1, 1);
					sm2_log("env %s=%s", line, eq + 1);
				}
			}
			fclose(ef);
		}
		/* debugging: Turnip options for this process from /var/local/mtl2-tu-debug (TU_DEBUG), its log to /var/local/turnip-<proc>.log */
		{
			char opts[128] = { 0 };
			FILE *f = fopen("/var/local/mtl2-tu-debug", "r");
			if (f) {
				if (fgets(opts, sizeof(opts), f))
					opts[strcspn(opts, "\r\n")] = 0;
				fclose(f);
			}
			/* 2026-11-21: Turnip renders without its tile memory (TU_DEBUG=sysmem) unless that file says otherwise: with the tile memory every pass loaded and stored whole screens
			 * for a handful of draws; 13.8 swaps/s against 10.7 and 10.8 panel copies/s against 8.0 in gesture series. The file's content "default" leaves TU_DEBUG alone. */
			if (!opts[0])
				strlcpy(opts, "sysmem", sizeof(opts));
			else if (!strcmp(opts, "default"))
				opts[0] = 0;
			if (opts[0]) {
				char path[128];
				snprintf(path, sizeof(path), "/var/local/turnip-%s.log", getprogname());
				setenv("TU_DEBUG", opts, 1);
				setenv("MESA_LOG_FILE", path, 1);
				sm2_log("TU_DEBUG=%s, log %s", opts, path);
			}
		}
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		double t0 = CFAbsoluteTimeGetCurrent();
		@try {
			device = native_bridge_metal_open(turnip.UTF8String, cache, registryID, missing);
		} @catch (NSException *e) {
			sm2_log("device: exception %s", e.reason.UTF8String);
			device = nil;
		}
		sm2_log("device %s in %.0f ms (cache %s)%s%s", device ? [[device name] UTF8String] : "refused",
			(CFAbsoluteTimeGetCurrent() - t0) * 1000, cache, device ? "" : ": ", device ? "" : nmb_last_error());
		/*
		 * Extended-range (XR10) formats: the frontend withholds them so backboardd keeps an 8-bit framebuffer; other
		 * processes may render into them (AegirPoster's CAMetalLayer throws "unsupported extended range format" without).
		 */
		if (device && strcmp(getprogname(), "backboardd") != 0)
			class_replaceMethod(object_getClass(device), sel_registerName("supportsExtendedXR10Formats"),
					    imp_implementationWithBlock(^BOOL(id me) { (void)me; return YES; }), "B16@0:8");
		/* image bases, for symbolicating pstack samples (atos -o out/SweetMetal2.dSYM -l BASE ADDR) */
		Dl_info self = { 0 }, tu = { 0 };
		dladdr((void *)sm2_logs, &self);
		void *th = dlopen(turnip.UTF8String, RTLD_NOW | RTLD_NOLOAD);
		void *vk = th ? dlsym(th, "vk_icdGetInstanceProcAddr") : NULL;
		if (vk)
			dladdr(vk, &tu);
		sm2_log("images: SweetMetal2 %p, libvulkan_freedreno %p", self.dli_fbase, tu.dli_fbase);
		bool prof_on = false;
		if (FILE *pf = fopen("/var/local/mtl2-prof", "r")) {        /* the profiler runs only when the file starts with 1 (any other content: off) */
			prof_on = fgetc(pf) == '1';
			fclose(pf);
		}
		if (device && !strcmp(getprogname(), "backboardd") && prof_on) {
			pthread_t pt;
			pthread_attr_t pa;
			sm2_prof_lo = (uint64_t)(uintptr_t)(self.dli_fbase < tu.dli_fbase ? self.dli_fbase : (tu.dli_fbase ? tu.dli_fbase : self.dli_fbase));
			sm2_prof_hi = (uint64_t)(uintptr_t)(self.dli_fbase > tu.dli_fbase ? self.dli_fbase : tu.dli_fbase) + 0x1600000;
			pthread_attr_init(&pa);
			pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
			pthread_create(&pt, &pa, sm2_profiler, NULL);
			sm2_log("profiler started");
		}
		/* backboardd: the frontend's counters every 10 s, into /var/local/mtl2.log only (with /var/local/mtl2-telemetry) */
		if (device && !strcmp(getprogname(), "backboardd"))
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				for (;;) {
					sleep(10);
					if (access("/var/local/mtl2-telemetry", F_OK) != 0)
						continue;
					static char text[8192];
					native_bridge_metal_telemetry(text, sizeof(text));
					int fd = open("/var/local/mtl2.log", O_WRONLY | O_APPEND);
					if (fd >= 0) {
						dprintf(fd, "MTL2[%d %s]: telemetry\n%s", getpid(), getprogname(), text);
						close(fd);
					}
				}
			});
	});
	return device;
}
@end
