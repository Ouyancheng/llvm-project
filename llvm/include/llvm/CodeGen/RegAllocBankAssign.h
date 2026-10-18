//===- llvm/CodeGen/RegAllocBankAssign.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REGALLOCBANKASSIGN_H
#define LLVM_CODEGEN_REGALLOCBANKASSIGN_H

#include "llvm/CodeGen/MachinePassManager.h"

namespace llvm {

/// Color the register-bank conflict graph of banked vector live ranges and
/// constrain each vreg to a per-bank subclass. Inserts COPY (cross-bank
/// moves) when the graph is not colorable with the available banks.
class RegAllocBankAssignPass
    : public RequiredPassInfoMixin<RegAllocBankAssignPass> {
public:
  LLVM_ABI PreservedAnalyses run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM);
};

} // namespace llvm

#endif // LLVM_CODEGEN_REGALLOCBANKASSIGN_H
