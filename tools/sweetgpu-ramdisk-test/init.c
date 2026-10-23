/*
 * sweetgpu-ramdisk-test: prove the SweetGPU IOKit user client (patches
 * 0092+0093+0100) from user space, as a static PID 1 off a ramdisk - no iOS
 * rootfs, no dyld, no libSystem. It talks to IOKit with raw Mach traps and
 * hand-built MIG messages (layouts taken from the kernel's own device.defs /
 * mach_host.defs). It opens SweetGPU, allocates a target and a command
 * buffer, maps them, builds one CP_MEM_WRITE that makes the GPU write a
 * sentinel into the target, submits it, waits on the fence and checks it.
 *
 *   tools/sweetgpu-ramdisk-test/build.sh -> out/sweetgpu-ramdisk-test (via mkinit.py)
 *   boot: mkpayload --ramdisk <that> --cmdline "... rd=md0 sargogpu=1"
 *
 * No globals (mkinit lays out one read-only image): all state on the stack.
 */

/* ---- raw XNU / Mach traps (svc #0x80, trap number in x16) ---- */
static long sc3(long n, long a, long b, long c) {
	register long x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c, x16 asm("x16")=n, r asm("x0");
	asm volatile("svc #0x80" : "=r"(r) : "r"(x0),"r"(x1),"r"(x2),"r"(x16) : "memory","cc");
	return r;
}
static long trap0(long n) {
	register long x16 asm("x16")=n, r asm("x0");
	asm volatile("svc #0x80" : "=r"(r) : "r"(x16) : "memory","cc");
	return r;
}
/*
 * mach_msg2_trap (-47): platform binaries must use it (legacy mach_msg_trap
 * raises a guard -> "Initproc failed to start"). The header fields are passed
 * in packed (lo|hi<<32) register args; `data` is still the full message.
 */
static long machmsg2(void *data, unsigned long long opt, unsigned long long mb_ss,
    unsigned long long mr_lp, unsigned long long mv_id, unsigned long long dc_rn,
    unsigned long long rs_pr, unsigned long long timeout) {
	register long x0 asm("x0")=(long)data; register unsigned long long x1 asm("x1")=opt,
	    x2 asm("x2")=mb_ss, x3 asm("x3")=mr_lp, x4 asm("x4")=mv_id, x5 asm("x5")=dc_rn,
	    x6 asm("x6")=rs_pr, x7 asm("x7")=timeout; register long x16 asm("x16")=-47, r asm("x0");
	asm volatile("svc #0x80" : "=r"(r)
	    : "r"(x0),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x6),"r"(x7),"r"(x16) : "memory","cc");
	return r;
}
#define MACH64_SEND_MQ_CALL 0x0000000400000000ull

/* ---- console output ---- */
typedef unsigned char u8; typedef unsigned int u32; typedef unsigned long long u64;
static long slen(const char *s){ long n=0; while(s[n]) n++; return n; }
static void w(int fd, const char *s){ sc3(4, fd, (long)s, slen(s)); }
static void whex(int fd, const char *label, u64 v){
	char b[24]; int i=0; const char *hx="0123456789abcdef";
	w(fd,label); b[i++]='0'; b[i++]='x';
	int started=0;
	for(int sh=60; sh>=0; sh-=4){ int d=(v>>sh)&0xf; if(d||started||sh==0){ b[i++]=hx[d]; started=1; } }
	b[i++]='\n'; sc3(4, fd, (long)b, i);
}

/* ---- byte buffer helpers (messages are pack(4), built by hand) ---- */
static void p32(u8*b,int o,u32 v){ b[o]=v;b[o+1]=v>>8;b[o+2]=v>>16;b[o+3]=v>>24; }
static void p64(u8*b,int o,u64 v){ p32(b,o,(u32)v); p32(b,o+4,(u32)(v>>32)); }
static u32 g32(u8*b,int o){ return (u32)b[o]|(u32)b[o+1]<<8|(u32)b[o+2]<<16|(u32)b[o+3]<<24; }
static u64 g64(u8*b,int o){ return (u64)g32(b,o)|((u64)g32(b,o+4)<<32); }

/* Set the MIG header and do a send+receive on the shared reply port. */
static long mig(u8*buf, u32 remote, u32 reply, u32 msgid, u32 send_size, int complex) {
	u32 bits = 19 | (21u<<8);			/* COPY_SEND remote, MAKE_SEND_ONCE local */
	if (complex) bits |= 0x80000000u;
	p32(buf,0,bits); p32(buf,4,send_size); p32(buf,8,remote);
	p32(buf,12,reply); p32(buf,16,0); p32(buf,20,msgid);
	u64 desc = complex ? g32(buf,24) : 0;			/* body->msgh_descriptor_count */
	u64 opt = 1ull | 2ull | MACH64_SEND_MQ_CALL;		/* SEND|RCV|MQ_CALL */
	return machmsg2(buf, opt,
	    (u64)bits | ((u64)send_size<<32),
	    (u64)remote | ((u64)reply<<32),
	    0ull | ((u64)msgid<<32),
	    desc | ((u64)reply<<32),
	    1024ull | 0ull, 0ull);
}

