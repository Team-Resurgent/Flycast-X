/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86

#include "rec_x86.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/addrspace.h"
#include "oslib/unwind_info.h"
#include "emulator.h"

static void (*mainloop)();
static void (*ngen_FailedToFindBlock_)();

static void (*intc_sched)();
static void (*no_update)();
static void (*ngen_LinkBlock_cond_Next_stub)();
static void (*ngen_LinkBlock_cond_Branch_stub)();
static void (*ngen_LinkBlock_Generic_stub)();
static void (*ngen_blockcheckfail)();
void (*X86Compiler::handleException)();

static Xbyak::Operand::Code alloc_regs[] {  Xbyak::Operand::EBX,  Xbyak::Operand::EBP,  Xbyak::Operand::ESI,  Xbyak::Operand::EDI, (Xbyak::Operand::Code)-1 };

// Execution-shape counter (defined in main_xbox.cpp): block entries, bumped by
// an `inc` emitted in every block prologue. kJitCounters gates EMISSION (a C++
// compile-time constant folded at codegen time): false = zero runtime cost.
static const bool kJitCounters = false;

// Folded per-block cycle check (sub sets the flags; no separate load+test).
// See the prologue in X86Compiler::compile. Flip to false to restore the
// original 4-instruction check if any timing/interrupt misbehaviour appears.
static const bool kSh4FastCycleCheck = true;

// JIT audit builds: emit a per-BLOCK execution counter (inc of profRuns in the
// RuntimeBlockInfo) so bm_DumpHotBlocks can print the hottest blocks' emitted
// x86 for offline inspection. Costs one memory inc per block entry -- flip to
// false for release builds.
static const bool kBlockProfile = false;

// Idle-loop skip (set from main_xbox.cpp per game id, 0 = off). The HOTBLOCKS
// audit (2026-07-05, MvC2 fight) showed ~55% of ALL guest time spinning in the
// game's wait-for-vblank loop (poll block + tiny task dispatcher + empty
// callback, ~2.2M iterations/s) -- host CPU burned emulating no-ops. When a
// block's vaddr matches, its prologue zeroes cycle_counter so every entry
// drains the timeslice through intc_sched: guest time advances a full
// timeslice per poll instead of ~50 cycles, cutting spin iterations ~9x.
// Semantically transparent: the loop still exits at the exact guest time the
// vblank ISR sets the flag; only the poll granularity coarsens (~2us guest).
// Validated on hardware: MvC2 fights 74-84% -> 90-100%.
// g_idleSkipOp = first SH4 word of the verified poll loop; the drain is only
// emitted while that instruction is actually present at the address.
extern "C" u32 g_idleSkipPc;
extern "C" u16 g_idleSkipOp;

// Automatic version of the above, for EVERY game (no per-game table needed).
// Compile-time fingerprint marks candidates: tiny (<=6 guest ops), memory
// READ-ONLY (no writem/ifb/pref/sync -- work loops write their results, wait
// loops don't), branching backward onto/near themselves. Candidates carry a
// run counter; bm_AutoIdleScan (called ~1/s by the platform loop) arms any
// candidate spinning above wait-loop rates and recompiles it with the drain.
// A false positive would only make that loop's guest time pass faster (no
// corruption -- read-only), and every arming is logged for blacklisting.
// DISABLED 2026-07-05: hardware testing showed the fingerprint (tiny,
// read-only, fast) also matches loops whose ITERATION COUNT does bounded work
// (CD-loader pumps, boot decompressors, possibly mid-frame handshakes) --
// symptoms: slow boots, game-internal sluggishness while speed reads ~100%.
// Needs a verify-by-trial redesign before re-enabling.
static const bool kAutoIdleDetect = false;
extern "C" unsigned g_cnt_block;
// Code-region markers (defined in main_xbox.cpp, assigned by driver.cpp /
// genMemHandlers): used by rewrite() to tell an INLINE fastmem fault (pc inside
// a block body) from a handler fastmem fault (pc inside the generated handlers).
extern "C" u8 *g_sh4CacheStart;
extern "C" u32 g_sh4CacheSize;
extern "C" const u8 *g_memHandlerStart, *g_memHandlerEnd;
// COMPILE-time emission counters (bumped by the compiler, not emitted code):
// which relinkBlock branch each block-exit actually got. Diagnoses why
// link/win wired stays 0 (suspicion: gameplay blocks compile with mmu OFF).
extern "C" unsigned g_cnt_emitMmuLink;   // mmu identity-linked exit emitted
extern "C" unsigned g_cnt_emitMmuNoUpd;  // mmu no_update (unlinkable) exit emitted
extern "C" unsigned g_cnt_emitNonMmu;    // non-mmu linkable exit emitted
static s8 alloc_fregs[] = { 7, 6, 5, 4, -1 };
alignas(16) static f32 thaw_regs[4];
UnwindInfo unwinder;

static u64 jmp_esp;

