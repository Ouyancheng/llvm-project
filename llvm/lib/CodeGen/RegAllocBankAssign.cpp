//===- RegAllocBankAssign.cpp - Bank-aware precoloring for vector RA ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Build a register-bank conflict graph (RCG) for banked vector live ranges:
// two vregs are adjacent if they are simultaneous reads (or writes) of the
// same instruction. Color the RCG with N = TRI->getNumRegFileBanks() colors
// and constrain each vreg to the corresponding bank subclass. When coloring
// fails, split a use by inserting COPY so the new live range can take another
// bank. COPY itself is 1R+1W and does not add an RCG edge.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/RegAllocBankAssign.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Analysis.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "regalloc-bank-assign"

STATISTIC(NumBankAssigned,
          "Number of vregs constrained to a register-file bank");
STATISTIC(NumBankSplits,
          "Number of COPY splits inserted to make the RCG colorable");

namespace {

class RegAllocBankAssign {
public:
  RegAllocBankAssign(LiveIntervals *LIS) : LIS(LIS) {}
  bool run(MachineFunction &MF);

private:
  using AdjMap = DenseMap<Register, SmallVector<Register, 4>>;

  bool isBankedVReg(Register Reg) const;
  void collectOps(const MachineInstr &MI, SmallVectorImpl<Register> &Reads,
                  SmallVectorImpl<Register> &Writes) const;
  void addClique(AdjMap &Adj, ArrayRef<Register> Nodes) const;
  void buildRCG(AdjMap &Adj, SmallVectorImpl<Register> &Vertices) const;
  Register colorRCG(const AdjMap &Adj, ArrayRef<Register> Vertices,
                    DenseMap<Register, unsigned> &Color) const;
  bool splitOneUse(Register VReg, const AdjMap &Adj,
                   const DenseMap<Register, unsigned> &Color);
  bool constrainToBanks(const DenseMap<Register, unsigned> &Color);

  LiveIntervals *LIS = nullptr;
  MachineFunction *MF = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  unsigned NumBanks = 0;
};

class RegAllocBankAssignLegacy : public MachineFunctionPass {
public:
  static char ID;
  RegAllocBankAssignLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return "Register-file bank assignment";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char RegAllocBankAssignLegacy::ID;

char &llvm::RegAllocBankAssignID = RegAllocBankAssignLegacy::ID;

INITIALIZE_PASS_BEGIN(RegAllocBankAssignLegacy, DEBUG_TYPE,
                      "Register-file bank assignment", false, false)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(RegAllocBankAssignLegacy, DEBUG_TYPE,
                    "Register-file bank assignment", false, false)

FunctionPass *llvm::createRegAllocBankAssignPass() {
  return new RegAllocBankAssignLegacy();
}

bool RegAllocBankAssign::isBankedVReg(Register Reg) const {
  if (!Reg.isVirtual() || !MRI->getRegClassOrNull(Reg))
    return false;
  return TRI->isBankedVectorRegClass(MRI->getRegClass(Reg));
}

void RegAllocBankAssign::collectOps(const MachineInstr &MI,
                                    SmallVectorImpl<Register> &Reads,
                                    SmallVectorImpl<Register> &Writes) const {
  Reads.clear();
  Writes.clear();
  SmallSet<Register, 8> SeenReads;
  SmallSet<Register, 8> SeenWrites;
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || MO.isUndef() || !MO.getReg())
      continue;
    Register Reg = MO.getReg();
    if (!isBankedVReg(Reg))
      continue;
    if (MO.isDef()) {
      if (SeenWrites.insert(Reg).second)
        Writes.push_back(Reg);
    }
    if (MO.isUse()) {
      unsigned TiedDef = 0;
      if (MI.isRegTiedToDefOperand(MO.getOperandNo(), &TiedDef))
        continue;
      if (SeenReads.insert(Reg).second)
        Reads.push_back(Reg);
    }
  }
}

void RegAllocBankAssign::addClique(AdjMap &Adj,
                                   ArrayRef<Register> Nodes) const {
  for (unsigned I = 0, E = Nodes.size(); I != E; ++I) {
    for (unsigned J = I + 1; J != E; ++J) {
      auto &AI = Adj[Nodes[I]];
      if (!is_contained(AI, Nodes[J]))
        AI.push_back(Nodes[J]);
      auto &AJ = Adj[Nodes[J]];
      if (!is_contained(AJ, Nodes[I]))
        AJ.push_back(Nodes[I]);
    }
  }
}