/* PM4 (shim/gpu.c) */
static u32 pm4_parity(u32 v){ return (0x9669u >> (0xfu & (v^(v>>4)^(v>>8)^(v>>12)^(v>>16)^(v>>20)^(v>>24)^(v>>28)))) & 1u; }
static u32 pkt7(u32 op,u32 cnt){ return 0x70000000u|cnt|pm4_parity(cnt)<<15|(op&0x7fu)<<16|pm4_parity(op)<<23; }

#define SENTINEL 0x5eed600du
#define SEL_BONEW 10
#define SEL_SUBMIT 13
#define SEL_WAIT 14
#define SEL_INFO 15
#define kIOReturnNotReady 0xe00002d9u
#define kIOMapAnywhere 1u

/* io_connect_method: scalar in, scalar out (and optional 24-byte inband in). */
static long connect_method(u8*buf, u32 conn, u32 reply, u32 sel,
    const u64 *sin, u32 sic, const u8 *inband, u32 iic, u32 soc_expect, u64 *sout, u32 *soc_got) {
	int o=24;
	/* NDR */ p32(buf,o,0);p32(buf,o+4,0x00000001);o+=8;	/* int_rep = little */
	p32(buf,o,sel);o+=4;
	p32(buf,o,sic);o+=4;
	for(u32 i=0;i<sic;i++){ p64(buf,o,sin[i]); o+=8; }
	p32(buf,o,iic);o+=4;
	for(u32 i=0;i<iic;i++) buf[o+i]=inband[i];
	o+=iic; while(o&3)o++;
	p64(buf,o,0);o+=8;			/* ool_input */
	p64(buf,o,0);o+=8;			/* ool_input_size */
	p32(buf,o,0);o+=4;			/* inband_outputCnt */
	p32(buf,o,soc_expect);o+=4;		/* scalar_outputCnt */
	p64(buf,o,0);o+=8;			/* ool_output */
	p64(buf,o,0);o+=8;			/* ool_output_size */
	long kr = mig(buf, conn, reply, 2880, o, 0);
	if (kr) return kr;
	u32 ret = g32(buf,32);
	if (ret) return (long)(int)ret;
	u32 ioc = g32(buf,36);
	int ro = 40 + (int)ioc; while(ro&3)ro++;
	u32 got = g32(buf,ro); ro+=4;
	if (soc_got) *soc_got=got;
	for(u32 i=0;i<got && sout;i++){ sout[i]=g64(buf,ro); ro+=8; }
	return 0;
}

