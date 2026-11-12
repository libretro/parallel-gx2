#pragma once
#include "util/Zir/Core/IR.h"

namespace ZirPass
{
	class ZpIRPass
	{
	public:
		ZpIRPass(ZpIR::ZpIRFunction* irFunction) : m_irFunction(irFunction) { };

		virtual void applyPass() = 0;

	protected:
		ZpIR::ZpIRFunction* m_irFunction;
	};

	class RegisterAllocatorForGLSL : public ZpIRPass
	{
		enum class PHYS_REG_TYPE : uint8
		{
			U32 = 0,
			S32 = 1,
			F32 = 2
		};

	public:
		RegisterAllocatorForGLSL(ZpIR::ZpIRFunction* irFunction) : ZpIRPass(irFunction) {};

		void applyPass()
		{
			assignPhysicalRegisters();
			m_irFunction->state.registersAllocated = true;
		}

		static bool IsPhysRegTypeU32(ZpIR::ZpIRPhysicalReg physReg)
		{
			return ((uint32)physReg >> 30) == (uint32)PHYS_REG_TYPE::U32;
		}

		static bool IsPhysRegTypeS32(ZpIR::ZpIRPhysicalReg physReg)
		{
			return ((uint32)physReg >> 30) == (uint32)PHYS_REG_TYPE::S32;
		}

		static bool IsPhysRegTypeF32(ZpIR::ZpIRPhysicalReg physReg)
		{
			return ((uint32)physReg >> 30) == (uint32)PHYS_REG_TYPE::F32;
		}

		static uint32 GetPhysRegIndex(ZpIR::ZpIRPhysicalReg physReg)
		{
			return (uint32)physReg & 0x3FFFFFFF;
		}

		static std::string DebugPrintHelper_getPhysRegisterName(ZpIR::ZpIRBasicBlock* block, ZpIR::ZpIRPhysicalReg r);

	private:
		void assignPhysicalRegisters();

		void assignPhysicalRegistersForBlock(ZpIR::ZpIRBasicBlock* basicBlock);

		uint32 m_physicalRegisterCounterU32{};
		uint32 m_physicalRegisterCounterS32{};
		uint32 m_physicalRegisterCounterF32{};

		ZpIR::ZpIRPhysicalReg MakePhysReg(PHYS_REG_TYPE regType, uint32 index)
		{
			uint32 v = (uint32)regType << 30;
			v |= index;
			return (ZpIR::ZpIRPhysicalReg)v;
		}
	};

};
