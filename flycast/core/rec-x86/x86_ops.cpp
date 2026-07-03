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

//#define CANONICAL_TEST

#include "rec_x86.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/mem/addrspace.h"
#include "oslib/unwind_info.h"

extern UnwindInfo unwinder;

namespace MemSize {
enum {
	S8,
	S16,
	S32,
	F32,
	F64,
	Count
};
}

namespace MemOp {
enum {
	R,
	W,
	Count
};
}

namespace MemType {
enum {
	Fast,
	StoreQueue,
	Slow,
	Count
};
}

static const void *MemHandlers[MemType::Count][MemSize::Count][MemOp::Count];
static const u8 *MemHandlerStart, *MemHandlerEnd;

// Sampling-profiler subrange (defined in the Xbox port's main_xbox.cpp).
extern "C" const u8 *g_memHandlerStart, *g_memHandlerEnd;

// Execution-shape counters (defined in main_xbox.cpp), bumped by EMITTED code:
// the xbdm EIP sampler proved blind (it reads the kernel's APC-delivery point,
// not the interrupted user EIP), so measure the JIT's execution profile with
// inline increments instead. One `inc dword` per event. Plain unsigned (not
// volatile): cross-thread read races are fine for diagnostics, and Xbyak
// address expressions want a non-volatile pointer. kJitCounters gates EMISSION
// (folded at codegen time): false = zero runtime cost for release builds.
static const bool kJitCounters = false;
extern "C" unsigned g_cnt_memh;
extern "C" unsigned g_cnt_ifb;