void X86RegAlloc::doAlloc(RuntimeBlockInfo* block)
{
	RegAlloc::DoAlloc(block, alloc_regs, alloc_fregs);
}
void X86RegAlloc::Preload(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->regPreload(reg, nreg);
}
void X86RegAlloc::Writeback(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->regWriteback(reg, nreg);
}
void X86RegAlloc::Preload_FPU(u32 reg, s8 nreg)
{
	compiler->regPreload_FPU(reg, nreg);
}
void X86RegAlloc::Writeback_FPU(u32 reg, s8 nreg)
{
	compiler->regWriteback_FPU(reg, nreg);
}

struct DynaRBI : RuntimeBlockInfo
{
	DynaRBI(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer)
	: sh4ctx(sh4ctx), codeBuffer(codeBuffer) {}
	u32 Relink() override;

private:
	Sh4Context& sh4ctx;
	Sh4CodeBuffer& codeBuffer;
};


void X86Compiler::alignStack(int amount)
{
#ifndef _WIN32
	if (amount > 0)
		add(esp, amount);
	else
		sub(esp, -amount);
	unwinder.allocStackPtr(getCurr(), -amount);
#endif
}

void X86Compiler::compile(RuntimeBlockInfo* block, bool force_checks, bool optimise)
{
	DEBUG_LOG(DYNAREC, "X86 compiling %08x to %p", block->addr, codeBuffer.get());
	current_opid = -1;

	unwinder.start((void *)getCurr());
	unwinder.pushReg(0, Xbyak::Operand::ESI);
	unwinder.pushReg(0, Xbyak::Operand::EDI);
	unwinder.pushReg(0, Xbyak::Operand::EBP);
	unwinder.pushReg(0, Xbyak::Operand::EBX);
#ifndef _WIN32
	// 16-byte alignment
	unwinder.allocStack(0, 12);
#endif
	unwinder.endProlog(0);

	checkBlock(force_checks, block);
	if (mmu_enabled() && block->has_fpu_op)
	{
		Xbyak::Label fpu_enabled;
		mov(eax, dword[&sh4ctx.sr.status]);
		test(eax, 0x8000);			// test SR.FD bit
		jz(fpu_enabled);
		push(Sh4Ex_FpuDisabled);	// exception code
		push(block->vaddr);			// pc
		call((void (*)())Do_Exception);
		add(esp, 8);
		mov(ecx, dword[&sh4ctx.pc]);
		jmp((const void *)no_update);
		L(fpu_enabled);
	}

	// Execution-shape counter (defined in main_xbox.cpp): block entries/frame.
	// Diagnostic builds only -- ~350k emitted incs/frame cost ~1ms at 733MHz.
	if (kJitCounters)
		inc(dword[&g_cnt_block]);

	// Automatic idle-loop detection (see kAutoIdleDetect above): flag blocks
	// with the wait-loop fingerprint and count their executions.
	if (kAutoIdleDetect
			&& block->guest_opcodes <= 6
			&& block->BranchBlock != 0xFFFFFFFF
			&& block->BranchBlock <= block->vaddr
			&& block->vaddr - block->BranchBlock <= 64)
	{
		bool readOnly = true;
		for (const shil_opcode& op : block->oplist)
			if (op.op == shop_writem || op.op == shop_ifb || op.op == shop_pref
					|| op.op == shop_sync_sr || op.op == shop_sync_fpscr)
			{
				readOnly = false;
				break;
			}
		block->idleCandidate = readOnly;
	}
	if (kBlockProfile || block->idleCandidate)
		inc(dword[&block->profRuns]);	// audit dump and/or auto idle detection

	if (g_idleSkipPc != 0 && block->vaddr == g_idleSkipPc)
	{
		// Idle-loop skip (see g_idleSkipPc above): force the timeslice drained
		// so this entry goes through intc_sched and guest time fast-forwards.
		// Fingerprint-guarded: only while the VALIDATED poll instruction is
		// loaded at that address -- during boot the same RAM holds loader
		// code, and draining an iteration-bound work loop slows it massively.
		const u16 *op = (const u16 *)GetMemPtr(block->addr, 2);
		if (op != nullptr && *op == g_idleSkipOp)
			mov(dword[&sh4ctx.cycle_counter], 0);
	}
	else if (bm_IsAutoIdleBlock(block))
		mov(dword[&sh4ctx.cycle_counter], 0);	// auto entries (kAutoIdleDetect builds)
	Xbyak::Label no_up;
	if (kSh4FastCycleCheck)
	{
		// Folded cycle check: let the sub itself produce the flags instead of
		// a separate load+test (2 instructions + 1 L1 access on EVERY block
		// entry, ~350k/frame in MvC2). Semantics preserved: the check still
		// runs before the block body; the slow path undoes the decrement so
		// intc_sched sees the exact same counter value as the old code, and
		// redoes it only on the continue path (do_iter redirects skip it,
		// exactly like the old order where the sub came after the call). The
		// only difference is the timeslice boundary can trigger up to one
		// block's guest_cycles early -- bounded jitter well below the natural
		// block-length variance.
		sub(dword[&sh4ctx.cycle_counter], block->guest_cycles);
		jg(no_up);
		add(dword[&sh4ctx.cycle_counter], block->guest_cycles);
		mov(ecx, block->vaddr);
		call((const void *)intc_sched);
		sub(dword[&sh4ctx.cycle_counter], block->guest_cycles);
		L(no_up);
	}
	else
	{
		mov(eax, dword[&sh4ctx.cycle_counter]);
		test(eax, eax);
		jg(no_up);
		mov(ecx, block->vaddr);
		call((const void *)intc_sched);
		L(no_up);
		sub(dword[&sh4ctx.cycle_counter], block->guest_cycles);
	}

	regalloc.doAlloc(block);

	for (current_opid = 0; current_opid < block->oplist.size(); current_opid++)
	{
		shil_opcode& op  = block->oplist[current_opid];

		regalloc.OpBegin(&op, current_opid);

		genOpcode(block, optimise, op);

		regalloc.OpEnd(&op);
	}
	regalloc.Cleanup();
	current_opid = -1;

	block->relink_offset = getCurr() - getCode();
	block->relink_data = 0;
	relinkBlock(block);

	block->code = (DynarecCodeEntryPtr)getCode();
	block->host_code_size = getSize();

	size_t unwindSize = unwinder.end(getSize());
	setSize(getSize() + unwindSize);

	codeBuffer.advance(getSize());
}

