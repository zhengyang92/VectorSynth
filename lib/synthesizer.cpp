// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.

#include "lib/llvm2alive.h"
#include "lib/ir.h"
#include "lib/constantsynth.h"
#include "smt/smt.h"
#include "tools/transform.h"
#include "util/symexec.h"
#include "util/config.h"
#include "util/version.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Scalar/DCE.h"

#include <iostream>
#include <queue>
#include <vector>
#include <set>
#include <map>

using namespace tools;
using namespace util;
using namespace std;
using namespace IR;

namespace vectorsynth {

static void findInputs(llvm::Value *Root, std::set<unique_ptr<Var>> &Cands, unsigned Max) {
  // breadth-first search
  unordered_set<llvm::Value *> Visited;
  queue<llvm::Value *> Q;
  Q.push(Root);

  while (!Q.empty()) {
    llvm::Value *V = Q.front();
    Q.pop();
    if (Visited.insert(V).second) {
      if (auto I = llvm::dyn_cast<llvm::Instruction>(V)) {
        for (auto &Op : I->operands()) {
          Q.push(Op);
        }
      }

      if (llvm::isa<llvm::Constant>(V))
        continue;
      if (V == Root)
        continue;
      Cands.insert(make_unique<Var>(V));
      if (Cands.size() >= Max)
        return;
    }
  }
}

static bool getSketches(set<unique_ptr<Var>> &Inputs, llvm::Value *V,
                        vector<pair<unique_ptr<Inst>,set<unique_ptr<ReservedConst>>>> &R) {
  auto &Ctx = V->getContext();
  R.clear();
  vector<Comp *> Comps;
  for (auto &I : Inputs) {
    Comps.emplace_back(I.get());
  }

  auto RC1 = make_unique<ReservedConst>(nullptr);
  Comps.emplace_back(RC1.get());
  llvm::Type *ty = V->getType();
  for (unsigned K = BinOp::Op::band; K <= BinOp::Op::mul; K++) {
    for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
      for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
        Inst *I = nullptr, *J = nullptr;
        set<unique_ptr<ReservedConst>> RCs;

        // (op rc, var)
        if (dynamic_cast<ReservedConst *>(*Op0)) {
          if (auto R = dynamic_cast<Var *>(*Op1)) {
            // ignore icmp temporarily
            if (R->V()->getType() != ty)
              continue;
            auto T = make_unique<ReservedConst>(R->V()->getType());
            I = T.get();
            RCs.insert(move(T));
            J = R;
          } else continue;
        }
        // (op var, rc)
        else if (dynamic_cast<ReservedConst *>(*Op1)) {
          if (auto L = dynamic_cast<Var *>(*Op0)) {
            if (L->V()->getType() != ty)
              continue;
            I = L;
            auto T = make_unique<ReservedConst>(L->V()->getType());
            J = T.get();
            RCs.insert(move(T));
          } else continue;
        }
        // (op var, var)
        else {
          if (auto L = dynamic_cast<Var *>(*Op0)) {
            if (auto R = dynamic_cast<Var *>(*Op1)) {
              if (L->V()->getType() != R->V()->getType())
                continue;
              if (L->V()->getType() != ty)
                continue;
            };
          };

          I = *Op0;
          J = *Op1;
        }
        BinOp::Op op = static_cast<BinOp::Op>(K);
        auto BO = make_unique<BinOp>(op, *I, *J);
        R.push_back(make_pair(move(BO), move(RCs)));
      }
    }
    // BinaryIntrinsics
    for (unsigned K = SIMDBinOp::Op::x86_avx2_packssdw; K <= SIMDBinOp::Op::x86_avx2_pshuf_b; ++K) {
      // typecheck for return val
      if (!ty->isVectorTy())
        continue;
      llvm::VectorType *vty = llvm::cast<llvm::FixedVectorType>(ty);
      SIMDBinOp::Op op = static_cast<SIMDBinOp::Op>(K);
      if (SIMDBinOp::binop_ret_v[op].first != vty->getNumElements())
        continue;
      if (SIMDBinOp::binop_ret_v[op].second != vty->getScalarSizeInBits())
        continue;

      for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
        for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
          Inst *I = nullptr, *J = nullptr;
          set<unique_ptr<ReservedConst>> RCs;

          if (auto L = dynamic_cast<Var *> (*Op0)) {
            // typecheck for op0
            if (!L->V()->getType()->isVectorTy())
              continue;
            llvm::VectorType *aty = llvm::cast<llvm::FixedVectorType>(L->V()->getType());
            if (SIMDBinOp::binop_op0_v[op].first  != aty->getNumElements())
              continue;
            if (SIMDBinOp::binop_op0_v[op].second != aty->getScalarSizeInBits()) {
              continue;
            }
          }
          if (auto R = dynamic_cast<Var *>(*Op1)) {
            // typecheck for op1
            if (!R->V()->getType()->isVectorTy())
              continue;
            llvm::VectorType *bty = llvm::cast<llvm::FixedVectorType>(R->V()->getType());
            if (SIMDBinOp::binop_op1_v[op].first  != bty->getNumElements())
              continue;
            if (SIMDBinOp::binop_op1_v[op].second != bty->getScalarSizeInBits())
              continue;
          }

          // (op rc, var)
          if (dynamic_cast<ReservedConst *>(*Op0)) {
            if (auto R = dynamic_cast<Var *>(*Op1)) {
              unsigned lanes = SIMDBinOp::binop_op0_v[op].first;
              unsigned bits = SIMDBinOp::binop_op0_v[op].second;
              auto aty = llvm::FixedVectorType::get(llvm::IntegerType::get(Ctx, bits), lanes);
              auto T = make_unique<ReservedConst>(aty);
              I = T.get();
              RCs.insert(move(T));
              J = R;
            } else continue;
          }
          // (op var, rc)
          else if (dynamic_cast<ReservedConst *>(*Op1)) {
            if (auto L = dynamic_cast<Var *>(*Op0)) {
              unsigned lanes = SIMDBinOp::binop_op1_v[op].first;
              unsigned bits = SIMDBinOp::binop_op1_v[op].second;
              auto bty = llvm::FixedVectorType::get(llvm::IntegerType::get(Ctx, bits), lanes);
              auto T = make_unique<ReservedConst>(bty);
              J = T.get();
              RCs.insert(move(T));
              I = L;
            } else continue;
          }
          // (op var, var)
          else {
            I = *Op0;
            J = *Op1;
          }

          auto BO = make_unique<BinIntr>(op, *I, *J);
          R.push_back(make_pair(move(BO), move(RCs)));
        }
      }
    }
  }

  // ShuffleVector