void X86Compiler::genMemHandlers()
{
	MemHandlerStart = getCurr();
	for (int type = 0; type < MemType::Count; type++)
	{
		for (int size = 0; size < MemSize::Count; size++)
		{
			for (int op = 0; op < MemOp::Count; op++)
			{
				MemHandlers[type][size][op] = getCurr();
				if (kJitCounters)
					inc(dword[&g_cnt_memh]);	// execution-shape counter

				if (type == MemType::Fast && addrspace::virtmemEnabled())
				{
					// save the original address in eax so it can be restored during rewriting
					mov(eax, ecx);
					and_(ecx, 0x1FFFFFFF);
					Xbyak::Address address = dword[ecx];
					Xbyak::Reg reg;
					switch (size)
					{
					case MemSize::S8:
						address = byte[ecx + (size_t)addrspace::ram_base];
						reg = op == MemOp::R ? (Xbyak::Reg)eax : (Xbyak::Reg)dl;
						break;
					case MemSize::S16:
						address = word[ecx + (size_t)addrspace::ram_base];
						reg = op == MemOp::R ? (Xbyak::Reg)eax : (Xbyak::Reg)dx;
						break;
					case MemSize::S32:
						address = dword[ecx + (size_t)addrspace::ram_base];
						reg = op == MemOp::R ? eax : edx;
						break;
					default:
						address = dword[ecx + (size_t)addrspace::ram_base];
						break;
					}
					if (size >= MemSize::F32)
					{
						if (op == MemOp::R)
							movss(xmm0, address);
						else
							movss(address, xmm0);
						if (size == MemSize::F64)
						{
							address = dword[ecx + (size_t)addrspace::ram_base + 4];
							if (op == MemOp::R)
								movss(xmm1, address);
							else
								movss(address, xmm1);
						}
					}
					else
					{
						if (op == MemOp::R)
						{
							if (size <= MemSize::S16)
								movsx(reg, address);
							else
								mov(reg, address);
						}
						else
							mov(address, reg);
					}
				}
				else if (type == MemType::StoreQueue)
				{
					if (op != MemOp::W || size < MemSize::S32)
						continue;
					Xbyak::Label no_sqw;

					mov(eax, ecx);
					shr(eax, 28);
					cmp(eax, 0xE);
					jne(no_sqw);
					and_(ecx, 0x3F);

					if (size == MemSize::S32)
						mov(dword[(size_t)sh4ctx.sq_buffer + ecx], edx);
					else if (size >= MemSize::F32)
					{
						movss(dword[(size_t)sh4ctx.sq_buffer + ecx], xmm0);
						if (size == MemSize::F64)
							movss(dword[((size_t)sh4ctx.sq_buffer + 4) + ecx], xmm1);
					}
					ret();
					L(no_sqw);
					// TODO Fall through SQ -> slow path to avoid code dup
					if (size == MemSize::F64)
					{
#ifndef _WIN32
						// 16-byte alignment
						alignStack(-12);
#else
						sub(esp, 8);
						unwinder.allocStackPtr(getCurr(), 8);
#endif
						movss(dword[esp], xmm0);
						movss(dword[esp + 4], xmm1);
						call((const void *)addrspace::write64);	// dynacall adds 8 to esp
						alignStack(4);
					}
					else
					{
						if (size == MemSize::F32)
							sse1movd(edx, xmm0);	// SSE1-safe (movd r32,xmm is SSE2)
						jmp((const void *)addrspace::write32);	// tail call
						continue;
					}
				}
				else
				{
					// Slow path
					if (op == MemOp::R)
					{
						switch (size) {
						case MemSize::S8:
							// 16-byte alignment
							alignStack(-12);
							call((const void *)addrspace::read8);
							movsx(eax, al);
							alignStack(12);
							break;
						case MemSize::S16:
							// 16-byte alignment
							alignStack(-12);
							call((const void *)addrspace::read16);
							movsx(eax, ax);
							alignStack(12);
							break;
						case MemSize::S32:
							jmp((const void *)addrspace::read32);	// tail call
							continue;
						case MemSize::F32:
							// 16-byte alignment
							alignStack(-12);
							call((const void *)addrspace::read32);
							sse1movd(xmm0, eax);	// SSE1-safe (movd xmm,r32 is SSE2)
							alignStack(12);
							break;
						case MemSize::F64:
							// 16-byte alignment
							alignStack(-12);
							call((const void *)addrspace::read64);
							sse1movd(xmm0, eax);
							sse1movd(xmm1, edx);
							alignStack(12);
							break;
						default:
							die("1..8 bytes");
						}
					}
					else
					{
						switch (size) {
						case MemSize::S8:
							jmp((const void *)addrspace::write8);	// tail call
							continue;
						case MemSize::S16:
							jmp((const void *)addrspace::write16);	// tail call
							continue;
						case MemSize::S32:
							jmp((const void *)addrspace::write32);	// tail call
							continue;
						case MemSize::F32:
							sse1movd(edx, xmm0);	// SSE1-safe (movd r32,xmm is SSE2)
							jmp((const void *)addrspace::write32);	// tail call
							continue;
						case MemSize::F64:
#ifndef _WIN32
							// 16-byte alignment
							alignStack(-12);
#else
							sub(esp, 8);
							unwinder.allocStackPtr(getCurr(), 8);
#endif
							movss(dword[esp], xmm0);
							movss(dword[esp + 4], xmm1);
							call((const void *)addrspace::write64);	// dynacall adds 8 to esp
							alignStack(4);
							break;
						default:
							die("1..8 bytes");
						}
					}
				}
				ret();
			}
		}
	}
	MemHandlerEnd = getCurr();

	// Sampling-profiler subrange (defined in main_xbox.cpp): distinguishes time
	// in the generated memory-access handlers from time in compiled block bodies.
	g_memHandlerStart = MemHandlerStart;
	g_memHandlerEnd = MemHandlerEnd;
}

