/*
	ARM7 (AICA sound CPU) recompiler -- x86-32 backend.

	Ported from arm7_rec_x64.cpp for the original Xbox (Pentium III, 32-bit).
	The ARM7 is integer-only, so there are NO SSE/FP concerns here.

	Key differences from the x64 backend (which has r8d-r15d as extra scratch):
	  * Only 3 host registers are used for the ARM register file (esi, edi, ebp).
	    The shared ArmRegAlloc spills the rest to memory -- still far cheaper than
	    the interpreter's per-instruction decode+dispatch.
	  * Operand scratch: r8d -> ebx, r9d -> edx.
	  * The persistent "carry to save" (r10d) lives in a memory slot; the saveFlags
	    temp (r11d) and the ADC/SBC carry-in are folded into eax.
	  * rip-relative addressing -> absolute (dword[&x]), matching rec-x86 (SH4).
	  * __fastcall (DYNACALL) => call_regs = { ecx, edx }.
	  * No jit_set_exec: the Xbox code cache is always RWX.
 */

#include "build.h"

#if HOST_CPU == CPU_X86 && FEAT_AREC != DYNAREC_NONE

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
using namespace Xbyak::util;

#include "arm7_rec.h"
#include "oslib/unwind_info.h"
#include "hw/aica/aica_if.h"	// aica_ram -- inline ARAM fast path in emitMemOp

namespace aica::arm
{

static void (*arm_dispatch)();
static void (**entry_points)();
static UnwindInfo unwinder;

// call_regs: __fastcall passes the first two integer args in ecx, edx.
static const Xbyak::Reg32 call_regs[] = { ecx, edx };

// Persistent "carry bit to write into PSR" (was r10d on x64). Kept in memory so
// it survives the op body -> saveFlags without stealing a register.
static u32 arm_scratch_carry;

class Arm7Compiler;

// esi/edi/ebp hold ARM registers; eax/ecx/edx/ebx are scratch. (ebp is safe as a
// data register here -- ARM7 JIT code never faults, so no SEH frame is needed;
// the SH-4 rec-x86 uses ebp the same way.)
static const std::array<Xbyak::Reg32, 3> alloc_regs { esi, edi, ebp };

class X86ArmRegAlloc : public ArmRegAlloc<std::size(alloc_regs), X86ArmRegAlloc>
{
	using super = ArmRegAlloc<std::size(alloc_regs), X86ArmRegAlloc>;
	Arm7Compiler& assembler;

	void LoadReg(int host_reg, Arm7Reg armreg);
	void StoreReg(int host_reg, Arm7Reg armreg);

	static const Xbyak::Reg32& getReg32(int i)
	{
		verify(i >= 0 && (u32)i < alloc_regs.size());
		return alloc_regs[i];
	}

public:
	X86ArmRegAlloc(Arm7Compiler& assembler, const std::vector<ArmOp>& block_ops)
		: super(block_ops), assembler(assembler) {}

	const Xbyak::Reg32& map(Arm7Reg r)
	{
		int i = super::map(r);
		return getReg32(i);
	}

	friend super;
};

class Arm7Compiler : public Xbyak::CodeGenerator
{
	bool logical_op_set_flags = false;
	bool set_carry_bit = false;
	bool set_flags = false;
	X86ArmRegAlloc *regalloc = nullptr;

	static const u32 N_FLAG = 1 << 31;
	static const u32 Z_FLAG = 1 << 30;
	static const u32 C_FLAG = 1 << 29;
	static const u32 V_FLAG = 1 << 28;

