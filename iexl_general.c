/*
 * (c) UQLX - see COPYRIGHT
 */

#include <stdio.h>

#include "QL68000.h"
#include "debug.h"
#include "SDL2screen.h"
#include "memaccess.h"
#include "mmodes.h"
#include "unixstuff.h"

#ifdef PROFILER
#include "profiler/profiler_events.h"
#endif

void    (**qlux_table)(void);

#ifdef DEBUG
//int trace_rts=0;
extern int trace_rts;
#endif

int extInt=0;

#ifdef DEBUG
#define TRR  {trace_rts=20;}
#else
#define TRR
#endif

#define D_ISREG


rw32
(*iexl_GetEA[8])(ashort) /*REGP1*/ ={GetEA_mBad,GetEA_mBad,GetEA_m2,GetEA_mBad,GetEA_mBad,
		    GetEA_m5,GetEA_m6,GetEA_m7};
vml rw32 (**ll_GetEA)(ashort) /*REGP1*/ =iexl_GetEA;

rw8
(*iexl_GetFromEA_b[8])(void)={GetFromEA_b_m0,GetFromEA_b_mBad,GetFromEA_b_m2,
			 GetFromEA_b_m3,GetFromEA_b_m4,GetFromEA_b_m5,GetFromEA_b_m6,GetFromEA_b_m7};
vml rw8 (**ll_GetFromEA_b)(void)=iexl_GetFromEA_b;


rw16
(*iexl_GetFromEA_w[8])(void)={GetFromEA_w_m0,GetFromEA_w_m1,GetFromEA_w_m2,
			 GetFromEA_w_m3,GetFromEA_w_m4,GetFromEA_w_m5,GetFromEA_w_m6,GetFromEA_w_m7};
vml rw16 (**ll_GetFromEA_w)(void)=iexl_GetFromEA_w;

rw32
(*iexl_GetFromEA_l[8])(void)={GetFromEA_l_m0,GetFromEA_l_m1,GetFromEA_l_m2,
			 GetFromEA_l_m3,GetFromEA_l_m4,GetFromEA_l_m5,GetFromEA_l_m6,GetFromEA_l_m7};
vml rw32 (**ll_GetFromEA_l)(void)=iexl_GetFromEA_l;

void
(*iexl_PutToEA_b[8])(ashort,aw8) /*REGP2*/ ={PutToEA_b_m0,PutToEA_b_mBad,PutToEA_b_m2,PutToEA_b_m3,PutToEA_b_m4,PutToEA_b_m5,PutToEA_b_m6,PutToEA_b_m7};

vml void (**ll_PutToEA_b)(ashort,aw8) /*REGP2*/ =iexl_PutToEA_b;

void
(*iexl_PutToEA_w[8])(ashort,aw16) /*REGP2*/ ={PutToEA_w_m0,PutToEA_w_m1,PutToEA_w_m2,
			    PutToEA_w_m3,PutToEA_w_m4,PutToEA_w_m5,PutToEA_w_m6,PutToEA_w_m7};

vml void (**ll_PutToEA_w)(ashort,aw16) /*REGP2*/ =iexl_PutToEA_w;

void
(*iexl_PutToEA_l[8])(ashort,aw32) /*REGP2*/ ={PutToEA_l_m0,PutToEA_l_m1,PutToEA_l_m2,
			    PutToEA_l_m3,PutToEA_l_m4,PutToEA_l_m5,PutToEA_l_m6,PutToEA_l_m7};

void (**ll_PutToEA_l)(ashort,aw32) /*REGP2*/ =iexl_PutToEA_l;

Cond
(*ConditionTrue[16])(void)={CondT,CondF,CondHI,CondLS,CondCC,CondCS,CondNE,
			    CondEQ,CondVC,CondVS,CondPL,CondMI,CondGE,CondLT,CondGT,CondLE};
vml Cond (**ll_ConditionTrue)(void)=ConditionTrue;

#ifndef G_reg
w32             reg[16];                        /* registri d0-d7/a0-a7 */
#endif
w32             usp,ssp;                        /* user and system stack
						   pointer (aggiornato solo quello non attivo) */

