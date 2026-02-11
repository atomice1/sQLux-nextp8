/*
 * (c) UQLX - see COPYRIGHT
 */


/* define memory access fns */
#include "QL68000.h"
#include "memaccess.h"
#include "general.h"
#include "QL_screen.h"
#include "SDL2screen.h"
#include "unixstuff.h"
#include <unistd.h>

#ifdef NEXTP8
#include "funcval_testbench.h"
#include "emulator_options.h"
#endif

#ifdef PROFILER
#include "profiler/profiler_events.h"
#endif

extern bool asyncTrace;

static void log_mem_wr_long(aw32 addr, aw32 d)
{
	if (!asyncTrace)
		return;

	// 68000 is big-endian: high word at addr, low word at addr+2
	printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(d >> 16) & 0xffff);
	printf("MEM WR: addr=0x%x data=0x%x\n", addr + 2, (unsigned)d & 0xffff);
}

static int is_hw(uint32_t addr)
{
	if (((addr >= QL_INTERNAL_IO_BASE) &&
        	(addr < (QL_INTERNAL_IO_BASE + QL_INTERNAL_IO_SIZE))) ||
		((addr >= QL_INTERNAL_MEM_BASE) &&
			(addr < (QL_INTERNAL_MEM_BASE + QL_INTERNAL_MEM_SIZE)))) {
		return 1;
	}

	return 0;
}

rw8 ReadByte(aw32 addr)
{
	rw8 result;
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataRead(addr);
#endif

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		result = funcval_read_byte(addr);
		if (asyncTrace) {
			if (addr & 1)
				printf("MEM RD: addr=0x%x data=0xzz%02x\n", addr, (unsigned)result & 0xff);
			else
				printf("MEM RD: addr=0x%x data=0x%02xzz\n", addr, (unsigned)result & 0xff);
		}
		return result;
	}
#endif

	if (is_hw(addr)) {
		result = ReadHWByte(addr);
		if (asyncTrace) {
			printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)(((result << 8) | result)) & 0xffff);
		}
		return result;
	}

	if ((addr >= RTOP) && (addr >=qlscreen.qm_hi)) {
		result = 0;
		if (asyncTrace) {
			if (addr & 1)
				printf("MEM RD: addr=0x%x data=0xzz%02x\n", addr, (unsigned)result & 0xff);
			else
				printf("MEM RD: addr=0x%x data=0x%02xzz\n", addr, (unsigned)result & 0xff);
		}
		return result;
	}

	result = *((w8 *)memBase + addr);
	if (asyncTrace) {
		if (addr & 1)
			printf("MEM RD: addr=0x%x data=0xzz%02x\n", addr, (unsigned)result & 0xff);
		else
			printf("MEM RD: addr=0x%x data=0x%02xzz\n", addr, (unsigned)result & 0xff);
	}
	return result;
}

rw16 ReadWord(aw32 addr)
{
	rw16 result;
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataRead(addr);
#endif

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		result = funcval_read_word(addr);
		if (asyncTrace) printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)result & 0xffff);
		return result;
	}
#endif

	if (is_hw(addr)) {
		result = (w16)ReadHWWord(addr);
		if (asyncTrace) printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)result & 0xffff);
		return result;
	}

	if ((addr >= RTOP) && (addr >=qlscreen.qm_hi)) {
		result = 0;
		if (asyncTrace) printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)result & 0xffff);
		return result;
	}

	result = (w16)RW((w16 *)((Ptr)memBase + addr)); /* make sure it is signed */
	if (asyncTrace) printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)result & 0xffff);
	return result;
}

rw32 ReadLong(aw32 addr)
{
	rw32 result;
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataRead(addr);
#endif

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		result = funcval_read_long(addr);
		if (asyncTrace) {
			printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)(result >> 16) & 0xffff);
			printf("MEM RD: addr=0x%x data=0x%x\n", addr + 2, (unsigned)result & 0xffff);
		}
		return result;
	}
#endif

	if (is_hw(addr)) {
		result = (w32)ReadHWLong(addr);
		if (asyncTrace) {
			printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)(result >> 16) & 0xffff);
			printf("MEM RD: addr=0x%x data=0x%x\n", addr + 2, (unsigned)result & 0xffff);
		}
		return result;
	}

	if ((addr >= RTOP) && (addr >=qlscreen.qm_hi)) {
		result = 0;
		if (asyncTrace) {
			printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)(result >> 16) & 0xffff);
			printf("MEM RD: addr=0x%x data=0x%x\n", addr + 2, (unsigned)result & 0xffff);
		}
		return result;
	}

	result = (w32)RL((Ptr)memBase + addr); /* make sure is is signed */
	if (asyncTrace) {
		printf("MEM RD: addr=0x%x data=0x%x\n", addr, (unsigned)(result >> 16) & 0xffff);
		printf("MEM RD: addr=0x%x data=0x%x\n", addr + 2, (unsigned)result & 0xffff);
	}
	return result;
}

