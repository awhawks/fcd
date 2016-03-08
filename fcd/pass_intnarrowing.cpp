//
// pass_intnarrowing.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is part of fcd.
// 
// fcd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// fcd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with fcd.  If not, see <http://www.gnu.org/licenses/>.
//

#include "llvm_warnings.h"
#include "passes.h"

SILENCE_LLVM_WARNINGS_BEGIN()
#include <llvm/Analysis/DemandedBits.h>
#include <llvm/IR/Constants.h>
SILENCE_LLVM_WARNINGS_END()

#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace std;

namespace
{
	struct IntNarrowing : public FunctionPass
	{
		static char ID;
		
		unordered_map<Value*, SmallDenseMap<unsigned, Instruction*, 8>> resized;
		
		IntNarrowing() : FunctionPass(ID)
		{
		}
		
		virtual const char* getPassName() const override
		{
			return "Narrow Integers";
		}
		
		virtual void getAnalysisUsage(AnalysisUsage& au) const override
		{
			au.addRequired<DemandedBits>();
		}
		
		Value* narrowDown(Value* thatValue, unsigned size, Instruction* location = nullptr)
		{
			auto valueSize = thatValue->getType()->getIntegerBitWidth();
			if (valueSize == size)
			{
				return thatValue;
			}
			
			auto& valueMap = resized[thatValue];
			auto& value = valueMap[size];
			if (value == nullptr)
			{
				location = location == nullptr ? cast<Instruction>(thatValue) : location;
				if (auto binOp = dyn_cast<BinaryOperator>(thatValue))
				{
					Value* left = narrowDown(binOp->getOperand(0), size, location);
					Value* right = narrowDown(binOp->getOperand(1), size, location);
					value = BinaryOperator::Create(binOp->getOpcode(), left, right, "", location);
				}
				else
				{
					assert(valueSize > size);
					Type* truncatedType = Type::getIntNTy(thatValue->getContext(), size);
					value = CastInst::Create(Instruction::Trunc, thatValue, truncatedType, "", location);
				}
			}
			return value;
		}
		
		virtual bool runOnFunction(Function& fn) override
		{
			resized.clear();
			DemandedBits& db = getAnalysis<DemandedBits>();
			
			for (BasicBlock& bb : fn)
			{
				for (Instruction& inst : bb)
				{
					if (auto binOp = dyn_cast<BinaryOperator>(&inst))
					if (binOp->getType()->isIntegerTy())
					{
						unsigned typeBits = binOp->getType()->getIntegerBitWidth();
						unsigned activeBits = typeBits;
						if (binOp->getOpcode() == BinaryOperator::And)
						{
							if (auto constantMask = dyn_cast<ConstantInt>(binOp->getOperand(1)))
							{
								activeBits = constantMask->getValue().getActiveBits();
							}
						}
						else
						{
							activeBits = db.getDemandedBits(binOp).getActiveBits();
						}
						
						if (activeBits < typeBits)
						{
							narrowDown(binOp, activeBits);
						}
					}
				}
			}
			
			for (auto& pair : resized)
			{
				if (auto key = dyn_cast<Instruction>(pair.first))
				{
					auto& otherSizes = pair.second;
					if (otherSizes.size() == 1)
					{
						auto toEnlarge = otherSizes.begin()->second;
						auto type = key->getType();
						CastInst* enlarged = CastInst::Create(Instruction::ZExt, toEnlarge, type);
						enlarged->insertAfter(toEnlarge);
						
						// replace almost all uses with
						for (Use& use : key->uses())
						{
							auto user = use.getUser();
							if (user != toEnlarge && resized.count(user) == 0)
							{
								use.set(enlarged);
							}
						}
					}
				}
			}
			
			return resized.size() > 0;
		}
	};
	
	char IntNarrowing::ID = 0;
	RegisterPass<IntNarrowing> intNarrowing("intnarrowing", "Narrow down integer types to their used bits");
}

FunctionPass* createIntNarrowingPass()
{
	return new IntNarrowing;
}