u32 X86Compiler::relinkBlock(RuntimeBlockInfo* block)
{
	const u8 *startPosition = getCurr();

#define BLOCK_LINKING
#ifndef BLOCK_LINKING
	switch (block->BlockType) {

	case BET_StaticJump:
	case BET_StaticCall:
		//next_pc = block->BranchBlock;
		mov(ecx, block->BranchBlock);
		break;

	case BET_Cond_0:
	case BET_Cond_1:
		{
			//next_pc = next_pc_value;
			//if (*jdyn == 0)
			//next_pc = branch_pc_value;

			mov(ecx, block->NextBlock);

			cmp(dword[block->has_jcond ? &sh4ctx.jdyn : &sh4ctx.sr.T], (u32)block->BlockType & 1);
			Xbyak::Label branch_not_taken;

			jne(branch_not_taken, T_SHORT);
			mov(ecx, block->BranchBlock);
			L(branch_not_taken);
		}
		break;

	case BET_DynamicJump:
	case BET_DynamicCall:
	case BET_DynamicRet:
		//next_pc = *jdyn;
		mov(ecx, dword[&sh4ctx.jdyn]);
		break;

	case BET_DynamicIntr:
	case BET_StaticIntr:
		if (block->BlockType == BET_DynamicIntr)
		{
			//next_pc = *jdyn;
			mov(ecx, dword[&sh4ctx.jdyn]);
			mov(dword[&sh4ctx.pc], ecx);
		}
		else
		{
			//next_pc = next_pc_value;
			mov(dword[&sh4ctx.pc], block->NextBlock);
		}
		call(UpdateINTC);
		mov(ecx, dword[&sh4ctx.pc]);
		break;

	default:
		die("Invalid block end type");
	}

	jmp((const void *)no_update);

#else

	switch(block->BlockType)
	{
	case BET_Cond_0:
	case BET_Cond_1:
		cmp(dword[block->has_jcond ? &sh4ctx.jdyn : &sh4ctx.sr.T], (u32)block->BlockType & 1);

		if (mmu_enabled())
		{
			// MMU block linking, identity regions only: P1/P2/P4 vaddrs
			// (fast_reg_lut nonzero) never translate -- the SH4 architecture
			// fixes their mapping -- so a static branch between them links
			// exactly like the non-MMU case. Counters showed ~350k block
			// dispatches/frame in WinCE games (avg block ~6 instrs), each
			// paying the full no_update dispatch; a linked exit pays one pc
			// store + direct jmp. The pc store is required because the target
			// block's entry check compares sh4ctx.pc to its vaddr
			// (checkBlock). Stub call and direct jmp are both 5 bytes and the
			// movs are unconditional, so Relink() re-emissions and the initial
			// emission are byte-for-byte size-stable, as the relink machinery
			// requires.
			if (fast_reg_lut[block->BranchBlock >> 29] != 0
					&& fast_reg_lut[block->NextBlock >> 29] != 0)
			{
				g_cnt_emitMmuLink++;
				Xbyak::Label noBranch;

				jne(noBranch);
				{
					//branch block
					mov(dword[&sh4ctx.pc], block->BranchBlock);
					if (block->pBranchBlock)
						jmp((const void *)block->pBranchBlock->code, T_NEAR);
					else
						call(ngen_LinkBlock_cond_Branch_stub);
				}
				L(noBranch);
				{
					//no branch block
					mov(dword[&sh4ctx.pc], block->NextBlock);
					if (block->pNextBlock)
						jmp((const void *)block->pNextBlock->code, T_NEAR);
					else
						call(ngen_LinkBlock_cond_Next_stub);
				}
				nop();
				nop();
				nop();
				nop();
				nop();
				nop();
			}
			else
			{
				// Non-identity target (user space): unlinkable, full dispatch.
				g_cnt_emitMmuNoUpd++;
				mov(ecx, block->BranchBlock);
				mov(eax, block->NextBlock);
				cmovne(ecx, eax);
				jmp((const void *)no_update);
			}
		}
		else
		{
			g_cnt_emitNonMmu++;
			Xbyak::Label noBranch;

			jne(noBranch);
			{
				//branch block
				if (block->pBranchBlock)
					jmp((const void *)block->pBranchBlock->code, T_NEAR);
				else
					call(ngen_LinkBlock_cond_Branch_stub);
			}
			L(noBranch);
			{
				//no branch block
				if (block->pNextBlock)
					jmp((const void *)block->pNextBlock->code, T_NEAR);
				else
					call(ngen_LinkBlock_cond_Next_stub);
			}
			nop();
			nop();
			nop();
			nop();
			nop();
			nop();
		}
		break;


	case BET_DynamicRet:
	case BET_DynamicCall:
	case BET_DynamicJump:
		mov(ecx, dword[&sh4ctx.jdyn]);
		jmp((const void *)no_update);

		break;

	case BET_StaticCall:
	case BET_StaticJump:
		if (!mmu_enabled())
		{
			if (block->pBranchBlock)
				jmp((const void *)block->pBranchBlock->code, T_NEAR);
			else
				call(ngen_LinkBlock_Generic_stub);
			nop();
			nop();
			nop();
			nop();
			nop();
		}
		else if (fast_reg_lut[block->BranchBlock >> 29] != 0)
		{
			// MMU identity-region link -- see the BET_Cond comment above. The
			// pc store satisfies the target's entry check; odd syscall-trap
			// targets never persist a link (rdv_LinkBlock's gate), so they
			// keep resolving through the stub each time, which is what runs
			// the WinCE HLE syscalls.
			mov(dword[&sh4ctx.pc], block->BranchBlock);
			if (block->pBranchBlock)
				jmp((const void *)block->pBranchBlock->code, T_NEAR);
			else
				call(ngen_LinkBlock_Generic_stub);
			nop();
			nop();
			nop();
			nop();
			nop();
		}
		else
		{
			mov(ecx, block->BranchBlock);
			jmp((const void *)no_update);
		}
		break;

	case BET_StaticIntr:
	case BET_DynamicIntr:
		if (block->BlockType == BET_StaticIntr)
		{
			mov(dword[&sh4ctx.pc], block->NextBlock);
		}
		else
		{
			mov(eax, dword[&sh4ctx.jdyn]);
			mov(dword[&sh4ctx.pc], eax);
		}
		call(UpdateINTC);

		mov(ecx, dword[&sh4ctx.pc]);
		jmp((const void *)no_update);

		break;

	default:
		die("Invalid block end type");
	}
#endif

	ready();

	return getCurr() - startPosition;
}