void start(void) {
	int fd = (int)sc3(5, (long)"/dev/console", 2, 0);		/* open RDWR */
	w(fd, "SWEET GPU RDTEST: start\n");
	u32 task = (u32)trap0(-28);					/* task_self_trap */
	u32 host = (u32)trap0(-29);					/* host_self_trap */
	u32 reply = (u32)trap0(-26);					/* mach_reply_port */
	u8 buf[1024];

	/* host_get_io_main (205): req = header only; reply port @28 */
	long kr = mig(buf, host, reply, 205, 24, 0);
	u32 master = g32(buf,28);
	whex(fd, "SWEET GPU RDTEST: io_main kr=", (u64)kr);
	whex(fd, "SWEET GPU RDTEST: master=", (u64)master);

	/* io_service_get_matching_service (2892): NDR, matchingOffset=0, matchingCnt, matching[] */
	const char *match = "<dict><key>IOProviderClass</key><string>SweetGPU</string></dict>";
	int mlen = (int)slen(match)+1;
	{ int o=24; p32(buf,o,0);p32(buf,o+4,0x00000001);o+=8; p32(buf,o,0);o+=4; p32(buf,o,mlen);o+=4;
	  for(int i=0;i<mlen;i++) buf[o+i]=match[i]; o+=mlen; while(o&3)o++;
	  kr = mig(buf, master, reply, 2892, o, 0); }
	u32 service = g32(buf,28);
	whex(fd, "SWEET GPU RDTEST: match kr=", (u64)kr);
	whex(fd, "SWEET GPU RDTEST: service=", (u64)service);
	if (!service) { w(fd,"SWEET GPU RDTEST: no service\n"); for(;;) sc3(93,0,0,0); }

	/* io_service_open_extended (2877): complex, 2 desc (task port + empty ool) */
	{ int o=24; p32(buf,o,2);o+=4;				/* msgh_body: 2 descriptors */
	  p32(buf,o,task);p32(buf,o+4,0);p32(buf,o+8,(0u)|(19u<<16)|(0u<<24));o+=12;	/* port: disp 19, type 0 */
	  p64(buf,o,0); buf[o+8]=0;buf[o+9]=2;buf[o+10]=0;buf[o+11]=1; p32(buf,o+12,0); o+=16; /* ool: copy=VIRTUAL(2), type 1, size 0 */
	  p32(buf,o,0);p32(buf,o+4,0x00000001);o+=8;		/* NDR */
	  p32(buf,o,0);o+=4;					/* connect_type */
	  p32(buf,o,0);p32(buf,o+4,0x00000001);o+=8;		/* ndr (arg) */
	  p32(buf,o,0);o+=4;					/* propertiesCnt */
	  kr = mig(buf, service, reply, 2877, o, 1); }
	u32 conn = g32(buf,28);
	whex(fd, "SWEET GPU RDTEST: open kr=", (u64)kr);
	whex(fd, "SWEET GPU RDTEST: conn=", (u64)conn);
	if (!conn) { w(fd,"SWEET GPU RDTEST: no conn\n"); for(;;) sc3(93,0,0,0); }

	/* wait for the GPU (externalMethod returns kIOReturnNotReady until ready) */
	u64 sout[4]; u32 got;
	for (int i=0;i<120;i++) {
		kr = connect_method(buf, conn, reply, SEL_INFO, 0,0, 0,0, 4, sout, &got);
		if ((u32)kr != kIOReturnNotReady) break;
		sc3(93,0,0,0);		/* select: ~idle one tick */
	}
	whex(fd, "SWEET GPU RDTEST: info kr=", (u64)(u32)kr);
	if (!kr) whex(fd, "SWEET GPU RDTEST: chip=", sout[0]);

	/* bo_new target + cmd; map each */
	u64 in = 16384; u32 th=0,ch=0; u64 tgt=0,cmd=0, tptr=0, cptr=0;
	kr = connect_method(buf, conn, reply, SEL_BONEW, &in,1, 0,0, 3, sout,&got);
	if (!kr){ th=(u32)sout[0]; tgt=sout[1]; }
	whex(fd, "SWEET GPU RDTEST: bo tgt kr=", (u64)(u32)kr);
	whex(fd, "SWEET GPU RDTEST: tgt iova=", tgt);
	kr = connect_method(buf, conn, reply, SEL_BONEW, &in,1, 0,0, 3, sout,&got);
	if (!kr){ ch=(u32)sout[0]; cmd=sout[1]; }
	whex(fd, "SWEET GPU RDTEST: cmd iova=", cmd);

	/* io_connect_map_memory_into_task (2878): complex, 1 desc (task), per handle */
	for (int which=0; which<2; which++) {
		u32 h = which? ch: th;
		int o=24; p32(buf,o,1);o+=4;
		p32(buf,o,task);p32(buf,o+4,0);p32(buf,o+8,(0u)|(19u<<16)|(0u<<24));o+=12;
		p32(buf,o,0);p32(buf,o+4,0x00000001);o+=8;	/* NDR */
		p32(buf,o,h);o+=4;				/* memory_type = handle */
		p64(buf,o,0);o+=8;				/* address (inout, 0) */
		p64(buf,o,0);o+=8;				/* size */
		p32(buf,o,kIOMapAnywhere);o+=4;			/* flags */
		kr = mig(buf, conn, reply, 2878, o, 1);
		u32 ret = g32(buf,32);
		u64 addr = g64(buf,36);
		if (which) cptr=addr; else tptr=addr;
		whex(fd, which? "SWEET GPU RDTEST: map cmd kr=":"SWEET GPU RDTEST: map tgt kr=", (u64)(u32)(kr?kr:ret));
	}
	if (!tptr || !cptr) { w(fd,"SWEET GPU RDTEST: map failed\n"); for(;;) sc3(93,0,0,0); }

	/* target = deadbeef; cmd = CP_MEM_WRITE sentinel to tgt iova */
	p32((u8*)tptr,0,0xdeadbeefu);
	u8 *ib=(u8*)cptr; int k=0;
	p32(ib,0,pkt7(0x3d,3)); p32(ib,4,(u32)tgt); p32(ib,8,(u32)(tgt>>32)); p32(ib,12,SENTINEL); k=4;

	/* submit: inband struct {nr_ibs, flags, iova, dwords, pad} */
	u8 sub[24]; p32(sub,0,1); p32(sub,4,0); p64(sub,8,cmd); p32(sub,16,(u32)k); p32(sub,20,0);
	kr = connect_method(buf, conn, reply, SEL_SUBMIT, 0,0, sub,24, 1, sout,&got);
	u64 seqno = (!kr && got) ? sout[0] : 0;
	whex(fd, "SWEET GPU RDTEST: submit kr=", (u64)(u32)kr);
	whex(fd, "SWEET GPU RDTEST: seqno=", seqno);

	/* wait */
	u64 win[2]={ seqno, 2000 };
	kr = connect_method(buf, conn, reply, SEL_WAIT, win,2, 0,0, 0, 0,0);
	whex(fd, "SWEET GPU RDTEST: wait kr=", (u64)(u32)kr);

	u32 got_val = g32((u8*)tptr,0);
	whex(fd, "SWEET GPU RDTEST: target=", (u64)got_val);
	w(fd, got_val==SENTINEL ? "SWEET GPU RDTEST: USER CLIENT OK\n" : "SWEET GPU RDTEST: USER CLIENT FAILED\n");
	w(fd, "SWEET GPU RDTEST: done; idle\n");
	for(;;) sc3(93,0,0,0);
}
