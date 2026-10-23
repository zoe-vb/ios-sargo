/*
 * sweetgpu-test: prove the SweetGPU submit/fence path (sargo, Adreno 615,
 * kernel patch 0093; boot-arg sargogpu=1) from user space. It opens the
 * SweetGPU user client, allocates two buffers, builds a one-packet command
 * buffer that makes the GPU write a sentinel into the other buffer, submits
 * it, waits on the fence and checks the sentinel landed. Then a negative
 * case: an IB pointing outside the client's buffers must be refused.
 *
 * The device has no interactive console, so output is written to both stdout
 * and /dev/null; boot with sweetnull=sweetgpu-test and the kernel tees this
 * process's stdout into the kernel log as |sweetgpu-test| ...
 *
 *   tools/sweetgpu-test/build.sh -> tools/sweetgpu-test/out/sweetgpu-test
 *
 * Selectors (iokit/Kernel/arm/SweetGPU.cpp):
 *   10 bo new  in[1]=size          out[3]=handle, gpu_iova, size
 *   11 bo free in[1]=handle
 *   13 submit  struct{u32 nr_ibs; u32 flags; ib[]}  out[1]=seqno
 *   14 wait    in[2]=seqno, timeout_ms
 *   15 info    out[4]=chip id, GMEM size, GMEM base, max freq
 * A buffer's CPU pointer comes from IOConnectMapMemory64(handle).
 */
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SENTINEL	0x5eed600du

/* submit struct, must match SweetGPU.cpp / bsd/dev/sweet_gpu.h */
struct sweet_gpu_ib {
	uint64_t iova;
	uint32_t dwords;
	uint32_t pad;
};
struct submit {
	uint32_t nr_ibs;
	uint32_t flags;
	struct sweet_gpu_ib ib[1];
};

static int g_null = -1;

static void
say(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	fflush(stdout);
	if (g_null >= 0 && n > 0) {
		write(g_null, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
	}
}

/* PM4, as shim/gpu.c */
static uint32_t
pm4_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
	    (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t
pkt7(unsigned op, unsigned cnt)
{
	return 0x70000000u | cnt | pm4_parity(cnt) << 15 | (op & 0x7fu) << 16 | pm4_parity(op) << 23;
}

/* bo new + map; returns the GPU VA and (through *cpu) the CPU pointer. */
static uint64_t
bo_new(io_connect_t c, uint64_t size, uint32_t *handle, void **cpu)
{
	uint64_t in = size, out[3] = {0};
	uint32_t n = 3;
	if (IOConnectCallScalarMethod(c, 10, &in, 1, out, &n) != KERN_SUCCESS || n < 3) {
		return 0;
	}
	*handle = (uint32_t)out[0];
	mach_vm_address_t addr = 0;
	mach_vm_size_t msize = 0;
	if (IOConnectMapMemory64(c, *handle, mach_task_self(), &addr, &msize, kIOMapAnywhere) != KERN_SUCCESS) {
		return 0;
	}
	*cpu = (void *)addr;
	return out[1];		/* GPU iova */
}

int
main(void)
{
	g_null = open("/dev/null", O_WRONLY);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	io_connect_t c = 0;
	kern_return_t kr = svc ? IOServiceOpen(svc, mach_task_self(), 0, &c) : KERN_FAILURE;
	if (kr != KERN_SUCCESS) {
		say("no SweetGPU (service %u, open 0x%x)\n", svc, kr);
		return 1;
	}

	/* the kernel brings the GPU up on its own thread: wait (up to 2 min). */
	uint64_t info[4] = {0};
	uint32_t ni = 4;
	for (int i = 0; i < 120; i++) {
		ni = 4;
		kr = IOConnectCallScalarMethod(c, 15, NULL, 0, info, &ni);
		if (kr != kIOReturnNotReady) {
			break;
		}
		sleep(1);
	}
	if (kr != KERN_SUCCESS) {
		say("GPU not ready: 0x%x\n", kr);
		return 1;
	}
	say("SweetGPU: chip %#llx, GMEM %#llx @ %#llx, %llu MHz\n",
	    info[0], info[1], info[2], info[3] / 1000000);

	/* target buffer (GPU writes here) and command buffer. */
	uint32_t th = 0, ch = 0;
	void *tp = NULL, *cp = NULL;
	uint64_t tgt = bo_new(c, 16384, &th, &tp);
	uint64_t cmd = bo_new(c, 16384, &ch, &cp);
	if (tgt == 0 || cmd == 0) {
		say("bo_new/map failed (target %#llx, cmd %#llx)\n", tgt, cmd);
		return 1;
	}
	volatile uint32_t *t = (volatile uint32_t *)tp;
	t[0] = 0xdeadbeefu;

	/* command buffer: CP_MEM_WRITE SENTINEL to the target's GPU VA. */
	uint32_t *ib = (uint32_t *)cp;
	unsigned k = 0;
	ib[k++] = pkt7(0x3d, 3);		/* CP_MEM_WRITE */
	ib[k++] = (uint32_t)tgt;
	ib[k++] = (uint32_t)(tgt >> 32);
	ib[k++] = SENTINEL;

	struct submit s = { .nr_ibs = 1, .ib[0] = { .iova = cmd, .dwords = k } };
	uint64_t seqno = 0;
	uint32_t ns = 1;
	kr = IOConnectCallMethod(c, 13, NULL, 0, &s, sizeof(s), &seqno, &ns, NULL, NULL);
	if (kr != KERN_SUCCESS) {
		say("submit failed: 0x%x\n", kr);
		return 1;
	}
	uint64_t w[2] = { seqno, 2000 };
	kern_return_t wr = IOConnectCallScalarMethod(c, 14, w, 2, NULL, NULL);
	uint32_t got = t[0];

	/* negative: an IB outside any buffer must be refused. */
	struct submit bad = { .nr_ibs = 1, .ib[0] = { .iova = 0xc0000000ull, .dwords = 4 } };
	uint64_t bseq = 0;
	ns = 1;
	kern_return_t br = IOConnectCallMethod(c, 13, NULL, 0, &bad, sizeof(bad), &bseq, &ns, NULL, NULL);

	int ok = kr == KERN_SUCCESS && wr == KERN_SUCCESS && got == SENTINEL && br != KERN_SUCCESS;
	say("submit seq %llu, wait 0x%x, target[0] = %#x (want %#x); foreign IB 0x%x -> %s\n",
	    seqno, wr, got, SENTINEL, br, ok ? "SUBMIT/FENCE OK" : "SUBMIT/FENCE WRONG");

	IOConnectCallScalarMethod(c, 11, (uint64_t[]){ th }, 1, NULL, NULL);
	IOConnectCallScalarMethod(c, 11, (uint64_t[]){ ch }, 1, NULL, NULL);
	return ok ? 0 : 1;
}