u32 DynaRBI::Relink()
{
	X86Compiler *compiler = new X86Compiler(sh4ctx, codeBuffer, (u8*)code + relink_offset);
	u32 codeSize = compiler->relinkBlock(this);
	delete compiler;

	return codeSize;
}

void X86Compiler::ngen_CC_param(const shil_opcode& op, const shil_param& param, CanonicalParamType tp)
{
	switch (tp)
	{
		//push the contents
		case CPT_u32:
		case CPT_f32:
			if (param.is_reg())
			{
				if (regalloc.IsAllocg(param))
					push(regalloc.MapRegister(param));
				else
				{
					sub(esp, 4);
					movss(dword[esp], regalloc.MapXRegister(param));
				}
			}
			else if (param.is_imm())
				push(param.imm_value());
			else
				die("invalid combination");
			CC_stackSize += 4;
			unwinder.allocStackPtr(getCurr(), 4);
			break;

		//push the ptr itself
		case CPT_ptr:
			verify(param.is_reg());
			push((uintptr_t)param.reg_ptr(sh4ctx));
			CC_stackSize += 4;
			unwinder.allocStackPtr(getCurr(), 4);
			break;

		case CPT_sh4ctx:
			push((uintptr_t)&sh4ctx);
			CC_stackSize += 4;
			unwinder.allocStackPtr(getCurr(), 4);
			break;

		// store from EAX
		case CPT_u64rvL:
		case CPT_u32rv:
			mov(regalloc.MapRegister(param), eax);
			break;

		// store from EDX
		case CPT_u64rvH:
			mov(regalloc.MapRegister(param), edx);
			break;

		// store from ST(0)
		case CPT_f32rv:
			fstp(dword[param.reg_ptr(sh4ctx)]);
			movss(regalloc.MapXRegister(param), dword[param.reg_ptr(sh4ctx)]);
			break;
	}
}

void X86Compiler::ngen_CC_Finish(const shil_opcode &op)
{
	add(esp, CC_stackSize);
	unwinder.allocStackPtr(getCurr(), -CC_stackSize);
}

void X86Compiler::freezeXMM()
{
	if (current_opid == (size_t)-1)
		return;
	s8 *fpreg = alloc_fregs;
	f32 *slpc = thaw_regs;
	while (*fpreg != -1)
	{
		if (regalloc.IsMapped(Xbyak::Xmm(*fpreg), current_opid))
			movss(dword[slpc++], Xbyak::Xmm(*fpreg));
		fpreg++;
	}
}

