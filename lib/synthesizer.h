// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.

#pragma once

#include "ir/function.h"

namespace llvm {
class DataLayout;
class Function;
class TargetLibraryInfo;
class Triple;
}

namespace vectorsynth {
bool synthesize (llvm::Function &F1, llvm::TargetLibraryInfo *TLI);
};