void WriteByte(aw32 addr,aw8 d)
{
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataWrite(addr);
#endif

	if (addr == 0x7ffffe || addr == 0x7fffff || addr < 32768) {
		printf("\n*** Write to non-writable address 0x%x (value=0x%02x) ***\n", addr, d & 0xff);
		DbgInfo();
		exit(1);
	}

	if (addr == 0xfffffe) {
		write(1, &d, 1);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(((d << 8) | d)) & 0xffff);
		return;
	} else if (addr == 0xffffff) {
		write(2, &d, 1);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(((d << 8) | d)) & 0xffff);
		return;
	}

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		funcval_write_byte(addr, d);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(((d << 8) | d)) & 0xffff);
		return;
	}
#endif

	if (is_hw(addr)) {
		WriteHWByte(addr, d);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(((d << 8) | d)) & 0xffff);
		return;
	}

	if ((addr >= RTOP) && (addr >= qlscreen.qm_hi))
		return;

	if (addr >= QL_SCREEN_BASE) {
		*((w8 *)memBase + addr) = d;
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)(((d << 8) | d)) & 0xffff);
	}
}

void WriteWord(aw32 addr,aw16 d)
{
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataWrite(addr);
#endif

	if (addr == 0x7ffffe || addr == 0x7fffff || addr < 32768) {
		printf("\n*** Write to non-writable address 0x%x (value=0x%04x) ***\n", addr, d & 0xffff);
		DbgInfo();
		exit(1);
	}

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		funcval_write_word(addr, d);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)d & 0xffff);
		return;
	}
#endif

	if (is_hw(addr)) {
		WriteHWWord(addr, d);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)d & 0xffff);
		return;
	}

	if ((addr >= RTOP) && (addr >= qlscreen.qm_hi))
		return;

	if (addr >= QL_SCREEN_BASE) {
		WW((Ptr)memBase + addr, d);
		if (asyncTrace) printf("MEM WR: addr=0x%x data=0x%x\n", addr, (unsigned)d & 0xffff);
	}
}

void WriteLong(aw32 addr,aw32 d)
{
	addr &= ADDR_MASK;

#ifdef PROFILER
	Profiler_RecordDataWrite(addr);
#endif

	if (addr == 0x7ffffe || addr == 0x7fffff || addr < 32768) {
		printf("\n*** Write to non-writable address 0x%x (value=0x%08x) ***\n", addr, d);
		DbgInfo();
		exit(1);
	}

#ifdef NEXTP8
	/* Check for FuncVal testbench access (3MB-4MB range) */
	if (funcval_mode && funcval_is_testbench_addr(addr)) {
		funcval_write_long(addr, d);
		log_mem_wr_long(addr, d);
		return;
	}
#endif

	if (is_hw(addr)) {
		WriteHWWord(addr, d >> 16);
		WriteHWWord(addr + 2, d);
		log_mem_wr_long(addr, d);
		return;
	}

	if ((addr >= RTOP) && (addr >=qlscreen.qm_hi))
		return;

	if (addr >= QL_SCREEN_BASE) {
		WL((Ptr)memBase + addr, d);
		log_mem_wr_long(addr, d);
	}
}

/*############################################################*/
int isreg=0;

rw8 ModifyAtEA_b(ashort mode,ashort r)
{
	shindex displ;
	w32 addr;

	isreg = 0;

	switch (mode)
	{
	case 0:
		isreg = 1;
		mea_acc = 0;
		lastAddr = 0;
		dest = (Ptr)(&reg[r]) + RBO;
		return *((w8 *)dest);
	case 2:
		addr = aReg[r];
		break;
	case 3:
		addr = aReg[r]++;
		if (r == 7)
			(*m68k_sp)++;
		break;
	case 4:
		if (r == 7)
			(*m68k_sp)--;
		addr = --aReg[r];
		break;
	case 5:
		addr = aReg[r] + (w16)RW_PC(pc++);
		break;
	case 6:
		displ = (w16)RW_PC(pc++);
		if ((displ & 2048) != 0)
			addr = reg[(displ >> 12) & 15] +
			       aReg[r] + (w32)((w8)displ);
		else
			addr = (w32)((w16)(reg[(displ >> 12) & 15])) +
			       aReg[r] + (w32)((w8)displ);
		break;
	case 7:
		switch (r)
		{
		case 0:
			addr = (w16)RW_PC(pc++);
			break;
		case 1:
			addr = RL_PC((w32 *)pc);
			pc += 2;
			break;
		default:
			exception = 4;
			extraFlag = true;
			nInst2 = nInst;
			nInst = 0;

			mea_acc = 0;
			lastAddr = 0;
			dest = (Ptr)(&dummy);
			return 0;
		}
		break;
	default:
		exception = 4;
		extraFlag = true;
		nInst2 = nInst;
		nInst = 0;

		mea_acc = 0;
		lastAddr = 0;
		dest = (Ptr)(&dummy);
		return 0;
	}

	addr &= ADDR_MASK;

	lastAddr = addr;
	dest = (Ptr)memBase + addr;
	return ReadByte(addr);
}