void X86Compiler::thawXMM()
{
	if (current_opid == (size_t)-1)
		return;
	s8* fpreg = alloc_fregs;
	f32* slpc = thaw_regs;
	while (*fpreg != -1)
	{
		if (regalloc.IsMapped(Xbyak::Xmm(*fpreg), current_opid))
			movss(Xbyak::Xmm(*fpreg), dword[slpc++]);
		fpreg++;
	}
}

void X86Compiler::genMainloop()
{
	unwinder.start((void *)getCurr());
	push(esi);
	unwinder.pushReg(getSize(), Xbyak::Operand::ESI);
	push(edi);
	unwinder.pushReg(getSize(), Xbyak::Operand::EDI);
	push(ebp);
	unwinder.pushReg(getSize(), Xbyak::Operand::EBP);
	push(ebx);
	unwinder.pushReg(getSize(), Xbyak::Operand::EBX);
#ifndef _WIN32
	// 16-byte alignment
	sub(esp, 12);
	unwinder.allocStack(getSize(), 12);
#endif
	unwinder.endProlog(getSize());

	// homemade longjmp to handle mmu exceptions
	mov(dword[&jmp_esp], esp);
	Xbyak::Label longjmpLabel;
	L(longjmpLabel);

	mov(ecx, dword[&sh4ctx.pc]);

	//next_pc _MUST_ be on ecx
	Xbyak::Label cleanup;
//no_update:
	Xbyak::Label no_updateLabel;
	L(no_updateLabel);
	mov(edx, dword[&sh4ctx.CpuRunning]);
	cmp(edx, 0);
	jz(cleanup);
	if (!mmu_enabled())
	{
		mov(esi, ecx);	// save sh4 pc in ESI, used below if FPCB is still empty for this address
		mov(eax, (uintptr_t)&sh4ctx + sizeof(Sh4Context) - sizeof(Sh4RCB) + offsetof(Sh4RCB, fpcb));	// address of fpcb[0]
		and_(ecx, RAM_SIZE_MAX - 2);
		jmp(dword[eax + ecx * 2]);
	}
	else
	{
		mov(dword[&sh4ctx.pc], ecx);
		// Under full MMU, block linking is off, so EVERY block transition comes
		// through here -- and it used to always pay a C call. Inline the common
		// case: an even pc in an identity-mapped region (P1/P2/P4, fast_reg_lut
		// nonzero -- WinCE kernel+game code runs from P1) needs no translation
		// and can take the exact FPCB table jump the non-MMU dispatch uses.
		// bm_GetCodeByVAddr computes the same thing for these addresses: identity
		// paddr, then FPCA masks with RAM_SIZE_MAX-2 -- same slot, same aliasing
		// semantics, same per-block check as before. Odd pcs (WinCE HLE syscall
		// traps like GetTickCount at 0xfffffde7) and translated regions (P0/U0/
		// P3) still take the C path. An empty FPCB slot jumps to
		// ngen_FailedToFindBlock_, which under MMU reads sh4ctx.pc -- stored
		// above -- so both paths feed it correctly.
		Xbyak::Label slowDispatch;
		test(cl, 1);
		jnz(slowDispatch);
		mov(eax, ecx);
		shr(eax, 29);
		cmp(dword[(uintptr_t)fast_reg_lut + eax * 4], 0);
		je(slowDispatch);
		mov(eax, (uintptr_t)&sh4ctx + sizeof(Sh4Context) - sizeof(Sh4RCB) + offsetof(Sh4RCB, fpcb));	// address of fpcb[0]
		and_(ecx, RAM_SIZE_MAX - 2);
		jmp(dword[eax + ecx * 2]);
		L(slowDispatch);
		call((void *)bm_GetCodeByVAddr);
		jmp(eax);
	}

//cleanup:
	L(cleanup);
	mov(dword[&sh4ctx.pc], ecx);
#ifndef _WIN32
	// 16-byte alignment
	add(esp, 12);
#endif
	pop(ebx);
	pop(ebp);
	pop(edi);
	pop(esi);

	ret();

//do_iter:
	Xbyak::Label do_iter;
	L(do_iter);
	add(esp, 4);	// pop intc_sched() return address
	mov(ecx, dword[&sh4ctx.pc]);
	jmp(no_updateLabel);

//ngen_LinkBlock_Shared_stub:
	Xbyak::Label ngen_LinkBlock_Shared_stub;
	L(ngen_LinkBlock_Shared_stub);
	pop(ecx);
	sub(ecx, 5);
	call((void *)rdv_LinkBlock);
	jmp(eax);

	size_t unwindSize = unwinder.end(getSize());
	setSize(getSize() + unwindSize);

	// Functions called by blocks

//intc_sched: ecx: vaddr
	unwinder.start((void *)getCurr());
	size_t startOffset = getSize();
	unwinder.endProlog(0);
	Xbyak::Label intc_schedLabel;
	L(intc_schedLabel);
	add(dword[&sh4ctx.cycle_counter], SH4_TIMESLICE);
	mov(dword[&sh4ctx.pc], ecx);
	call((void *)UpdateSystem_INTC);
	// Re-dispatch (re-check CpuRunning) only when UpdateSystem_INTC signals it:
	// an interrupt is pending, OR the CPU was stopped (UpdateSystem_INTC returns 1
	// when !CpuRunning -- see sh4_interpreter.cpp). Otherwise ret and continue the
	// block. (An earlier unconditional jmp(do_iter) here was a wrong fix for the
	// Shinobi hang -- that was the RAM-mirror fault bug -- and only added overhead.)
	cmp(eax, 0);
	jnz(do_iter);
	ret();

//ngen_LinkBlock_cond_Next_stub:
	Xbyak::Label ngen_LinkBlock_cond_Next_label;
	L(ngen_LinkBlock_cond_Next_label);
	mov(edx, 0);
	jmp(ngen_LinkBlock_Shared_stub);

//ngen_LinkBlock_cond_Branch_stub:
	Xbyak::Label ngen_LinkBlock_cond_Branch_label;
	L(ngen_LinkBlock_cond_Branch_label);
	mov(edx, 1);
	jmp(ngen_LinkBlock_Shared_stub);

//ngen_LinkBlock_Generic_stub:
	Xbyak::Label ngen_LinkBlock_Generic_label;
	L(ngen_LinkBlock_Generic_label);
	mov(edx, dword[&sh4ctx.jdyn]);
	jmp(ngen_LinkBlock_Shared_stub);

	genMemHandlers();

	unwindSize = unwinder.end(getSize() - startOffset);
	setSize(getSize() + unwindSize);

	// The following code and all code blocks use the same stack frame as mainloop()
	// (direct jump from there or from a block)
	unwinder.start((void *)getCurr());
	startOffset = getSize();
	unwinder.pushReg(0, Xbyak::Operand::ESI);
	unwinder.pushReg(0, Xbyak::Operand::EDI);
	unwinder.pushReg(0, Xbyak::Operand::EBP);
	unwinder.pushReg(0, Xbyak::Operand::EBX);
#ifndef _WIN32
	// 16-byte alignment
	unwinder.allocStack(0, 12);
#endif
	unwinder.endProlog(0);

//ngen_FailedToFindBlock_:
	Xbyak::Label failedToFindBlock;
	L(failedToFindBlock);
	if (mmu_enabled())
		call((void *)rdv_FailedToFindBlock_pc);
	else
	{
		mov(ecx, esi);	// get back the saved sh4 PC saved above
		call((void *)rdv_FailedToFindBlock);
	}
	jmp(eax);

//ngen_blockcheckfail:
	Xbyak::Label ngen_blockcheckfailLabel;
	L(ngen_blockcheckfailLabel);
	call((void *)rdv_BlockCheckFail);
	if (mmu_enabled())
	{
		Xbyak::Label jumpblockLabel;
		cmp(eax, 0);
		jne(jumpblockLabel);
		mov(ecx, dword[&sh4ctx.pc]);
		jmp(no_updateLabel);
		L(jumpblockLabel);
	}
	jmp(eax);

//handleException:
	Xbyak::Label handleExceptionLabel;
	L(handleExceptionLabel);
	mov(esp, dword[&jmp_esp]);
	jmp(longjmpLabel);

	unwindSize = unwinder.end(getSize() - startOffset);
	setSize(getSize() + unwindSize);

	ready();

	::mainloop = (void (*)())getCode();
	ngen_FailedToFindBlock_ = (void (*)())failedToFindBlock.getAddress();
	intc_sched = (void (*)())intc_schedLabel.getAddress();
	no_update = (void (*)())no_updateLabel.getAddress();
	ngen_LinkBlock_cond_Next_stub = (void (*)())ngen_LinkBlock_cond_Next_label.getAddress();
	ngen_LinkBlock_cond_Branch_stub = (void (*)())ngen_LinkBlock_cond_Branch_label.getAddress();
	ngen_LinkBlock_Generic_stub = (void (*)())ngen_LinkBlock_Generic_label.getAddress();
	ngen_blockcheckfail = (void (*)())ngen_blockcheckfailLabel.getAddress();
	X86Compiler::handleException = (void (*)())handleExceptionLabel.getAddress();

	codeBuffer.advance(getSize());
}