#if (false)
  for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
    for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
      if (!V->getType()->isVectorTy())
        continue;
      auto vty = llvm::cast<llvm::VectorType>(V->getType());

      Inst *I = nullptr, *J = nullptr;
      set<unique_ptr<ReservedConst>> RCs;
      // (shufflevecttor rc, var, mask)
      if (dynamic_cast<ReservedConst *>(*Op0)) {
        if (dynamic_cast<Var *>(*Op1))
          continue;
      }
        // (shufflevector var, rc, mask)
      else if (dynamic_cast<ReservedConst *>(*Op1)) {
        if (auto L = dynamic_cast<Var *>(*Op0)) {
          if (!L->V()->getType()->isVectorTy())
            continue;
          auto lvty = llvm::cast<llvm::VectorType>(L->V()->getType());
          if (lvty->getElementType() != vty->getElementType())
            continue;
          I = L;
          auto T = make_unique<ReservedConst>(L->V()->getType());
          J = T.get();
          RCs.insert(move(T));
        } else continue;
      }
      // (shufflevector, var, var, mask)
      else {
        if (auto L = dynamic_cast<Var *>(*Op0)) {
          if (auto R = dynamic_cast<Var *>(*Op1)) {
            if (L->V()->getType() != R->V()->getType())
              continue;
            if (!L->V()->getType()->isVectorTy())
              continue;
            auto lvty = llvm::cast<llvm::VectorType>(L->V()->getType());
            if (lvty->getElementType() != vty->getElementType())
              continue;
          };
        };
        I = *Op0;
        J = *Op1;
      }
      auto mty = llvm::VectorType::get(llvm::Type::getInt32Ty(V->getContext()), vty->getElementCount());
      auto mask = make_unique<ReservedConst>(mty);
      auto BO = make_unique<ShuffleVector>(*I, *J, *mask.get());
      RCs.insert(move(mask));
      R.push_back(make_pair(move(BO), move(RCs)));
    }
  }