// ---- Inline fastmem ---------------------------------------------------------
// Counters showed ~600-700k memory accesses/frame in heavy scenes, EACH paying a
// call+ret round trip into a generated handler whose body is 3 instructions.
// Emit the fast-path RAM access directly into the block instead:
//     mov  eax, ecx                 ; original addr survives in eax at fault time
//     and  ecx, 0x1FFFFFFF
//     <access> [ecx + ram_base]     ; result/data in eax/edx/xmm0, same ABI as
//                                   ; the Fast handlers
// An access that faults (MMIO / unmapped / store queue) is patched IN PLACE by
// rewriteInlineMemAccess below: the whole sequence is overwritten with a 5-byte
// call to the Slow (or StoreQueue) handler plus NOP padding -- the exact analogue
// of the existing call-site rewrite, applied to an inline sequence. The two
// functions must stay byte-for-byte in sync: the rewriter identifies the access
// by its opcode bytes and validates disp32 == ram_base.
// F64 and non-optimised blocks keep the call path.
bool X86Compiler::genInlineFastMem(int memOpSize, u32 memOp)
{
	if (!addrspace::virtmemEnabled() || memOpSize > MemSize::F32)
		return false;

	const size_t base = (size_t)addrspace::ram_base;
	mov(eax, ecx);				// 2 bytes -- with the and below, always 8 bytes
	and_(ecx, 0x1FFFFFFF);		// 6 bytes    before the access instruction
	if (memOp == MemOp::R)
	{
		switch (memOpSize)
		{
		case MemSize::S8:  movsx(eax, byte[ecx + base]);  break;	// 0F BE 81 disp32
		case MemSize::S16: movsx(eax, word[ecx + base]);  break;	// 0F BF 81 disp32
		case MemSize::S32: mov(eax, dword[ecx + base]);   break;	// 8B 81 disp32
		default:           movss(xmm0, dword[ecx + base]); break;	// F3 0F 10 81 disp32
		}
	}
	else
	{
		switch (memOpSize)
		{
		case MemSize::S8:  mov(byte[ecx + base], dl);     break;	// 88 91 disp32
		case MemSize::S16: mov(word[ecx + base], dx);     break;	// 66 89 91 disp32
		case MemSize::S32: mov(dword[ecx + base], edx);   break;	// 89 91 disp32
		default:           movss(dword[ecx + base], xmm0); break;	// F3 0F 11 81 disp32
		}
	}
	return true;
}

// Fault-time patcher for the inline sequences above. The compiler is positioned
// at context.pc - 8 (the sequence start: the mov/and prefix is 8 bytes in every
// variant). Decodes the faulting access instruction, validates its disp32 is
// really ram_base (so a stray fault in block code can never be mispatched), and
// overwrites the sequence with the same call the old call-site rewriter would
// emit, NOP-padded to the original length.
bool X86Compiler::rewriteInlineMemAccess(host_context_t &context)
{
	const u8 *p = (const u8 *)context.pc;
	int size, memOp;
	u32 accessLen, dispOff;

	if (p[0] == 0x8B && p[1] == 0x81)
	{	size = MemSize::S32; memOp = MemOp::R; accessLen = 6; dispOff = 2; }
	else if (p[0] == 0x0F && p[1] == 0xBF && p[2] == 0x81)
	{	size = MemSize::S16; memOp = MemOp::R; accessLen = 7; dispOff = 3; }
	else if (p[0] == 0x0F && p[1] == 0xBE && p[2] == 0x81)
	{	size = MemSize::S8;  memOp = MemOp::R; accessLen = 7; dispOff = 3; }
	else if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x10 && p[3] == 0x81)
	{	size = MemSize::F32; memOp = MemOp::R; accessLen = 8; dispOff = 4; }
	else if (p[0] == 0x88 && p[1] == 0x91)
	{	size = MemSize::S8;  memOp = MemOp::W; accessLen = 6; dispOff = 2; }
	else if (p[0] == 0x66 && p[1] == 0x89 && p[2] == 0x91)
	{	size = MemSize::S16; memOp = MemOp::W; accessLen = 7; dispOff = 3; }
	else if (p[0] == 0x89 && p[1] == 0x91)
	{	size = MemSize::S32; memOp = MemOp::W; accessLen = 6; dispOff = 2; }
	else if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x11 && p[3] == 0x81)
	{	size = MemSize::F32; memOp = MemOp::W; accessLen = 8; dispOff = 4; }
	else
		return false;

	if (*(const u32 *)(p + dispOff) != (u32)(uintptr_t)addrspace::ram_base)
		return false;

	// Patch: same target selection as rewriteMemAccess -- a write with the
	// original address (still in eax) in the 0xE0000000 region is a store-queue
	// write, everything else goes to the Slow handler.
	const u8 *cs = getCurr();
	if (memOp == MemOp::W && size >= MemSize::S32 && (context.eax >> 28) == 0xE)
		call(MemHandlers[MemType::StoreQueue][size][MemOp::W]);
	else
		call(MemHandlers[MemType::Slow][size][memOp]);
	verify(getCurr() - cs == 5);
	const u32 seqLen = 8 + accessLen;
	while ((u32)(getCurr() - cs) < seqLen)
		nop();
	ready();

	// Re-execute from the patched call. The handler expects the UNMASKED address
	// in ecx; it survived in eax (the access instruction faulted before its
	// destination write, and stores don't touch eax at all).
	context.pc = (uintptr_t)(p - 8);
	context.ecx = context.eax;
	return true;
}