void RegAllocBankAssign::buildRCG(AdjMap &Adj,
                                  SmallVectorImpl<Register> &Vertices) const {
  Adj.clear();
  Vertices.clear();
  SmallSet<Register, 16> Seen;
  for (const MachineBasicBlock &MBB : *MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.isDebugInstr() || MI.isImplicitDef() || MI.isKill())
        continue;
      // COPY / move-reg is 1R+1W and is allowed to change banks.
      if (MI.isCopy() || TII->isCopyInstr(MI))
        continue;
      SmallVector<Register, 8> Reads, Writes;
      collectOps(MI, Reads, Writes);
      if (Reads.size() > NumBanks) {
        LLVM_DEBUG(dbgs() << "Skipping oversized read clique in " << MI);
        continue;
      }
      if (Writes.size() > NumBanks) {
        LLVM_DEBUG(dbgs() << "Skipping oversized write clique in " << MI);
        continue;
      }
      addClique(Adj, Reads);
      addClique(Adj, Writes);
      for (Register R : Reads)
        if (Seen.insert(R).second)
          Vertices.push_back(R);
      for (Register R : Writes)
        if (Seen.insert(R).second)
          Vertices.push_back(R);
    }
  }
  // Isolated banked vregs still need a bank so allocation stays in one slice.
  for (unsigned I = 0, E = MRI->getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (!isBankedVReg(Reg) || MRI->reg_nodbg_empty(Reg))
      continue;
    if (Seen.insert(Reg).second) {
      Vertices.push_back(Reg);
      (void)Adj[Reg];
    }
  }
}

Register
RegAllocBankAssign::colorRCG(const AdjMap &Adj, ArrayRef<Register> Vertices,
                             DenseMap<Register, unsigned> &Color) const {
  Color.clear();
  for (Register V : Vertices)
    Color[V] = ~0u;

  auto Saturation = [&](Register V) {
    SmallBitVector Used(NumBanks);
    auto It = Adj.find(V);
    if (It == Adj.end())
      return 0;
    for (Register N : It->second) {
      unsigned C = Color.lookup(N);
      if (C != ~0u && C < NumBanks)
        Used.set(C);
    }
    return (int)Used.count();
  };

  auto Degree = [&](Register V) -> unsigned {
    auto It = Adj.find(V);
    return It == Adj.end() ? 0 : It->second.size();
  };

  auto LoadOf = [&](Register V, unsigned Bank) -> uint64_t {
    uint64_t Load = 0;
    if (!LIS->hasInterval(V))
      return 0;
    const LiveInterval &LI = LIS->getInterval(V);
    for (Register Other : Vertices) {
      if (Other == V || Color.lookup(Other) != Bank || !LIS->hasInterval(Other))
        continue;
      const LiveInterval &OL = LIS->getInterval(Other);
      if (LI.overlaps(OL))
        Load += OL.getSize();
    }
    return Load;
  };

  for (unsigned Assigned = 0, N = Vertices.size(); Assigned < N; ++Assigned) {
    Register Best;
    int BestSat = -1;
    unsigned BestDeg = 0;
    for (Register V : Vertices) {
      if (Color[V] != ~0u)
        continue;
      int Sat = Saturation(V);
      unsigned Deg = Degree(V);
      if (Sat > BestSat || (Sat == BestSat && Deg > BestDeg) ||
          !Best.isValid()) {
        Best = V;
        BestSat = Sat;
        BestDeg = Deg;
      }
    }
    assert(Best.isValid() && "no uncolored vertex");

    SmallBitVector Forbidden(NumBanks);
    auto It = Adj.find(Best);
    if (It != Adj.end()) {
      for (Register N : It->second) {
        unsigned C = Color.lookup(N);
        if (C != ~0u && C < NumBanks)
          Forbidden.set(C);
      }
    }

    int Chosen = -1;
    uint64_t BestLoad = ~0ull;
    for (unsigned B = 0; B < NumBanks; ++B) {
      if (Forbidden.test(B))
        continue;
      uint64_t Load = LoadOf(Best, B);
      if (Chosen < 0 || Load < BestLoad) {
        BestLoad = Load;
        Chosen = (int)B;
      }
    }
    if (Chosen < 0)
      return Best;
    Color[Best] = (unsigned)Chosen;
  }
  return Register();
}