#endif

  for (auto &I : Inputs) {
    if (I->V()->getType()  != V->getType())
      continue;
    set<unique_ptr<ReservedConst>> RCs;
    // TODO : reduce copy
    auto V = make_unique<Var>(I->V());
    R.push_back(make_pair(move(V), move(RCs)));
  }

  return true;
}

static optional<smt::smt_initializer> smt_init;
static bool compareFunctions(IR::Function &Func1, IR::Function &Func2,
                             unsigned &goodCount,
                             unsigned &badCount, unsigned &errorCount) {
  TransformPrintOpts print_opts;
  smt_init->reset();
  Transform t;
  t.src = move(Func1);
  t.tgt = move(Func2);
  TransformVerify verifier(t, false);
  t.print(cout, print_opts);

  {
    auto types = verifier.getTypings();
    if (!types) {
      cerr << "Transformation doesn't verify!\n"
              "ERROR: program doesn't type check!\n\n";
      ++errorCount;
      return true;
    }
    assert(types.hasSingleTyping());
  }

  Errors errs = verifier.verify();
  bool result(errs);
  if (result) {
    if (errs.isUnsound()) {
      cerr << "Transformation doesn't verify!\n" << errs << endl;
      ++badCount;
    } else {
      cerr << errs << endl;
      ++errorCount;
    }
  } else {
    cerr << "Transformation seems to be correct!\n\n";
    ++goodCount;
  }

  return result;
}
static bool constantSynthesis(IR::Function &Func1, IR::Function &Func2,
                              unsigned &goodCount,
                              unsigned &badCount, unsigned &errorCount,
                              unordered_map<Input *, llvm::Argument *> &inputMap,
                              unordered_map<llvm::Argument *, llvm::Constant *> &constMap) {
  TransformPrintOpts print_opts;
  smt_init->reset();
  Transform t;
  t.src = move(Func1);
  t.tgt = move(Func2);
  vectorsynth::ConstantSynth verifier(t, false);
  t.print(cout, print_opts);
  // assume type verifies
  std::unordered_map<const IR::Input *, smt::expr> result;
  Errors errs = verifier.synthesize(result);

  bool ret(errs);
  if (result.empty())
    return ret;

  for (auto p : inputMap) {
    auto &ty = p.first->getType();
    auto lty = p.second->getType();

    if (ty.isIntType()) {
      // TODO, fix, do not use numeral_string()
      constMap[p.second] = llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(lty), result[p.first].numeral_string(), 10);
    } else if (ty.isFloatType()) {
      //TODO
      UNREACHABLE();
    } else if (ty.isVectorType()) {
      auto trunk = result[p.first];
      llvm::FixedVectorType *vty = llvm::cast<llvm::FixedVectorType>(lty);
      llvm::IntegerType *ety = llvm::cast<llvm::IntegerType>(vty->getElementType());
      vector<llvm::Constant *> v;
      for (int i = vty->getNumElements() - 1 ; i >= 0 ; i --) {
        unsigned bits = ety->getBitWidth();
        auto elem = trunk.extract((i + 1) * bits - 1, i * bits);
        // TODO: support undef
        if (!elem.isConst())
          return ret;
        v.push_back(llvm::ConstantInt::get(ety, elem.numeral_string(), 10));
      }
      constMap[p.second] = llvm::ConstantVector::get(v);
    }
  }

  goodCount++;

  return ret;
}