void X86Compiler::genMmuLookup(RuntimeBlockInfo* block, const shil_opcode& op, u32 write)
{
	if (mmu_enabled())
	{
#ifdef FAST_MMU
		Xbyak::Label inCache;
		Xbyak::Label done;

		mov(eax, ecx);
		shr(eax, 12);
		mov(eax, dword[(uintptr_t)mmuAddressLUT + eax * 4]);
		test(eax, eax);
		jne(inCache);
#endif
		// Deferred exception spill (pairs with the relaxed MMU flush in
		// ssa_regalloc.h): this slow path is the ONLY place a memory access can
		// raise an SH4 exception (a LUT hit cannot throw), so the dirty, live
		// guest registers are written back to the context HERE instead of
		// flushing around every memory op. On the ~100% LUT-hit path this code
		// never runs. mmuDynarecLookup preserves ebx/ebp/esi/edi (callee-saved)
		// and the XMM values were saved by freezeXMM at the call site, so on a
		// successful (non-throwing) return the host registers stay
		// authoritative and the allocator state is untouched -- these stores
		// are pure shadow copies for the exception handler.
		regalloc.ForEachSpillableReg([this](u32 reg, u32 hostReg, bool isFloat) {
			if (isFloat)
				movss(dword[GetRegPtr(sh4ctx, reg)], Xbyak::Xmm((int)hostReg));
			else
				mov(dword[GetRegPtr(sh4ctx, reg)], Xbyak::Reg32((int)hostReg));
		});
		mov(edx, write);
		push(block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));	// pc
		call((void*)mmuDynarecLookup);
		mov(ecx, eax);
#ifdef FAST_MMU
		jmp(done);
		L(inCache);
		and_(ecx, 0xFFF);
		or_(ecx, eax);
		L(done);
#endif
	}
}

[[noreturn]]
static void DYNACALL handle_sh4_exception(Sh4Context *ctx, SH4ThrownException& ex, u32 pc)
{
	if (pc & 1)
	{
		// Delay slot
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex.expEvn);
	ctx->cycle_counter += 4;	// probably more is needed
	X86Compiler::handleException();
	// not reached
	std::abort();
}

static void DYNACALL interpreter_fallback(Sh4Context *ctx, u16 op, OpCallFP *oph, u32 pc)
{
	try {
		oph(ctx, op);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ctx, ex, pc);
	}
}

static void DYNACALL do_sqw_mmu_no_ex(u32 addr, Sh4Context *ctx, u32 pc)
{
	try {
		ctx->doSqWrite(addr, ctx);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ctx, ex, pc);
	}
}

