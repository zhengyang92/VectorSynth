#include "synthesis/ir.h"
#include <string>

using namespace std;

namespace syn {

void Var::print(ostream &os) const {
  const char *str = "val ";

  //  os << "sv" << " = " << str;
  os << str;
}

void ReservedConst::print(ostream &os) const {
  const char *str = "reservedconst ";

  //  os << "sv" << " = " << str;
  os << str;
}

void BinOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case band:       str = "and "; break;
  case bor:        str = "or ";  break;
  case bxor:       str = "xor "; break;
  case add:        str = "add "; break;
  case sub:        str = "sub "; break;
  case mul:        str = "mul "; break;
  }

  //  os << "sv" << " = " << str;
  os << str;
}

void BinIntr::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case IR::SIMDBinOp::Op::x86_avx2_pshuf_b:     str = "v.avx2.pshufb "; break;
  default:                                      str = "(null)"        ; break;
  }

  //  os << "sv" << " = " << str;
  os << str;
}

};