rw16 ModifyAtEA_w(ashort mode,ashort r)
{
	/*w16*/
	shindex displ;
	w32 addr = 0;

	isreg = 0;

	switch (mode)
	{
	case 0:
		isreg = 1;
		dest = (Ptr)(&reg[r]) + RWO;
		return *((w16 *)dest);
	case 1:
		isreg = 1;
		dest = (Ptr)(&aReg[r]) + RWO;
		return *((w16 *)dest);
	case 2:
		addr = aReg[r];
		break;
	case 3:
		addr = aReg[r];
		aReg[r] += 2;
		break;
	case 4:
		addr = (aReg[r] -= 2);
		break;
	case 5:
		addr = aReg[r] + (w16)RW_PC(pc++);
		break;
	case 6:
		displ = (w16)RW_PC(pc++);
		if ((displ & 2048) != 0)
			addr = reg[(displ >> 12) & 15] +
			       aReg[r] + (w32)((w8)displ);
		else
			addr = (w32)((w16)(reg[(displ >> 12) & 15])) +
			       aReg[r] + (w32)((w8)displ);
		break;
	case 7:
		switch (r)
		{
		case 0:
			addr = (w16)RW_PC(pc++);
			break;
		case 1:
			addr = RL_PC((w32 *)pc);
			pc += 2;
			break;
		default:
			exception = 4;
			extraFlag = true;
			nInst2 = nInst;
			nInst = 0;
			mea_acc = 0;
			dest = (Ptr)(&dummy);
			return 0;
		}
		break;
	}
	addr &= ADDR_MASK;

	lastAddr = addr;
	dest = (Ptr)memBase + addr;
	return ReadWord(addr);
}

rw32 ModifyAtEA_l(ashort mode, ashort r)
{
	/*w16*/
	shindex displ;
	w32 addr = 0;

	isreg = 0;

	switch (mode)
	{
	case 0:
		isreg = 1;
		dest = (Ptr)(&reg[r]);
		return *((w32 *)dest);
	case 1:
		isreg = 1;
		dest = (Ptr)(&aReg[r]);
		return *((w32 *)dest);
	case 2:
		addr = aReg[r];
		break;
	case 3:
		addr = aReg[r];
		aReg[r] += 4;
		break;
	case 4:
		addr = (aReg[r] -= 4);
		break;
	case 5:
		addr = aReg[r] + (w16)RW_PC(pc++);
		break;
	case 6:
		displ = (w16)RW_PC(pc++);
		if ((displ & 2048) != 0)
			addr = reg[(displ >> 12) & 15] +
			       aReg[r] + (w32)((w8)displ);
		else
			addr = (w32)((w16)(reg[(displ >> 12) & 15])) +
			       aReg[r] + (w32)((w8)displ);
		break;
	case 7:
		switch (r)
		{
		case 0:
			addr = (w16)RW_PC(pc++);
			break;
		case 1:
			addr = RL_PC((w32 *)pc);
			pc += 2;
			break;
		default:
			exception = 4;
			extraFlag = true;
			nInst2 = nInst;
			nInst = 0;
			mea_acc = 0;

			dest = (Ptr)(&dummy);
			return 0;
		}
		break;
	}

	addr &= ADDR_MASK;

	lastAddr = addr;
	dest = (Ptr)memBase + addr;
	return ReadLong(addr);
}

void RewriteEA_b(aw8 d)
{
	if (isreg)
		*((w8*)dest)=d;
	else {
		WriteByte(lastAddr, d);
	}
}

void RewriteEA_w(aw16 d)
{
	if (isreg) {
		*((w16*)dest)=d;
	} else {
		WriteWord(lastAddr, d);
	}
}

void RewriteEA_l(aw32 d)
{
	if (isreg) {
		*((w32*)dest)=d;
	} else {
		WriteLong(lastAddr, d);
	}
}