static set<llvm::Function *> IntrinsicDecls;
static llvm::Value *codeGen(Inst *I, llvm::IRBuilder<> &b,
                            llvm::ValueToValueMapTy &VMap, llvm::Function &F,
                            unordered_map<llvm::Argument *, llvm::Constant *> *constMap) {
  if (auto V = dynamic_cast<Var *>(I)) {
    if (VMap.empty()) {
      return V->V();
    } else {
      return VMap[V->V()];
    }
  } else if (auto B = dynamic_cast<BinOp *>(I)) {
    auto op0 = codeGen(B->L(), b, VMap, F, constMap);
    auto op1 = codeGen(B->R(), b, VMap, F, constMap);
    llvm::Value *r = nullptr;
    switch (B->K()) {
    case BinOp::band:
      r = b.CreateAnd(op0, op1, "and");
      break;
    case BinOp::bor:
      r = b.CreateOr(op0, op1, "or");
      break;
    case BinOp::bxor:
      r = b.CreateXor(op0, op1, "xor");
      break;
    case BinOp::add:
      r = b.CreateAdd(op0, op1, "add");
      break;
    case BinOp::sub:
      r = b.CreateSub(op0, op1, "sub");
      break;
    case BinOp::mul:
      r = b.CreateMul(op0, op1, "mul");
      break;
    default:
      UNREACHABLE();
    }
    return r;
  } else if (auto B = dynamic_cast<BinIntr *>(I)) {
    auto op0 = codeGen(B->L(), b, VMap, F, constMap);
    auto op1 = codeGen(B->R(), b, VMap, F, constMap);
    llvm::Function *Intr = nullptr;
    llvm::Module *M = F.getParent();
    switch (B->K()) {
    case SIMDBinOp::Op::x86_avx2_packssdw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_packssdw);
      break;
    case SIMDBinOp::Op::x86_avx2_packsswb:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_packsswb);
      break;
    case SIMDBinOp::Op::x86_avx2_packusdw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_packusdw);
      break;
    case SIMDBinOp::Op::x86_avx2_packuswb:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_packuswb);
      break;
    case SIMDBinOp::Op::x86_avx2_pavg_b:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pavg_b);
      break;
    case SIMDBinOp::Op::x86_avx2_pavg_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pavg_w);
      break;
    case SIMDBinOp::Op::x86_avx2_phadd_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phadd_d);
      break;
    case SIMDBinOp::Op::x86_avx2_phadd_sw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phadd_sw);
      break;
    case SIMDBinOp::Op::x86_avx2_phadd_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phadd_w);
      break;
    case SIMDBinOp::Op::x86_avx2_phsub_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phsub_d);
      break;
    case SIMDBinOp::Op::x86_avx2_phsub_sw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phsub_sw);
      break;
    case SIMDBinOp::Op::x86_avx2_phsub_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_phsub_w);
      break;
    case SIMDBinOp::Op::x86_avx2_pmadd_ub_sw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pmadd_ub_sw);
      break;
    case SIMDBinOp::Op::x86_avx2_pmadd_wd:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pmadd_wd);
      break;
    case SIMDBinOp::Op::x86_avx2_pmul_hr_sw:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pmul_hr_sw);
      break;
    case SIMDBinOp::Op::x86_avx2_pmulh_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pmulh_w);
      break;
    case SIMDBinOp::Op::x86_avx2_pmulhu_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pmulhu_w);
      break;
    case SIMDBinOp::Op::x86_avx2_psign_b:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psign_b);
      break;
    case SIMDBinOp::Op::x86_avx2_psign_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psign_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psign_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psign_w);
      break;
    case SIMDBinOp::Op::x86_avx2_psll_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psll_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psll_q:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psll_q);
      break;
    case SIMDBinOp::Op::x86_avx2_psll_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psll_w);
      break;
    case SIMDBinOp::Op::x86_avx2_psllv_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psllv_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psllv_d_256:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psllv_d_256);
      break;
    case SIMDBinOp::Op::x86_avx2_psllv_q:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psllv_q);
      break;
    case SIMDBinOp::Op::x86_avx2_psllv_q_256:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psllv_q_256);
      break;
    case SIMDBinOp::Op::x86_avx2_psrav_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrav_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psrav_d_256:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrav_d_256);
      break;
    case SIMDBinOp::Op::x86_avx2_psrl_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrav_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psrl_q:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrl_q);
      break;
    case SIMDBinOp::Op::x86_avx2_psrl_w:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrl_w);
      break;
    case SIMDBinOp::Op::x86_avx2_psrlv_d:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrlv_d);
      break;
    case SIMDBinOp::Op::x86_avx2_psrlv_d_256:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrlv_d_256);
      break;
    case SIMDBinOp::Op::x86_avx2_psrlv_q:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrlv_q);
      break;
    case SIMDBinOp::Op::x86_avx2_psrlv_q_256:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_psrlv_q_256);
      break;
    case SIMDBinOp::Op::x86_avx2_pshuf_b:
      Intr = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::x86_avx2_pshuf_b);
      break;
    default:
      UNREACHABLE();
    }
    IntrinsicDecls.insert(Intr);
    return llvm::CallInst::Create(Intr, llvm::ArrayRef<llvm::Value *>({op0, op1}), "intr", llvm::cast<llvm::Instruction>(b.GetInsertPoint()));
  } else if (auto RC = dynamic_cast<ReservedConst *>(I)) {
    if (!constMap) {
      return RC->getA();
    } else {
      return (*constMap)[RC->getA()];
    }
  } else if (auto SV = dynamic_cast<ShuffleVector *>(I)) {
    // TODO
    (void) SV;
#if (false)
    auto op0 = codeGen(SV->L(), b, VMap, F, constMap);
    auto op1 = codeGen(SV->R(), b, VMap, F, constMap);
    auto M = codeGen(SV->M(), b, VMap, F, constMap);
    op0->dump();
    op1->dump();
    M->dump();
    return b.CreateShuffleVector(op0, op1, M);
#endif
    return nullptr;
  }
  return nullptr;
}