#ifndef GREGS
uw16    *pc;                            /* program counter : Ptr nella */
gshort    code;
int      nInst;
#endif

Cond    trace,supervisor,xflag,negative,zero,overflow,carry;    /*flags */
char    iMask;                          /* SR interrupt mask */
Cond    stopped;                        /* processor status */
volatile char   pendingInterrupt;       /* interrupt requesting service */


#ifndef ZEROMAP
w32             *memBase;                        /* Ptr to ROM in Mac memory */
#endif

w32             *ramTop;                        /* Ptr to RAM top in Mac
						   memory */
w32             RTOP;                           /* QL ram top address */
short   exception;                      /* pending exception */
w32             badAddress;                     /* bad address address */
w16             readOrWrite;            /* bad address action */
w32             dummy;                          /* free 4 bytes for who care */
Ptr             dest;                           /* Mac address for
read+write operations */

#if 1
Cond mea_acc;
#else
Cond    isHW;                           /* dest is a HW register ? */
#if !defined(VM_SCR)
Cond    isDisplay;                      /* dest is in display RAM ? */
#endif
#endif
w32             lastAddr;                       /* QL address for
						   read+write operations */

volatile Cond   extraFlag;      /* signals exception or trace */


char    dispScreen=0;           /* screen 0 or 1 */
Cond    dispMode=0;                     /* mode 4 or 8 */
Cond    dispActive=true;        /* display is on ? */
Cond    badCodeAddress;

int   nInst2;
extern int script;

volatile w8     intReg=0;
volatile w8     theInt=0;

Cond doTrace;            /* trace after current instruction */

bool asyncTrace;
bool check_calling_convention = false;
bool exit_on_cpu_disable = true;  /* exit emulator when CPU is disabled (RESET_REQ = 0xff) */

/* Calling convention checking: stack to track saved register state */
#define CC_STACK_MAX 1024
typedef struct {
    w32 pc;         /* PC at call site */
    w32 regs[14];   /* a2-a7 (indices 0-5), d2-d7 (indices 6-11), and extra slots */
} cc_frame_t;

static cc_frame_t cc_stack[CC_STACK_MAX];
static int cc_stack_depth = 0;

void cc_push_frame(w32 call_pc) {
    if (!check_calling_convention)
        return;

    if (cc_stack_depth >= CC_STACK_MAX) {
        fprintf(stderr, "WARNING: Calling convention stack overflow at PC=0x%08x\n", call_pc);
        return;
    }

    cc_frame_t *frame = &cc_stack[cc_stack_depth++];
    frame->pc = call_pc;
    /* Save a2-a7 (aReg indices 2-7) */
    frame->regs[0] = aReg[2];
    frame->regs[1] = aReg[3];
    frame->regs[2] = aReg[4];
    frame->regs[3] = aReg[5];
    frame->regs[4] = aReg[6];
    frame->regs[5] = aReg[7];
    /* Save d2-d7 (reg indices 2-7) */
    frame->regs[6] = reg[2];
    frame->regs[7] = reg[3];
    frame->regs[8] = reg[4];
    frame->regs[9] = reg[5];
    frame->regs[10] = reg[6];
    frame->regs[11] = reg[7];
}