bool RegAllocBankAssign::splitOneUse(
    Register VReg, const AdjMap &Adj,
    const DenseMap<Register, unsigned> &Color) {
  MachineOperand *SplitMO = nullptr;
  unsigned NumUses = 0;
  for (MachineOperand &MO : MRI->reg_nodbg_operands(VReg)) {
    if (!MO.isUse() || MO.isUndef())
      continue;
    MachineInstr &MI = *MO.getParent();
    if (MI.isCopy() || TII->isCopyInstr(MI) || MI.isDebugInstr())
      continue;
    unsigned TiedDef = 0;
    if (MI.isRegTiedToDefOperand(MO.getOperandNo(), &TiedDef))
      continue;
    ++NumUses;
    if (!SplitMO)
      SplitMO = &MO;
    // Prefer a use whose co-reads are already colored: that is the
    // constraint that made coloring fail.
    SmallVector<Register, 8> Reads, Writes;
    collectOps(MI, Reads, Writes);
    bool HasColoredNeighbor = false;
    for (Register R : Reads)
      if (R != VReg && Color.lookup(R) != ~0u)
        HasColoredNeighbor = true;
    if (HasColoredNeighbor)
      SplitMO = &MO;
  }
  if (!SplitMO || NumUses < 2)
    return false;

  const TargetRegisterClass *RC = MRI->getRegClass(VReg);
  Register NewVReg = MRI->createVirtualRegister(RC);
  MachineInstr &MI = *SplitMO->getParent();
  bool WasKill = SplitMO->isKill();
  MachineInstr *CopyMI = BuildMI(*MI.getParent(), MI, MI.getDebugLoc(),
                                 TII->get(TargetOpcode::COPY), NewVReg)
                             .addReg(VReg, getKillRegState(WasKill));
  SplitMO->setReg(NewVReg);
  SplitMO->setIsKill();
  LIS->InsertMachineInstrInMaps(*CopyMI);
  LIS->createAndComputeVirtRegInterval(NewVReg);
  if (LIS->hasInterval(VReg))
    LIS->shrinkToUses(&LIS->getInterval(VReg));
  ++NumBankSplits;
  LLVM_DEBUG(dbgs() << "Bank-split " << printReg(VReg, TRI) << " -> "
                    << printReg(NewVReg, TRI) << " at " << MI);
  return true;
}

bool RegAllocBankAssign::constrainToBanks(
    const DenseMap<Register, unsigned> &Color) {
  bool Changed = false;
  for (auto [Reg, Bank] : Color) {
    if (Bank == ~0u)
      continue;
    const TargetRegisterClass *RC = MRI->getRegClass(Reg);
    const TargetRegisterClass *BankRC = TRI->getRegClassForBank(RC, Bank);
    if (RC == BankRC)
      continue;
    if (!MRI->constrainRegClass(Reg, BankRC)) {
      LLVM_DEBUG(dbgs() << "Failed to constrain " << printReg(Reg, TRI)
                        << " to bank " << Bank << '\n');
      continue;
    }
    ++NumBankAssigned;
    Changed = true;
    LLVM_DEBUG(dbgs() << "Assigned " << printReg(Reg, TRI) << " to bank "
                      << Bank << '\n');
  }
  return Changed;
}

bool RegAllocBankAssign::run(MachineFunction &Fn) {
  MF = &Fn;
  MRI = &Fn.getRegInfo();
  TRI = Fn.getSubtarget().getRegisterInfo();
  TII = Fn.getSubtarget().getInstrInfo();

  NumBanks = 0;
  for (unsigned I = 0, E = MRI->getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (!isBankedVReg(Reg))
      continue;
    NumBanks =
        std::max(NumBanks, TRI->getNumRegFileBanks(MRI->getRegClass(Reg)));
  }
  if (NumBanks <= 1)
    return false;

  LLVM_DEBUG(dbgs() << "********** BANK ASSIGN " << MF->getName() << " ("
                    << NumBanks << " banks) **********\n");

  const unsigned MaxSplits = 64;
  for (unsigned Iter = 0; Iter <= MaxSplits; ++Iter) {
    AdjMap Adj;
    SmallVector<Register, 16> Vertices;
    buildRCG(Adj, Vertices);
    if (Vertices.empty())
      return Iter > 0;

    DenseMap<Register, unsigned> Color;
    Register Failed = colorRCG(Adj, Vertices, Color);
    if (!Failed)
      return constrainToBanks(Color) || Iter > 0;

    LLVM_DEBUG(dbgs() << "RCG not " << NumBanks << "-colorable at "
                      << printReg(Failed, TRI) << '\n');
    if (!splitOneUse(Failed, Adj, Color)) {
      // Try any other vertex with multiple uses.
      bool Split = false;
      for (Register V : Vertices) {
        if (splitOneUse(V, Adj, Color)) {
          Split = true;
          break;
        }
      }
      if (!Split) {
        LLVM_DEBUG(dbgs() << "Giving up; constraining partial coloring\n");
        return constrainToBanks(Color) || Iter > 0;
      }
    }
  }
  LLVM_DEBUG(dbgs() << "Hit split iteration limit\n");
  return true;
}

PreservedAnalyses
RegAllocBankAssignPass::run(MachineFunction &MF,
                            MachineFunctionAnalysisManager &MFAM) {
  auto &LIS = MFAM.getResult<LiveIntervalsAnalysis>(MF);
  if (!RegAllocBankAssign(&LIS).run(MF))
    return PreservedAnalyses::all();
  auto PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserve<SlotIndexesAnalysis>();
  return PA;
}

bool RegAllocBankAssignLegacy::runOnMachineFunction(MachineFunction &MF) {
  auto &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  return RegAllocBankAssign(&LIS).run(MF);
}