	Xbyak::Operand getOperand(const ArmOp::Operand& arg, Xbyak::Reg32 scratch_reg)
	{
		Xbyak::Reg32 r;
		if (!arg.isReg())
		{
			if (arg.isNone() || arg.shift_imm)
				return Xbyak::Operand();
			mov(scratch_reg, arg.getImmediate());
			r = scratch_reg;
		}
		else
			r = regalloc->map(arg.getReg().armreg);
		if (arg.isShifted())
		{
			if (r != scratch_reg)
			{
				mov(scratch_reg, r);
				r = scratch_reg;
			}
			if (arg.shift_imm)
			{
				// shift by immediate
				if (arg.shift_type != ArmOp::ROR && arg.shift_value != 0 && !logical_op_set_flags)
				{
					switch (arg.shift_type)
					{
					case ArmOp::LSL: shl(r, arg.shift_value); break;
					case ArmOp::LSR: shr(r, arg.shift_value); break;
					case ArmOp::ASR: sar(r, arg.shift_value); break;
					default: die("invalid"); break;
					}
				}
				else if (arg.shift_value == 0)
				{
					// Shift by 32
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSR)
					{
						if (set_carry_bit)
						{
							mov(dword[&arm_scratch_carry], r);	// carry = rm[31]
							shr(dword[&arm_scratch_carry], 31);
						}
						mov(r, 0);							// r = 0
					}
					else if (arg.shift_type == ArmOp::ASR)
					{
						if (set_carry_bit)
						{
							mov(dword[&arm_scratch_carry], r);	// carry = rm[31]
							shr(dword[&arm_scratch_carry], 31);
						}
						sar(r, 31);							// r = rm < 0 ? -1 : 0
					}
					else if (arg.shift_type == ArmOp::ROR)
					{
						// RRX: r = (rm >> 1) | (oldC << 31); new C = rm[0]
						verify(r != eax);
						if (set_carry_bit)
						{
							mov(eax, r);
							and_(eax, 1);
							mov(dword[&arm_scratch_carry], eax);	// new C = rm[0]
						}
						mov(eax, dword[&arm_Reg[RN_PSR_FLAGS].I]);
						shl(eax, 2);						// C(bit29) -> bit31
						and_(eax, 0x80000000);				// eax = oldC in bit31
						shr(r, 1);							// r = rm >> 1
						or_(r, eax);						// r[31] = oldC
					}
					else
						die("Invalid shift");
				}
				else
				{
					// Carry must be preserved or Ror shift
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSL)
					{
						if (set_carry_bit)
						{
							mov(dword[&arm_scratch_carry], r);
							shr(dword[&arm_scratch_carry], 32 - arg.shift_value);
						}
						shl(r, arg.shift_value);			// r <<= shift
						if (set_carry_bit)
							and_(dword[&arm_scratch_carry], 1);	// carry = rm[lsb]
					}
					else
					{
						if (set_carry_bit)
						{
							mov(dword[&arm_scratch_carry], r);
							shr(dword[&arm_scratch_carry], arg.shift_value - 1);
							and_(dword[&arm_scratch_carry], 1);	// carry = rm[msb]
						}
						if (arg.shift_type == ArmOp::LSR)
							shr(r, arg.shift_value);
						else if (arg.shift_type == ArmOp::ASR)
							sar(r, arg.shift_value);
						else if (arg.shift_type == ArmOp::ROR)
							ror(r, arg.shift_value);
						else
							die("Invalid shift");
					}
				}
			}
			else
			{
				// shift by register
				const Xbyak::Reg32 shift_reg = regalloc->map(arg.shift_reg.armreg);
				switch (arg.shift_type)
				{
				case ArmOp::LSL:
				case ArmOp::LSR:
					mov(ecx, shift_reg);
					mov(eax, 0);
					if (arg.shift_type == ArmOp::LSL)
						shl(r, cl);
					else
						shr(r, cl);
					cmp(shift_reg, 32);
					cmovnb(r, eax);		// LSL/LSR by >=32 gives 0
					break;
				case ArmOp::ASR:
					mov(ecx, shift_reg);
					mov(eax, r);
					sar(eax, 31);
					sar(r, cl);
					cmp(shift_reg, 32);
					cmovnb(r, eax);		// ASR by >=32 gives 0 or -1
					break;
				case ArmOp::ROR:
					mov(ecx, shift_reg);
					ror(r, cl);
					break;
				default:
					die("Invalid shift");
					break;
				}
			}
		}
		return r;
	}

	Xbyak::Label *startConditional(ArmOp::Condition cc)
	{
		if (cc == ArmOp::AL)
			return nullptr;
		Xbyak::Label *label = new Xbyak::Label();
		cc = (ArmOp::Condition)((u32)cc ^ 1);	// invert the condition
		mov(eax, dword[&arm_Reg[RN_PSR_FLAGS].I]);
		switch (cc)
		{
		case ArmOp::EQ:	and_(eax, Z_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::NE:	and_(eax, Z_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::CS:	and_(eax, C_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::CC:	and_(eax, C_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::MI:	and_(eax, N_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::PL:	and_(eax, N_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::VS:	and_(eax, V_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::VC:	and_(eax, V_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::HI:	// (C==1) && (Z==0)
			and_(eax, C_FLAG | Z_FLAG); cmp(eax, C_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::LS:	// (C==0) || (Z==1)
			and_(eax, C_FLAG | Z_FLAG); cmp(eax, C_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::GE:	// N==V
			mov(ecx, eax); shl(ecx, 3); xor_(eax, ecx); and_(eax, N_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::LT:	// N!=V
			mov(ecx, eax); shl(ecx, 3); xor_(eax, ecx); and_(eax, N_FLAG); jnz(*label, T_NEAR); break;
		case ArmOp::GT:	// (Z==0) && (N==V)
			mov(ecx, eax); mov(edx, eax); shl(ecx, 3); shl(edx, 1);
			xor_(eax, ecx); or_(eax, edx); and_(eax, N_FLAG); jz(*label, T_NEAR); break;
		case ArmOp::LE:	// (Z==1) || (N!=V)
			mov(ecx, eax); mov(edx, eax); shl(ecx, 3); shl(edx, 1);
			xor_(eax, ecx); or_(eax, edx); and_(eax, N_FLAG); jnz(*label, T_NEAR); break;
		default:
			die("Invalid condition code"); break;
		}
		return label;
	}

	void endConditional(Xbyak::Label *label)
	{
		if (label != nullptr)
		{
			L(*label);
			delete label;
		}
	}

	bool emitDataProcOp(const ArmOp& op)
	{
		bool save_v_flag = true;

		Xbyak::Operand arg0 = getOperand(op.arg[0], ebx);	// r8d -> ebx
		Xbyak::Operand arg1 = getOperand(op.arg[1], edx);	// r9d -> edx
		Xbyak::Reg32 rd;
		if (op.rd.isReg())
			rd = regalloc->map(op.rd.getReg().armreg);
		if (logical_op_set_flags)
		{
			// carry <- bit[31] of a >255 shifted immediate operand2
			if (op.arg[0].isImmediate() && op.arg[0].getImmediate() > 255)
			{
				set_carry_bit = true;
				mov(dword[&arm_scratch_carry], (op.arg[0].getImmediate() & 0x80000000) >> 31);
			}
			else if (op.arg[1].isImmediate() && op.arg[1].getImmediate() > 255)
			{
				set_carry_bit = true;
				mov(dword[&arm_scratch_carry], (op.arg[1].getImmediate() & 0x80000000) >> 31);
			}
		}

		switch (op.op_type)
		{
		case ArmOp::AND:
			if (arg1 == rd)
				and_(rd, arg0);
			else
			{
				if (rd != arg0) { mov(rd, arg0); verify(rd != arg1); }
				if (!arg1.isNone())	and_(rd, arg1);
				else				and_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::ORR:
			if (arg1 == rd)
				or_(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					if (arg0.isNone())	mov(rd, op.arg[0].getImmediate());
					else				mov(rd, arg0);
					verify(rd != arg1);
				}
				if (!arg1.isNone())	or_(rd, arg1);
				else				or_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::EOR:
			if (arg1 == rd)
				xor_(rd, arg0);
			else
			{
				if (rd != arg0) { verify(rd != arg1); mov(rd, arg0); }
				if (!arg1.isNone())	xor_(rd, arg1);
				else				xor_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::BIC:
			if (arg1.isNone())
			{
				mov(eax, op.arg[1].getImmediate());
				arg1 = eax;
			}
			if (rd == arg0)
			{
				if (arg1 != edx) mov(edx, arg1);
				not_(edx);
				and_(rd, edx);
			}
			else
			{
				if (arg1 != rd) mov(rd, static_cast<Xbyak::Reg32&>(arg1));
				not_(rd);
				and_(rd, arg0);
			}
			save_v_flag = false;
			break;

		case ArmOp::TST:
			if (!arg1.isNone())	test(arg0, static_cast<Xbyak::Reg32&>(arg1));
			else				test(arg0, op.arg[1].getImmediate());
			save_v_flag = false;
			break;
		case ArmOp::TEQ:
			if (arg0 != ebx) mov(ebx, arg0);
			if (!arg1.isNone())	xor_(ebx, arg1);
			else				xor_(ebx, op.arg[1].getImmediate());
			save_v_flag = false;
			break;
		case ArmOp::CMP:
			if (!arg1.isNone())	cmp(arg0, arg1);
			else				cmp(arg0, op.arg[1].getImmediate());
			if (set_flags) { setnb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::CMN:
			if (arg0 != ebx) mov(ebx, arg0);
			if (!arg1.isNone())	add(ebx, arg1);
			else				add(ebx, op.arg[1].getImmediate());
			if (set_flags) { setb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;

		case ArmOp::MOV:
			if (arg0.isNone())		mov(rd, op.arg[0].getImmediate());
			else if (arg0 != rd)	mov(rd, arg0);
			if (set_flags) { test(rd, rd); save_v_flag = false; }
			break;
		case ArmOp::MVN:
			if (arg0.isNone())	mov(rd, ~op.arg[0].getImmediate());
			else { if (arg0 != rd) mov(rd, arg0); not_(rd); }
			if (set_flags) { test(rd, rd); save_v_flag = false; }
			break;

		case ArmOp::SUB:
			if (arg1 == rd)
			{
				sub(arg0, arg1);
				if (rd != arg0) mov(rd, arg0);
			}
			else
			{
				if (rd != arg0) mov(rd, arg0);
				if (arg1.isNone())	sub(rd, op.arg[1].getImmediate());
				else				sub(rd, arg1);
			}
			if (set_flags) { setnb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::RSB:
			if (arg1 == rd)
				sub(rd, arg0);
			else
			{
				if (rd != arg0) mov(rd, arg0);
				neg(rd);
				if (arg1.isNone())	add(rd, op.arg[1].getImmediate());
				else				add(rd, arg1);
			}
			if (set_flags) { setb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::ADD:
			if (arg1 == rd)
				add(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					if (arg0.isNone())	mov(rd, op.arg[0].getImmediate());
					else				mov(rd, arg0);
				}
				if (arg1.isNone())	add(rd, op.arg[1].getImmediate());
				else				add(rd, arg1);
			}
			if (set_flags) { setb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::ADC:
			// eax used only to turn the ARM C flag into the x86 carry flag
			mov(eax, dword[&arm_Reg[RN_PSR_FLAGS].I]);
			and_(eax, C_FLAG);
			neg(eax);				// CF = ARM C
			if (arg1 == rd)
				adc(rd, arg0);
			else
			{
				if (rd != arg0) mov(rd, arg0);
				if (arg1.isNone())	adc(rd, op.arg[1].getImmediate());
				else				adc(rd, arg1);
			}
			if (set_flags) { setb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::SBC:
			// rd = rn - op2 - !C
			mov(eax, dword[&arm_Reg[RN_PSR_FLAGS].I]);
			and_(eax, C_FLAG);
			neg(eax);
			cmc();		// on arm, borrow if carry is clear
			if (arg1 == rd)
			{
				sbb(arg0, arg1);
				if (rd != arg0) mov(rd, arg0);
			}
			else
			{
				if (rd != arg0) mov(rd, arg0);
				if (arg1.isNone())	sbb(rd, op.arg[1].getImmediate());
				else				sbb(rd, arg1);
			}
			if (set_flags) { setnb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		case ArmOp::RSC:
			// rd = op2 - rn - !C
			mov(eax, dword[&arm_Reg[RN_PSR_FLAGS].I]);
			and_(eax, C_FLAG);
			neg(eax);
			cmc();
			if (arg1 == rd)
				sbb(rd, arg0);
			else
			{
				if (arg1.isNone())		mov(rd, op.arg[1].getImmediate());
				else if (rd != arg1)	mov(rd, static_cast<Xbyak::Reg32&>(arg1));
				sbb(rd, arg0);
			}
			if (set_flags) { setnb(byte[&arm_scratch_carry]); set_carry_bit = true; }
			break;
		default:
			die("invalid");
			break;
		}

		return save_v_flag;
	}

	void emitMemOp(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], call_regs[0]);
		if (addr_reg != call_regs[0])
		{
			if (addr_reg.isNone())	mov(call_regs[0], op.arg[0].getImmediate());
			else					mov(call_regs[0], addr_reg);
			addr_reg = call_regs[0];
		}
		if (op.pre_index)
		{
			const ArmOp::Operand& offset = op.arg[1];
			// BUG FIX: getOperand's shift-by-REGISTER case (e.g. offset is
			// "Rm, LSL Rs") hardcodes ecx/eax as scratch for the shift count,
			// regardless of the scratch_reg we pass it -- but ecx (call_regs[0])
			// is already holding the base address computed above! Without this
			// save/restore, that shift computation silently clobbers the base
			// address before the offset is even added, corrupting the effective
			// memory address (observed as intermittent audio corruption --
			// ARM7's own sample-buffer LDR/STR hit this). ebx is free here (unused
			// elsewhere in this function); the offset result always lands in edx
			// (the scratch_reg we pass), which this save/restore doesn't touch.
			bool offsetClobbersEcx = offset.isReg() && !offset.shift_imm;
			if (offsetClobbersEcx)
				mov(ebx, ecx);

			Xbyak::Operand offset_reg = getOperand(offset, edx);	// r9d -> edx

			if (offsetClobbersEcx)
				mov(ecx, ebx);

			if (!offset_reg.isNone())
			{
				if (op.add_offset)	add(addr_reg, offset_reg);
				else				sub(addr_reg, offset_reg);
			}
			else if (offset.isImmediate() && offset.getImmediate() != 0)
			{
				if (op.add_offset)	add(addr_reg, offset.getImmediate());
				else				sub(addr_reg, offset.getImmediate());
			}
		}
		// addr_reg == ecx (call_regs[0]) from here on, and is left UNTOUCHED by the
		// fast path below so it's still valid as the __fastcall addr arg if we fall
		// through to the slow call.
		if (op.op_type == ArmOp::STR)
		{
			if (op.arg[2].isImmediate())	mov(call_regs[1], op.arg[2].getImmediate());
			else							mov(call_regs[1], regalloc->map(op.arg[2].getReg().armreg));
		}

		// ---- Inline fast path: plain ARAM access. -----------------------------
		// The sound driver's LDR/STR overwhelmingly targets the 8MB ARAM window
		// (sample buffers, channel/common data) with the *generic* backend always
		// paying a full call+ret into DoMemOp -> readMem/writeMem for EVERY
		// access. readMem/writeMem's ARAM case is just a masked array read/write
		// (arm_mem.h), so replicate that exact logic inline and only fall back to
		// the call for AICA register-space (addr>=0x800000, rare) or a misaligned
		// 32-bit access (readMem's byte-rotate case, rare/never in practice) --
		// matches the ORIGINAL semantics exactly, just skips the call for the
		// common case.
		Xbyak::Label slow, done;
		mov(eax, addr_reg);
		and_(eax, 0x00FFFFFF);
		cmp(eax, 0x800000);
		jae(slow, T_NEAR);
		if (!op.byte_xfer)
		{
			test(eax, 3);
			jnz(slow, T_NEAR);		// misaligned 32-bit -- let the slow path's rotate logic handle it
		}
		and_(eax, (u32)ARAM_MASK);
		u8* aramBase = &aica_ram[0];
		if (op.op_type == ArmOp::LDR)
		{
			Xbyak::Reg32 rd = regalloc->map(op.rd.getReg().armreg);
			if (op.byte_xfer)	movzx(rd, byte[eax + (size_t)aramBase]);
			else				mov(rd, dword[eax + (size_t)aramBase]);
			jmp(done, T_NEAR);
			L(slow);
			call(recompiler::getMemOp(true, op.byte_xfer));
			mov(rd, eax);
			L(done);
		}
		else
		{
			if (op.byte_xfer)	mov(byte[eax + (size_t)aramBase], dl);
			else				mov(dword[eax + (size_t)aramBase], edx);
			jmp(done, T_NEAR);
			L(slow);
			call(recompiler::getMemOp(false, op.byte_xfer));
			L(done);
		}
	}

	void saveFlags(bool save_v_flag)
	{
		if (!set_flags)
			return;

		// LAHF+SETO instead of PUSHFD+POP: same bits (ZF/SF land in AH, OF via a
		// byte set), no round trip through the stack -- cheaper on every single
		// flag-setting instruction, which is most ARM7 ALU ops.
		lahf();						// AH (eax bits 15:8) = SF ZF 0 AF 0 PF 1 CF
		if (save_v_flag)
			seto(dl);					// dl = OF (0/1), captured before eax is reshuffled below

		shl(eax, 16);				// bit31=SF(N), bit30=ZF(Z); other bits are stale/masked next
		and_(eax, Z_FLAG | N_FLAG);	// eax = Z,N

		if (save_v_flag)
		{
			movzx(edx, dl);			// zero-extend OF bit (dl's value from seto above)
			shl(edx, 28);			// V bit into position
			or_(eax, edx);			// eax = Z,N,V
		}

		mov(edx, dword[&arm_Reg[RN_PSR_FLAGS].I]);	// edx = old PSR (dl's OF value already consumed above)
		if (set_carry_bit)
		{
			if (save_v_flag)	and_(edx, (u32)~(Z_FLAG | N_FLAG | C_FLAG | V_FLAG));
			else				and_(edx, (u32)~(Z_FLAG | N_FLAG | C_FLAG));
			mov(ecx, dword[&arm_scratch_carry]);	// 0/1
			shl(ecx, 29);
			or_(edx, ecx);
		}
		else
		{
			if (save_v_flag)	and_(edx, (u32)~(Z_FLAG | N_FLAG | V_FLAG));
			else				and_(edx, (u32)~(Z_FLAG | N_FLAG));
		}
		or_(edx, eax);
		mov(dword[&arm_Reg[RN_PSR_FLAGS].I], edx);
	}

	void emitBranch(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], eax);
		if (addr_reg.isNone())
			mov(eax, op.arg[0].getImmediate());
		else
		{
			if (eax != addr_reg)
				mov(eax, addr_reg);
			and_(eax, 0xfffffffc);
		}
		mov(dword[&arm_Reg[R15_ARM_NEXT].I], eax);
	}

	void emitMSR(const ArmOp& op)
	{
		if (op.arg[0].isImmediate())	mov(call_regs[0], op.arg[0].getImmediate());
		else							mov(call_regs[0], regalloc->map(op.arg[0].getReg().armreg));
		if (op.spsr)	call(recompiler::MSR_do<1>);
		else			call(recompiler::MSR_do<0>);
	}

	void emitMRS(const ArmOp& op)
	{
		call(CPUUpdateCPSR);

		if (op.spsr)	mov(regalloc->map(op.rd.getReg().armreg), dword[&arm_Reg[RN_SPSR].I]);
		else			mov(regalloc->map(op.rd.getReg().armreg), dword[&arm_Reg[RN_CPSR].I]);
	}

	void emitFallback(const ArmOp& op)
	{
		set_flags = false;
		mov(call_regs[0], op.arg[0].getImmediate());
		call(recompiler::interpret);
	}

public:
	Arm7Compiler() : Xbyak::CodeGenerator(recompiler::spaceLeft(), recompiler::currentCode()) { }

	void compile(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		regalloc = new X86ArmRegAlloc(*this, block_ops);

		sub(dword[&arm_Reg[CYCL_CNT].I], cycles);

		ArmOp::Condition currentCondition = ArmOp::AL;
		Xbyak::Label *condLabel = nullptr;

		for (u32 i = 0; i < block_ops.size(); i++)
		{
			const ArmOp& op = block_ops[i];
			DEBUG_LOG(AICA_ARM, "-> %s", op.toString().c_str());

			set_flags = op.flags & ArmOp::OP_SETS_FLAGS;
			logical_op_set_flags = op.isLogicalOp() && set_flags;
			set_carry_bit = false;
			bool save_v_flag = true;

			if (op.op_type == ArmOp::FALLBACK)
			{
				endConditional(condLabel);
				condLabel = nullptr;
				currentCondition = ArmOp::AL;
			}
			else if (op.condition != currentCondition)
			{
				endConditional(condLabel);
				currentCondition = op.condition;
				condLabel = startConditional(op.condition);
			}

			regalloc->load(i);

			if (op.op_type <= ArmOp::MVN)
				save_v_flag = emitDataProcOp(op);
			else if (op.op_type <= ArmOp::STR)
				emitMemOp(op);
			else if (op.op_type <= ArmOp::BL)
				emitBranch(op);
			else if (op.op_type == ArmOp::MRS)
				emitMRS(op);
			else if (op.op_type == ArmOp::MSR)
				emitMSR(op);
			else if (op.op_type == ArmOp::FALLBACK)
				emitFallback(op);
			else
				die("invalid");

			saveFlags(save_v_flag);

			regalloc->store(i);

			if (set_flags)
			{
				currentCondition = ArmOp::AL;
				endConditional(condLabel);
				condLabel = nullptr;
			}
		}
		endConditional(condLabel);

		jmp((void*)arm_dispatch);

		ready();
		recompiler::advance(getSize());

		delete regalloc;
		regalloc = nullptr;
	}

	void generateMainLoop()
	{
		if (!recompiler::empty())
		{
			verify(arm_mainloop != nullptr);
			verify(arm_compilecode != nullptr);
			return;
		}
		Xbyak::Label arm_dispatch_label;
		Xbyak::Label arm_mainloop_label;

		// arm_compilecode:  (every not-yet-compiled EntryPoints slot lands here)
		call(recompiler::compile);
		jmp(arm_dispatch_label);

		// arm_mainloop(reg_pair *arm_regs /*unused*/, void (*entrypoints[])())
		L(arm_mainloop_label);
		unwinder.start((void *)getCurr());
		size_t startOffset = getSize();
		push(ebx);	unwinder.pushReg(getSize(), Xbyak::Operand::EBX);
		push(esi);	unwinder.pushReg(getSize(), Xbyak::Operand::ESI);
		push(edi);	unwinder.pushReg(getSize(), Xbyak::Operand::EDI);
		push(ebp);	unwinder.pushReg(getSize(), Xbyak::Operand::EBP);
		sub(esp, 12);	// 16-byte stack alignment (4 pushes + ret addr = 20 -> +12 = 32)
		unwinder.allocStack(getSize(), 12);
		unwinder.endProlog(getSize());

		// entrypoints arg: cdecl, at [esp + 8] on entry; now +28 (16 pushed + 12 sub).
		mov(eax, dword[esp + 8 + 16 + 12]);
		mov(dword[&entry_points], eax);

		// arm_dispatch:
		L(arm_dispatch_label);
		mov(edx, dword[&entry_points]);
		mov(ecx, dword[&arm_Reg[R15_ARM_NEXT].I]);
		mov(eax, dword[&arm_Reg[INTR_PEND].I]);
		cmp(dword[&arm_Reg[CYCL_CNT].I], 0);
		Xbyak::Label arm_exit;
		jle(arm_exit);			// timeslice over
		test(eax, eax);
		Xbyak::Label arm_dofiq;
		jne(arm_dofiq);			// interrupt pending

		and_(ecx, 0x7ffffc);
		jmp(dword[edx + ecx]);	// 4-byte entries: byte offset == (pc/4)*4 == pc

		// arm_dofiq:
		L(arm_dofiq);
		call(CPUFiq);
		jmp(arm_dispatch_label);

		// arm_exit:
		L(arm_exit);
		add(esp, 12);
		pop(ebp);
		pop(edi);
		pop(esi);
		pop(ebx);
		ret();

		size_t savedSize = getSize();
		setSize(recompiler::spaceLeft() - 128 - startOffset);
		size_t unwindSize = unwinder.end(getSize());
		verify(unwindSize <= 128);
		setSize(savedSize);

		ready();
		arm_compilecode = (void (*)())getCode();
		arm_mainloop = (arm_mainloop_t)arm_mainloop_label.getAddress();
		arm_dispatch = (void (*)())arm_dispatch_label.getAddress();

		recompiler::advance(getSize());
	}
};

void X86ArmRegAlloc::LoadReg(int host_reg, Arm7Reg armreg)
{
	assembler.mov(getReg32(host_reg), dword[&arm_Reg[(u32)armreg].I]);
}

void X86ArmRegAlloc::StoreReg(int host_reg, Arm7Reg armreg)
{
	assembler.mov(dword[&arm_Reg[(u32)armreg].I], getReg32(host_reg));
}

void arm7backend_compile(const std::vector<ArmOp>& block_ops, u32 cycles)
{
	Arm7Compiler assembler;
	assembler.compile(block_ops, cycles);
}

void arm7backend_flush()
{
	unwinder.clear();
	Arm7Compiler assembler;
	assembler.generateMainLoop();
}

} // namespace aica::arm
#endif // HOST_CPU == CPU_X86 && FEAT_AREC != DYNAREC_NONE
