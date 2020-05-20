#pragma once

#include "ir/function.h"

namespace llvm {
class DataLayout;
class Function;
class TargetLibraryInfo;
class Triple;
}

namespace syn {
bool synthesize (llvm::Function &F1, llvm::TargetLibraryInfo *TLI);
};