void X86Compiler::genOpcode(RuntimeBlockInfo* block, bool optimise, shil_opcode& op)
{
	switch (op.op)
	{
	case shop_ifb:
		if (kJitCounters)
			inc(dword[&g_cnt_ifb]);	// execution-shape counter: interpreter fallbacks
		if (mmu_enabled())
		{
			push(reinterpret_cast<uintptr_t>(*OpDesc[op.rs3._imm]->oph));	// op handler
			push(block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
		}
		if (op.rs1.is_imm() && op.rs1.imm_value())
			mov(dword[&sh4ctx.pc], op.rs2.imm_value());
        mov(ecx, (uintptr_t)&sh4ctx);
		mov(edx, op.rs3.imm_value());
		if (!mmu_enabled())
			genCall(OpDesc[op.rs3.imm_value()]->oph);
		else
			genCall(interpreter_fallback);

		break;

	case shop_mov64:
		verify(op.rd.is_r64f());
		verify(op.rs1.is_r64f());

#if ALLOC_F64 == true
		movss(regalloc.MapXRegister(op.rd, 0), regalloc.MapXRegister(op.rs1, 0));
		movss(regalloc.MapXRegister(op.rd, 1), regalloc.MapXRegister(op.rs1, 1));
#else
		verify(!regalloc.IsAllocAny(op.rd));
		movlps(xmm0, qword[op.rs1.reg_ptr(sh4ctx)]);	// SSE1 64-bit move (movq xmm is SSE2)
		movlps(qword[op.rd.reg_ptr(sh4ctx)], xmm0);
#endif
		break;

	case shop_readm:
		if (!genReadMemImmediate(op, block))
		{
			// Not an immediate address
			shil_param_to_host_reg(op.rs1, ecx);
			if (!op.rs3.is_null())
			{
				if (op.rs3.is_imm())
					add(ecx, op.rs3._imm);
				else if (regalloc.IsAllocg(op.rs3))
					add(ecx, regalloc.MapRegister(op.rs3));
				else
					add(ecx, dword[op.rs3.reg_ptr(sh4ctx)]);
			}

			int memOpSize;
			switch (op.size)
			{
			case 1:
				memOpSize = MemSize::S8;
				break;
			case 2:
				memOpSize = MemSize::S16;
				break;
			case 4:
				memOpSize = regalloc.IsAllocf(op.rd) ? MemSize::F32 : MemSize::S32;
				break;
			case 8:
				memOpSize = MemSize::F64;
				break;
			}

			if (mmu_enabled())
				freezeXMM();
			genMmuLookup(block, op, 0);
			if (!(optimise && genInlineFastMem(memOpSize, MemOp::R)))
			{
				const u8 *start = getCurr();
				call(MemHandlers[optimise ? MemType::Fast : MemType::Slow][memOpSize][MemOp::R]);
				verify(getCurr() - start == 5);
			}
			if (mmu_enabled())
				thawXMM();

			if (memOpSize <= MemSize::S32)
			{
				host_reg_to_shil_param(op.rd, eax);
			}
			else if (memOpSize == MemSize::F32)
			{
				host_reg_to_shil_param(op.rd, xmm0);
			}
			else
			{
				if (op.rd.count() == 2 && regalloc.IsAllocf(op.rd))
				{
					movss(regalloc.MapXRegister(op.rd, 0), xmm0);	// SSE1 FP-reg copy
					movss(regalloc.MapXRegister(op.rd, 1), xmm1);
				}
				else
				{
					verify(!regalloc.IsAllocAny(op.rd));
					movss(dword[op.rd.reg_ptr(sh4ctx)], xmm0);
					movss(dword[op.rd.reg_ptr(sh4ctx) + 1], xmm1);
				}
			}
		}
		break;

	case shop_writem:
		if (!genWriteMemImmediate(op, block))
		{
			shil_param_to_host_reg(op.rs1, ecx);
			if (!op.rs3.is_null())
			{
				if (op.rs3.is_imm())
					add(ecx, op.rs3._imm);
				else if (regalloc.IsAllocg(op.rs3))
					add(ecx, regalloc.MapRegister(op.rs3));
				else
					add(ecx, dword[op.rs3.reg_ptr(sh4ctx)]);
			}

			int memOpSize;
			switch (op.size)
			{
			case 1:
				memOpSize = MemSize::S8;
				break;
			case 2:
				memOpSize = MemSize::S16;
				break;
			case 4:
				memOpSize = regalloc.IsAllocf(op.rs2) ? MemSize::F32 : MemSize::S32;
				break;
			case 8:
				memOpSize = MemSize::F64;
				break;
			}

			if (mmu_enabled())
				freezeXMM();
			genMmuLookup(block, op, 1);

			if (memOpSize <= MemSize::S32)
				shil_param_to_host_reg(op.rs2, edx);
			else if (memOpSize == MemSize::F32)
				shil_param_to_host_reg(op.rs2, xmm0);
			else {
				if (op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2))
				{
					movss(xmm0, regalloc.MapXRegister(op.rs2, 0));	// SSE1 FP-reg copy
					movss(xmm1, regalloc.MapXRegister(op.rs2, 1));
				}
				else
				{
					movss(xmm0, dword[op.rs2.reg_ptr(sh4ctx)]);	// SSE1 (movd xmm,m32 is SSE2)
					movss(xmm1, dword[op.rs2.reg_ptr(sh4ctx) + 1]);
				}
			}
			if (!(optimise && genInlineFastMem(memOpSize, MemOp::W)))
			{
				const u8 *start = getCurr();
				call(MemHandlers[optimise ? MemType::Fast : MemType::Slow][memOpSize][MemOp::W]);
				verify(getCurr() - start == 5);
			}
			if (mmu_enabled())
				thawXMM();
		}
		break;

	case shop_jcond:
	case shop_jdyn:
	case shop_mov32:
		genBaseOpcode(op);
		break;

#ifndef CANONICAL_TEST
	case shop_sync_sr:
		genCallCdecl(UpdateSR);
		break;
	case shop_sync_fpscr:
		mov(ecx, (uintptr_t)&sh4ctx);
		genCall(Sh4Context::UpdateFPSCR);
		break;

	case shop_pref:
		{
			Xbyak::Label no_sqw;

			if (op.rs1.is_imm())
			{
				// this test shouldn't be necessary
				if ((op.rs1.imm_value() & 0xF0000000) != 0xE0000000)
					break;
				mov(ecx, op.rs1.imm_value());
			}
			else
			{
				Xbyak::Reg32 rn;
				if (regalloc.IsAllocg(op.rs1))
				{
					rn = regalloc.MapRegister(op.rs1);
				}
				else
				{
					mov(eax, dword[op.rs1.reg_ptr(sh4ctx)]);
					rn = eax;
				}
				mov(ecx, rn);
				shr(ecx, 28);
				cmp(ecx, 0xE);
				jne(no_sqw);

				mov(ecx, rn);
			}
			mov(edx, (uintptr_t)&sh4ctx);
			if (mmu_enabled())
			{
				push(block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
				genCall(do_sqw_mmu_no_ex);
			}
			else
			{
				freezeXMM();
				call(dword[&sh4ctx.doSqWrite]);
				thawXMM();
			}
			L(no_sqw);
		}
		break;

	case shop_mul_s64:
		mov(eax, regalloc.MapRegister(op.rs1));
		if (op.rs2.is_reg())
			mov(edx, regalloc.MapRegister(op.rs2));
		else
			mov(edx, op.rs2._imm);
		imul(edx);
		mov(regalloc.MapRegister(op.rd), eax);
		mov(regalloc.MapRegister(op.rd2), edx);
		break;

	case shop_frswap:
		mov(eax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
		mov(ecx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
		for (int i = 0; i < 4; i++)
		{
			movaps(xmm0, xword[eax + (i * 16)]);
			movaps(xmm1, xword[ecx + (i * 16)]);
			movaps(xword[eax + (i * 16)], xmm1);
			movaps(xword[ecx + (i * 16)], xmm0);
		}
		break;

	// SSE1 vector FPU ops. Without these every FIPR/FTRV becomes a marshalled
	// C call (shil_chf canonical path) -- 3D games execute tens of thousands
	// per frame for geometry transform and lighting. Vector operands (count>2)
	// are never register-allocated: the allocator write-back-flushes sources
	// and hard-flushes destinations at OpBegin, so ctx memory is coherent and
	// 16-byte aligned (same guarantees shop_frswap above relies on). xmm0-3
	// are scratch (the allocator only uses xmm4-7). Single-precision result
	// (canonical sums in double) -- same tradeoff the ARM backends make.
	case shop_fipr:
		// rd = dot4(rs1, rs2)
		mov(eax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
		movaps(xmm0, xword[eax]);
		if (op.rs2._reg == op.rs1._reg)
			mulps(xmm0, xmm0);
		else
		{
			mov(ecx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
			mulps(xmm0, xword[ecx]);
		}
		// horizontal add without SSE3: [x y z w] -> x+z, y+w -> sum
		movhlps(xmm1, xmm0);
		addps(xmm0, xmm1);			// lane0 = x+z, lane1 = y+w
		movaps(xmm1, xmm0);
		shufps(xmm1, xmm1, 1);		// lane0 = y+w
		addss(xmm0, xmm1);
		host_reg_to_shil_param(op.rd, xmm0);
		break;

	case shop_ftrv:
		// rd[j] = sum_i rs1[i] * rs2[j + 4*i]  (XMTRX)
		// = rs1[0]*M[0..3] + rs1[1]*M[4..7] + rs1[2]*M[8..11] + rs1[3]*M[12..15]
		mov(eax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
		mov(ecx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
		movaps(xmm3, xword[eax]);	// load the full vector first: rd may alias rs1
		movaps(xmm0, xmm3);
		shufps(xmm0, xmm0, 0x00);	// broadcast v0
		mulps(xmm0, xword[ecx]);
		movaps(xmm1, xmm3);
		shufps(xmm1, xmm1, 0x55);	// v1
		mulps(xmm1, xword[ecx + 16]);
		addps(xmm0, xmm1);
		movaps(xmm2, xmm3);
		shufps(xmm2, xmm2, 0xAA);	// v2
		mulps(xmm2, xword[ecx + 32]);
		addps(xmm0, xmm2);
		shufps(xmm3, xmm3, 0xFF);	// v3
		mulps(xmm3, xword[ecx + 48]);
		addps(xmm0, xmm3);
		mov(eax, (uintptr_t)op.rd.reg_ptr(sh4ctx));
		movaps(xword[eax], xmm0);
		break;
#endif

	default:
#ifndef CANONICAL_TEST
		if (!genBaseOpcode(op))
#endif
			shil_chf[op.op](&op);
		break;
	}
}

bool X86Compiler::rewriteMemAccess(host_context_t &context)
{
	u8 *retAddr = *(u8 **)context.esp;
	//DEBUG_LOG(DYNAREC, "rewriteMemAccess hpc %08x retadr %08x", context.pc, (size_t)retAddr);
	if (context.pc < (size_t)MemHandlerStart || context.pc >= (size_t)MemHandlerEnd)
		return false;

	void *ca = *(u32 *)(retAddr - 4) + retAddr;

	for (int size = 0; size < MemSize::Count; size++)
	{
		for (int op = 0; op < MemOp::Count; op++)
		{
			if ((void *)MemHandlers[MemType::Fast][size][op] != ca)
				continue;

			//found !
			const u8 *start = getCurr();
			if (op == MemOp::W && size >= MemSize::S32 && (context.eax >> 28) == 0xE)
				call(MemHandlers[MemType::StoreQueue][size][MemOp::W]);
			else
				call(MemHandlers[MemType::Slow][size][op]);
			verify(getCurr() - start == 5);

			ready();

			context.pc = (size_t)(retAddr - 5);
			//remove the call from call stack
			context.esp += 4;
			//restore the addr from eax to ecx so it's valid again
			context.ecx = context.eax;

			return true;
		}
	}
	ERROR_LOG(DYNAREC, "rewriteMemAccess code not found: hpc %08x retadr %p acc %08x", context.pc, retAddr, context.eax);
	die("Failed to match the code");

	return false;
}
#endif