bool X86Compiler::genReadMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
{
	if (!op.rs1.is_imm())
		return false;
	void *ptr;
	bool isram;
	u32 addr;
	if (!rdv_readMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
		return false;

	if (isram)
	{
		// Immediate pointer to RAM: super-duper fast access
		switch (op.size)
		{
		case 1:
			if (regalloc.IsAllocg(op.rd))
				movsx(regalloc.MapRegister(op.rd), byte[ptr]);
			else
			{
				movsx(eax, byte[ptr]);
				mov(dword[op.rd.reg_ptr(sh4ctx)], eax);
			}
			break;

		case 2:
			if (regalloc.IsAllocg(op.rd))
				movsx(regalloc.MapRegister(op.rd), word[ptr]);
			else
			{
				movsx(eax, word[ptr]);
				mov(dword[op.rd.reg_ptr(sh4ctx)], eax);
			}
			break;

		case 4:
			if (regalloc.IsAllocg(op.rd))
				mov(regalloc.MapRegister(op.rd), dword[ptr]);
			else if (regalloc.IsAllocf(op.rd))
				movss(regalloc.MapXRegister(op.rd), dword[ptr]);	// SSE1 (movd xmm,m32 is SSE2)
			else
			{
				mov(eax, dword[ptr]);
				mov(dword[op.rd.reg_ptr(sh4ctx)], eax);
			}
			break;

		case 8:
			if (op.rd.count() == 2 && regalloc.IsAllocf(op.rd))
			{
				movss(regalloc.MapXRegister(op.rd, 0), dword[ptr]);
				movss(regalloc.MapXRegister(op.rd, 1), dword[(u32 *)ptr + 1]);
			}
			else
			{
				movlps(xmm0, qword[ptr]);	// SSE1 64-bit move (movq xmm is SSE2)
				movlps(qword[op.rd.reg_ptr(sh4ctx)], xmm0);
			}
			break;

		default:
			die("Invalid immediate size");
				break;
		}
	}
	else
	{
		// Not RAM: the returned pointer is a memory handler
		if (op.size == 8)
		{
			verify(!regalloc.IsAllocAny(op.rd));

			// Need to call the handler twice
			mov(ecx, addr);
			genCall((void (DYNACALL *)())ptr);
			mov(dword[op.rd.reg_ptr(sh4ctx)], eax);

			mov(ecx, addr + 4);
			genCall((void (DYNACALL *)())ptr);
			mov(dword[op.rd.reg_ptr(sh4ctx) + 1], eax);
		}
		else
		{
			mov(ecx, addr);

			switch(op.size)
			{
			case 1:
				genCall((void (DYNACALL *)())ptr);
				movsx(eax, al);
				break;

			case 2:
				genCall((void (DYNACALL *)())ptr);
				movsx(eax, ax);
				break;

			case 4:
				genCall((void (DYNACALL *)())ptr);
				break;

			default:
				die("Invalid immediate size");
					break;
			}
			host_reg_to_shil_param(op.rd, eax);
		}
	}

	return true;
}

bool X86Compiler::genWriteMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
{
	if (!op.rs1.is_imm())
		return false;
	void *ptr;
	bool isram;
	u32 addr;
	if (!rdv_writeMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
		return false;

	if (isram)
	{
		// Immediate pointer to RAM: super-duper fast access
		switch (op.size)
		{
		case 1:
			if (regalloc.IsAllocg(op.rs2))
			{
				Xbyak::Reg32 rs2 = regalloc.MapRegister(op.rs2);
				if (rs2.getIdx() >= 4)
				{
					mov(eax, rs2);
					mov(byte[ptr], al);
				}
				else
					mov(byte[ptr], rs2.cvt8());
			}
			else if (op.rs2.is_imm())
				mov(byte[ptr], (u8)op.rs2.imm_value());
			else
			{
				mov(al, byte[op.rs2.reg_ptr(sh4ctx)]);
				mov(byte[ptr], al);
			}
			break;

		case 2:
			if (regalloc.IsAllocg(op.rs2))
				mov(word[ptr], regalloc.MapRegister(op.rs2).cvt16());
			else if (op.rs2.is_imm())
				mov(word[ptr], (u16)op.rs2.imm_value());
			else
			{
				mov(cx, word[op.rs2.reg_ptr(sh4ctx)]);
				mov(word[ptr], cx);
			}
			break;

		case 4:
			if (regalloc.IsAllocg(op.rs2))
				mov(dword[ptr], regalloc.MapRegister(op.rs2));
			else if (regalloc.IsAllocf(op.rs2))
				movss(dword[ptr], regalloc.MapXRegister(op.rs2));	// SSE1 (movd m32,xmm is SSE2)
			else if (op.rs2.is_imm())
				mov(dword[ptr], op.rs2.imm_value());
			else
			{
				mov(ecx, dword[op.rs2.reg_ptr(sh4ctx)]);
				mov(dword[ptr], ecx);
			}
			break;

		case 8:
			if (op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2))
			{
				movss(dword[ptr], regalloc.MapXRegister(op.rs2, 0));
				movss(dword[(u32 *)ptr + 1], regalloc.MapXRegister(op.rs2, 1));
			}
			else
			{
				movlps(xmm0, qword[op.rs2.reg_ptr(sh4ctx)]);	// SSE1 64-bit move
				movlps(qword[ptr], xmm0);
			}
			break;

		default:
			die("Invalid immediate size");
			break;
		}
	}
	else
	{
		// Not RAM: the returned pointer is a memory handler
		mov(ecx, addr);
		shil_param_to_host_reg(op.rs2, edx);

		genCall((void (DYNACALL *)())ptr);
	}

	return true;
}

void X86Compiler::checkBlock(bool smc_checks, RuntimeBlockInfo* block)
{
	if (mmu_enabled() || smc_checks)
		mov(ecx, block->addr);

	if (mmu_enabled())
	{
		mov(eax, dword[&sh4ctx.pc]);
		cmp(eax, block->vaddr);
		jne(reinterpret_cast<const void*>(ngen_blockcheckfail));
	}

	if (!smc_checks)
		return;

	s32 sz = block->sh4_code_size;
	u32 sa = block->addr;
	while (sz > 0)
	{
		void* p = GetMemPtr(sa, 4);
		if (p)
		{
			if (sz == 2)
				cmp(word[p], (u32)*(s16*)p);
			else
				cmp(dword[p], *(u32*)p);
			jne((const void *)ngen_blockcheckfail);
		}
		sz -= 4;
		sa += 4;
	}
}

class X86Dynarec : public Sh4Dynarec
{
public:
	X86Dynarec() {
		sh4Dynarec = this;
	}

	void init(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) override
	{
		this->sh4ctx = &sh4ctx;
		this->codeBuffer = &codeBuffer;
	}

	void handleException(host_context_t &context) override
	{
		context.pc = (uintptr_t)X86Compiler::handleException;
	}

	void generate_mainloop()
	{
		if (::mainloop != nullptr)
			return;

		compiler = new X86Compiler(*sh4ctx, *codeBuffer);

		try {
			compiler->genMainloop();
		} catch (const Xbyak::Error& e) {
			ERROR_LOG(DYNAREC, "Fatal xbyak error: %s", e.what());
		}

		delete compiler;
		compiler = nullptr;

		rdv_SetFailedToFindBlockHandler(ngen_FailedToFindBlock_);
	}

	void reset() override
	{
		::mainloop = nullptr;
		unwinder.clear();

		if (sh4ctx->CpuRunning)
		{
			// Force the dynarec out of mainloop() to regenerate it
			sh4ctx->CpuRunning = 0;
			restarting = true;
		}
		else
			generate_mainloop();
	}

	RuntimeBlockInfo* allocateBlock() override
	{
		generate_mainloop();
		return new DynaRBI(*sh4ctx, *codeBuffer);
	}

	void mainloop(void* v_cntx) override
	{
		try {
			do {
				restarting = false;
				generate_mainloop();

				::mainloop();
				if (restarting && !emu.restartCpu())
					restarting = false;
			} while (restarting);
		} catch (const SH4ThrownException& e) {
			ERROR_LOG(DYNAREC, "SH4ThrownException in mainloop %x pc %x", e.expEvn, e.epc);
			throw FlycastException("Fatal: Unhandled SH4 exception");
		}
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		compiler = new X86Compiler(*sh4ctx, *codeBuffer);

		try {
			compiler->compile(block, smc_checks, optimise);
		} catch (const Xbyak::Error& e) {
			ERROR_LOG(DYNAREC, "Fatal xbyak error: %s", e.what());
		}

		delete compiler;
	}

	bool rewrite(host_context_t &context, void *faultAddress) override
	{
		if (codeBuffer == nullptr)
			// init() not called yet
			return false;
		// Inline fastmem fault: the faulting pc is inside the SH4 code cache but
		// OUTSIDE the generated memory handlers -- the access was emitted inline
		// into the block (genInlineFastMem). Every inline variant has an 8-byte
		// mov/and prefix, so the sequence starts at pc-8; rewriteInlineMemAccess
		// validates the bytes before patching anything.
		if (g_sh4CacheStart != nullptr
				&& context.pc >= (uintptr_t)g_sh4CacheStart
				&& context.pc <  (uintptr_t)g_sh4CacheStart + g_sh4CacheSize
				&& !(context.pc >= (uintptr_t)g_memHandlerStart
						&& context.pc < (uintptr_t)g_memHandlerEnd))
		{
			u8 *rewriteAddr = (u8 *)context.pc - 8;
			X86Compiler *compiler = new X86Compiler(*sh4ctx, *codeBuffer, rewriteAddr);
			bool rv = compiler->rewriteInlineMemAccess(context);
			delete compiler;

			return rv;
		}
		u8 *rewriteAddr = *(u8 **)context.esp - 5;
		X86Compiler *compiler = new X86Compiler(*sh4ctx, *codeBuffer, rewriteAddr);
		bool rv = compiler->rewriteMemAccess(context);
		delete compiler;

		return rv;
	}

	void canonStart(const shil_opcode *op) override
	{
		compiler->ngen_CC_Start(*op);
	}

	void canonParam(const shil_opcode *op, const shil_param *par, CanonicalParamType tp) override
	{
		compiler->ngen_CC_param(*op, *par, tp);
	}

	void canonCall(const shil_opcode *op, void *function) override
	{
		compiler->ngen_CC_Call(*op, function);
	}

	void canonFinish(const shil_opcode *op) override
	{
		compiler->ngen_CC_Finish(*op);
	}

private:
	Sh4Context *sh4ctx = nullptr;
	Sh4CodeBuffer *codeBuffer = nullptr;
	X86Compiler *compiler = nullptr;
	bool restarting = false;
};

static X86Dynarec instance;

#endif
