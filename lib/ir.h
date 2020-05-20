#pragma once


#include "llvm/IR/IRBuilder.h"
#include "ir/simd.h"
#include <vector>

namespace syn {
class Inst {
public:
  virtual void print(std::ostream &os) const = 0;
  virtual ~Inst() {}
};


class Comp : public Inst {
public:
};

class Var final : public Comp {
  llvm::Value *v;
public:
  Var(llvm::Value *v) : v(v) {}
  void print(std::ostream &os) const override;
  llvm::Value *V () { return v; }
};

class ReservedConst final : public Comp {
  // type?
  llvm::Type *type;
  llvm::Argument *A;
public:
  ReservedConst(llvm::Type *t) : type(t) {}
  void print(std::ostream &os) const override;
  llvm::Type *T () { return type; }
  llvm::Argument *getA () { return A; }
  void setA (llvm::Argument *Arg) { A = Arg; }
};

class BinOp final : public Inst {
public:
  enum Op { band, bor, bxor, add, sub, mul};
private:
  Op op;
  Inst* lhs;
  Inst* rhs;
public:
  BinOp(Op op, Inst &lhs, Inst &rhs) : op(op), lhs(&lhs), rhs(&rhs) {}
  void print(std::ostream &os) const override;
  Inst *L() { return lhs; }
  Inst *R() { return rhs; }
  Op K() { return op; }
};

class BinIntr final : public Inst {
private:
  IR::SIMDBinOp::Op op;
  Inst* lhs;
  Inst* rhs;
public:
  BinIntr(IR::SIMDBinOp::Op op, Inst &lhs, Inst &rhs) : op(op), lhs(&lhs), rhs(&rhs) {}
  void print(std::ostream &os) const override;
  Inst *L() { return lhs; }
  Inst *R() { return rhs; }
  IR::SIMDBinOp::Op K() { return op; }
};

class Hole : Inst {
};
};