void cc_pop_frame_and_check(w32 return_pc) {
    if (!check_calling_convention)
        return;

    if (cc_stack_depth <= 0) {
        fprintf(stderr, "WARNING: Calling convention stack underflow at RTS to PC=0x%08x (RTS without matching JSR/BSR)\n", return_pc);
        return;
    }

    cc_frame_t *frame = &cc_stack[--cc_stack_depth];
    int violations = 0;

    /* Check a2-a7 */
    if (frame->regs[0] != aReg[2]) { fprintf(stderr, "WARNING: a2 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[0], aReg[2], frame->pc, return_pc); violations++; }
    if (frame->regs[1] != aReg[3]) { fprintf(stderr, "WARNING: a3 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[1], aReg[3], frame->pc, return_pc); violations++; }
    if (frame->regs[2] != aReg[4]) { fprintf(stderr, "WARNING: a4 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[2], aReg[4], frame->pc, return_pc); violations++; }
    if (frame->regs[3] != aReg[5]) { fprintf(stderr, "WARNING: a5 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[3], aReg[5], frame->pc, return_pc); violations++; }
    if (frame->regs[4] != aReg[6]) { fprintf(stderr, "WARNING: a6 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[4], aReg[6], frame->pc, return_pc); violations++; }
    /* Note: a7 (SP) is allowed to change within reason, but should be restored. We check it but it's common to see variations */
    if (frame->regs[5] != aReg[7]) {
        w32 sp_diff = (aReg[7] > frame->regs[5]) ? (aReg[7] - frame->regs[5]) : (frame->regs[5] - aReg[7]);
        if (sp_diff > 16) {  /* Allow small stack adjustments */
            fprintf(stderr, "WARNING: a7/SP modified (0x%08x -> 0x%08x, diff=%d) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[5], aReg[7], (int)sp_diff, frame->pc, return_pc);
            violations++;
        }
    }

    /* Check d2-d7 */
    if (frame->regs[6] != reg[2]) { fprintf(stderr, "WARNING: d2 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[6], reg[2], frame->pc, return_pc); violations++; }
    if (frame->regs[7] != reg[3]) { fprintf(stderr, "WARNING: d3 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[7], reg[3], frame->pc, return_pc); violations++; }
    if (frame->regs[8] != reg[4]) { fprintf(stderr, "WARNING: d4 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[8], reg[4], frame->pc, return_pc); violations++; }
    if (frame->regs[9] != reg[5]) { fprintf(stderr, "WARNING: d5 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[9], reg[5], frame->pc, return_pc); violations++; }
    if (frame->regs[10] != reg[6]) { fprintf(stderr, "WARNING: d6 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[10], reg[6], frame->pc, return_pc); violations++; }
    if (frame->regs[11] != reg[7]) { fprintf(stderr, "WARNING: d7 modified (0x%08x -> 0x%08x) in function called at PC=0x%08x, returning to PC=0x%08x\n", frame->regs[11], reg[7], frame->pc, return_pc); violations++; }

    if (violations > 0) {
        fprintf(stderr, "ERROR: %d calling convention violation(s) detected!\n", violations);
    }
}

void cc_poison_scratch_regs(void) {
    if (!check_calling_convention)
        return;

    /* Poison only caller-saved non-return-value registers: a1 */
    /* a0, d0 and d1 are return value registers and must NOT be poisoned */
    aReg[1] = 0xDEADBEEF;
}

void ProcessInterrupts(void)
{
  /* gestione interrupts */
  if( exception==0 && (pendingInterrupt==7 || pendingInterrupt>iMask)
      && !doTrace)
    {
      if(!supervisor)
	{
	  usp=(*m68k_sp);
	  (*m68k_sp)=ssp;
	}
      ExceptionIn(24+pendingInterrupt);
      WriteLong((*m68k_sp)-4,(Ptr)pc-(Ptr)memBase);
      (*m68k_sp)-=6;
      WriteWord(*m68k_sp,GetSR());
      SetPCX(24+pendingInterrupt);
      iMask=pendingInterrupt;
      pendingInterrupt=0;
      supervisor=true;
      trace=false;
      stopped=false;
      extraFlag=false;
    }
}

rw16 GetSR(void)
{
  rw16 sr;
  sr=(w16)iMask<<8;
  if(trace) sr|=0x8000;
  if(supervisor) sr|=0x2000;
  if(xflag) sr|=16;
  if(negative) sr|=8;
  if(zero) sr|=4;
  if(overflow) sr|=2;
  if(carry) sr|=1;
  return sr;
}

void REGP1 PutSR(aw16 sr)
{
  Cond oldSuper;
  oldSuper=supervisor;
  trace=(sr&0x8000)!=0;
  extraFlag=doTrace || trace || exception!=0;
  if(extraFlag)
    {
      nInst2=nInst;
      nInst=0;
    }
  supervisor=(sr&0x2000)!=0;
  xflag=(sr&0x0010)!=0;
  negative=(sr&0x0008)!=0;
  zero=(sr&0x0004)!=0;
  overflow=(sr&0x0002)!=0;
  carry=(sr&0x0001)!=0;
  iMask=(char)(sr>>8)&7;
  if(oldSuper!=supervisor)
    {
      if(supervisor)
	{
	  usp=(*m68k_sp);
	  (*m68k_sp)=ssp;
	}
      else
	{
	  ssp=(*m68k_sp);
	  (*m68k_sp)=usp;
	}
    }
  ProcessInterrupts();
}

rw16 REGP1 BusErrorCode(aw16 dataOrCode)
{
  if(supervisor) dataOrCode+=4;
  return dataOrCode+readOrWrite+8;
}


void REGP1 SetPCX(int i)
{
#ifdef BACKTRACE
  Ptr p=pc;
#endif

  pc=(uw16*)((Ptr)memBase+(RL(&memBase[i])&ADDR_MASK));

#ifdef TRACE
  CheckTrace();
#ifdef BACKTRACE
  AddBackTrace(p,-i);
#endif
#endif

  if(((char)(uintptr_t)pc&1)!=0)
    {
      exception=3;
      extraFlag=true;
      nInst2=nInst;
      nInst=0;
      readOrWrite=16;
      badAddress=(Ptr)pc-(Ptr)memBase;
      badCodeAddress=true;
    }
}

#ifdef BACKTRACE
void SetPCB(w32 addr, int type)
{
  /*  printf("SetPC: addr=%x\n",addr); */

  Ptr p=pc;


  if(((char)addr&1)!=0)
    {
      exception=3;
      extraFlag=true;
      nInst2=nInst;
      nInst=0;
      readOrWrite=16;
      badAddress=addr;
      badCodeAddress=true;

      return;
    }

  pc=(uw16*)((Ptr)memBase+(addr&ADDR_MASK));

  CheckTrace();
  AddBackTrace(p,type);
}

#endif

void REGP1 SetPC(w32 addr)
{
  /*  printf("SetPC: addr=%x\n",addr); */

  if(((char)addr&1)!=0)
    {
      exception=3;
      extraFlag=true;
      nInst2=nInst;
      nInst=0;
      readOrWrite=16;
      badAddress=addr;
      badCodeAddress=true;

      return;
    }

  pc=(uw16*)((Ptr)memBase+(addr&ADDR_MASK));
#ifdef TRACE
  CheckTrace();
#endif
}

#if 0
void ShowException(void){}
#else
void ShowException(void)
{
  short i;
  int p1,p2,p4;
  unsigned char *p3;

  long xc=exception+3;

  if (exception==0) return;


  p1=(xc);
  p2=((Ptr)pc-(Ptr)memBase-(xc==4? 0:2));
  if(xc==4)
    {
      p3="Illegal code=";
      p4=(code);
    }
  else
    {
      p3="";
      if(xc==3) p3="address error";
      if(xc==5) p3="divide by zero";
      if(xc==6) p3="CHK instruction";
      if(xc==7) p3="TRAPV instruction";
      if(xc==8) p3="privilege violation";
      if(xc==9) p3="trace xc";
      if(xc==10) p3="Axxx instruction code";
      if(xc==11) p3="Fxxx instruction code";
      if(xc>35 && xc<48) {p3="TRAP instruction"; p4=xc-35;}
      else p4=0;
    }
  printf("Exception %s %d at PC=%x, xx=%d\n",p3,p1,p2,p4);
}
#endif

extern int tracetrap;

#define UpdateNowRegisters()
#if 0
#define ExceptionIn(x)
#else
void REGP1 ExceptionIn(char x)
{
  if (!tracetrap) return;

  printf("Entering TRAP #%d\n",x-32);
  DbgInfo();
}
#endif
#if 1
void ExceptionOut()
{
  if (!tracetrap) return;

  printf("RTE\n");
  DbgInfo();
}

#endif

void ExceptionProcessing()
{
  if(pendingInterrupt!=0 && !doTrace) ProcessInterrupts();
  if(exception!=0)
    {
      if(exception<32 || exception>36) /* tutte le eccezioni
					  tranne le trap 0-4 */
	{
	  extraFlag=exception<3 || (exception>9 &&
				    exception<32) || exception>47;
	  if(!extraFlag) extraFlag=ReadLong(0x28050l)==0;
	  if(extraFlag)
	    {
	      UpdateNowRegisters();
	      ShowException();
	      nInst=nInst2=0;
	    }
	}
      if(!supervisor)
	{
	  usp=(*m68k_sp);
	  (*m68k_sp)=ssp;
	}
      ExceptionIn(exception);
      (*m68k_sp)-=6;
      WriteLong((*m68k_sp)+2,(uintptr_t)pc-(uintptr_t)memBase);
      WriteWord((*m68k_sp),GetSR());
      SetPCX(exception);
      if(exception==3) /* address error */
	{
	  (*m68k_sp)-=8;
	  WriteWord((*m68k_sp)+6,code);
	  WriteLong((*m68k_sp)+2,badAddress);
	  WriteWord((*m68k_sp),BusErrorCode(badCodeAddress? 2:1));
	  badCodeAddress=false;
	  if(nInst) exception=0;
	} else exception=0; /* allow interrupts */
      extraFlag=false;
      supervisor=true;
      trace=false;
    }
   if(doTrace)
    {
      if(!supervisor)
	{
	  usp=(*m68k_sp);
	  (*m68k_sp)=ssp;
	}
      ExceptionIn(9);
      (*m68k_sp)-=6;
      WriteLong((*m68k_sp)+2,(Ptr)pc-(Ptr)memBase);
      WriteWord((*m68k_sp),GetSR());
      SetPCX(9);
      if(nInst==0) exception=9;       /* no interrupt allowed here */
      supervisor=true;
      /*doTrace=*/trace=false;
      extraFlag=false;
      stopped=false;
    }
  doTrace=trace;
  if(doTrace) {nInst2=nInst;nInst=1;}
  if(pendingInterrupt!=0 && !doTrace)   /* delay interrupt after trace exception */
    {
      extraFlag=true;
      nInst2=nInst;
      nInst=0;
    }

}

/******************************************************************/
/* now read in ReadByte etc macros */

rw32 AREGP GetEA_mBad(ashort r)
{
  exception=4;
  extraFlag=true;
  nInst2=nInst;
  nInst=0;
  return 0;
}

static char buf1[32], buf2[32], buf3[32], buf4[32], buf5[32], buf6[32], buf7[32], buf8[32], buf9[32];
static char buf10[32], buf11[32], buf12[32], buf13[32], buf14[32], buf15[32], buf16[32], buf17[32];

static const char *change_to_str(char *buf, uint32_t old_val, uint32_t new_val) {
    if (old_val == new_val) {
        snprintf(buf, 32, "0x%x", new_val);
    } else {
        snprintf(buf, 32, "0x%x->0x%x", old_val, new_val);
    }
    return buf;
}

void ExecuteLoop(void)  /* fetch and dispatch loop */
{
  while(--nInst>=0)
    {
      const bool trace = asyncTrace;
#ifdef TRACE
      if (pc>tracelo) DoTrace();
#endif
    uint32_t old_pc, old_d0, old_d1, old_d2, old_d3, old_d4, old_d5, old_d6, old_d7;
    uint32_t old_a0, old_a1, old_a2, old_a3, old_a4, old_a5, old_a6, old_a7;
    if (trace) {
        old_pc = (void*)pc-(void*)memBase;
        old_d0 = reg[0];
        old_d1 = reg[1];
        old_d2 = reg[2];
        old_d3 = reg[3];
        old_d4 = reg[4];
        old_d5 = reg[5];
        old_d6 = reg[6];
        old_d7 = reg[7];
        old_a0 = reg[8];
        old_a1 = reg[9];
        old_a2 = reg[10];
        old_a3 = reg[11];
        old_a4 = reg[12];
        old_a5 = reg[13];
        old_a6 = reg[14];
        old_a7 = reg[15];
    }
    //printf("ExecuteLoop: pc = %x\n", (w32)((void*)pc-(void*)memBase));
    /*if ((w32)((void*)pc-(void*)memBase) >= 0x40000 ) {
      printf("ExecuteLoop: pc = %x\n", (w32)((void*)pc-(void*)memBase));
      if ((w32)((void*)pc-(void*)memBase) > 0x451d8 ) {
        fflush(stdout);
        fprintf(stderr, "Invalid PC address: %x\n", (w32)((void*)pc-(void*)memBase));
        exit(1);
      }
    }*/
#ifdef PROFILER
      // Record instruction execution
      Profiler_RecordInstructionExecute((w32)((void*)pc-(void*)memBase));
#endif
      qlux_table[code=RW_PC(pc++)&0xffff]();

      if (trace) {
        uint32_t new_pc = (char*)pc-(char*)memBase;
        printf("PC=%s D0=%s D1=%s D2=%s D3=%s D4=%s D5=%s D6=%s D7=%s A0=%s A1=%s A2=%s A3=%s A4=%s A5=%s A6=%s A7=%s\n",
               change_to_str(buf1, old_pc, new_pc),
               change_to_str(buf2, old_d0, reg[0]),
               change_to_str(buf3, old_d1, reg[1]),
               change_to_str(buf4, old_d2, reg[2]),
               change_to_str(buf5, old_d3, reg[3]),
               change_to_str(buf6, old_d4, reg[4]),
               change_to_str(buf7, old_d5, reg[5]),
               change_to_str(buf8, old_d6, reg[6]),
               change_to_str(buf9, old_d7, reg[7]),
               change_to_str(buf10, old_a0, reg[8]),
               change_to_str(buf11, old_a1, reg[9]),
               change_to_str(buf12, old_a2, reg[10]),
               change_to_str(buf13, old_a3, reg[11]),
               change_to_str(buf14, old_a4, reg[12]),
               change_to_str(buf15, old_a5, reg[13]),
               change_to_str(buf16, old_a6, reg[14]),
               change_to_str(buf17, old_a7, reg[15]));
        fflush(stdout);
      }
    }

  if (SDL_AtomicGet(&doPoll)) dosignal();

  if(extraFlag)
    {
      nInst=nInst2;
      ExceptionProcessing();
      if(nInst>0) ExecuteLoop();
    }
}

void ExecuteChunk(long n)       /* execute n emulated 68K istructions */
{
  if((uintptr_t)pc&1) return;

  extraFlag=false;
  ProcessInterrupts();

  if(stopped) return;
  exception=0;

  extraFlag=trace || doTrace || pendingInterrupt==7 ||
    pendingInterrupt>iMask;

  nInst=n+1;
  if(extraFlag)
    {
      nInst2=nInst;
      nInst=0;
    }

  ExecuteLoop();
}

void InitialSetup(void) /* 68K state when powered on */
{
  ssp=*m68k_sp=RL(&memBase[0]);
  SetPC(RL(&memBase[1]));
  if(V3)printf("initial PC=%x SP=%x\n",(w32)((void*)pc-(void*)memBase),ssp);

  iMask=7;
  supervisor=true;
  trace=doTrace=false;
  exception=0;
  extraFlag=false;
  pendingInterrupt=0;
  stopped=false;
  badCodeAddress=false;
}

void DumpState(void)
{
	printf("PC=%lx\n", (unsigned long)((char *)pc - (char *)memBase));
	for (int i=-8;i<=8;i+=2)
	    printf("%lx: %lx\n", (unsigned long)((char *)pc - (char *)memBase + i), (unsigned long)ReadWord((aw32)((char *)pc - (char *)memBase) + i));
	printf("*SP=%lx\n", (unsigned long)ReadLong(*m68k_sp));
	for (int i=0;i<=16;i+=2)
	    printf("%lx: %lx\n", (unsigned long)(*m68k_sp+i), (unsigned long)ReadLong(*m68k_sp+i));
	printf("Backtrace:\n");
	uint32_t fp = ReadLong(*(aReg+6));
	while (fp) {
		uint32_t ret = ReadLong(fp+4);
		if (ret)
			printf("  %lx\n", (unsigned long) ret);
		fp = ReadLong(fp);
	}
  fflush(stdout);
}