static void removeUnusedDecls () {
  for (auto Intr : IntrinsicDecls) {
    if (Intr->isDeclaration() && Intr->use_empty()) {
      Intr->eraseFromParent();
    }
  }
  IntrinsicDecls.clear();
}


  //void optimizeFunction(llvm::Function &F) {
static void DCE(llvm::Function &F) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager FPM;
  /*    PB.buildFunctionSimplificationPipeline(llvm::PassBuilder::OptimizationLevel::O0,
        llvm::PassBuilder::ThinLTOPhase::None);*/
  FPM.addPass(llvm::DCEPass());
  FPM.run(F, FAM);
}

bool synthesize (llvm::Function &F1, llvm::TargetLibraryInfo *TLI) {
  config::disable_undef_input = true;
  config::disable_poison_input = true;

  smt_init.emplace();
  Inst *R = nullptr;
  bool result = false;

  for (auto &BB : F1) {
    for (llvm::BasicBlock::reverse_iterator I = BB.rbegin(), E = BB.rend(); I != E; I++) {
      unordered_map<llvm::Argument *, llvm::Constant *> constMap;
      unordered_map<Input *, llvm::Argument *> inputMap;
      set<unique_ptr<Var>> Inputs;
      findInputs(&*I, Inputs, 20);

      vector<pair<unique_ptr<Inst>,set<unique_ptr<ReservedConst>>>> Sketches;
      getSketches(Inputs, &*I, Sketches);

      struct Comparator {
        bool operator()(tuple<llvm::Function *, Inst *, bool>& p1, tuple<llvm::Function *, Inst *, bool> &p2) {
          return get<0>(p1)->getInstructionCount() > get<0>(p2)->getInstructionCount();
        }
      };
      priority_queue<tuple<llvm::Function *, Inst *, bool>, vector<tuple<llvm::Function *, Inst *, bool>>, Comparator> Fns;
      // Fast Codegen
      for (auto &Sketch : Sketches) {
        auto &G = Sketch.first;
        llvm::ValueToValueMapTy VMap;
        //auto GF = llvm::CloneFunction(&F1, VMap, 0);
        auto FT = F1.getFunctionType();

        llvm::SmallVector<llvm::Type *, 8> Args;
        for (auto I: FT->params()) {
          Args.push_back(I);
        }
        for (auto &C : Sketch.second) {
          Args.push_back(C->T());
        }

        auto nFT = llvm::FunctionType::get(FT->getReturnType(), Args, FT->isVarArg());

        auto GF = llvm::Function::Create(nFT, F1.getLinkage(), F1.getName(), F1.getParent());
        llvm::SmallVector<llvm::ReturnInst *, 8> returns;

        llvm::Function::arg_iterator GI = GF->arg_begin();

        for (llvm::Function::arg_iterator I = F1.arg_begin(), E = F1.arg_end(); I != E; ++I) {
          VMap[I] = GI;
          GI->setName(I->getName());
          GI ++;
        }
        unsigned CI = 0;
        for (auto &C : Sketch.second) {
          GI->setName("_reservedc_" + std::to_string(CI));
          C->setA(GI);
          CI ++;
        }

        llvm::CloneFunctionInto(GF, &F1, VMap, false, returns);

        llvm::Instruction *PrevI = llvm::cast<llvm::Instruction>(VMap[&*I]);
        llvm::IRBuilder<> builder(PrevI);
        llvm::Value *V = codeGen(G.get(), builder, VMap, F1, nullptr);
        PrevI->replaceAllUsesWith(V);

        DCE(*GF);
        if (GF->getInstructionCount() >= F1.getInstructionCount()) {
          GF->eraseFromParent();
          continue;
        }

        Fns.push(make_tuple(GF, G.get(), !Sketch.second.empty()));
      }

      while (!Fns.empty()) {
        auto [GF, G, HaveC] = Fns.top();
        Fns.pop();
        auto Func1 = llvm2alive(F1, *TLI);
        unsigned goodCount = 0, badCount = 0, errorCount = 0;
        if (!HaveC) {
          auto Func2 = llvm2alive(*GF, *TLI);
          result |= compareFunctions(*Func1, *Func2, goodCount, badCount, errorCount);
        } else {
          inputMap.clear();
          constMap.clear();
          auto Func2 = llvm2aliveCM(*GF, *TLI, {}, &inputMap);
          result |= constantSynthesis(*Func1, *Func2, goodCount, badCount, errorCount, inputMap, constMap);
        }
        GF->eraseFromParent();
        if (goodCount) {
          R = G;
          break;
        }
      }
      while (!Fns.empty()) {
        auto [GF, G, HaveC] = Fns.top();
        Fns.pop();
        (void) G; (void) HaveC;
        GF->eraseFromParent();
      }
      if (R) {
        // replace on original function
        llvm::IRBuilder<> builder(&*I);
        llvm::ValueToValueMapTy VMap;
        auto V = codeGen(R, builder, VMap, F1, &constMap);
        I->replaceAllUsesWith(V);
        DCE(F1);
        removeUnusedDecls();
        return true;
      }
    }
  }
  removeUnusedDecls();
  return false;
}



};
