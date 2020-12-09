#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "ir/function.h"
#include <memory>
#include <utility>
#include <vector>

namespace llvm {
class CallInst;
class TargetLibraryInfo;
}

namespace IR {
class BasicBlock;
class Instr;
class Value;
}

namespace vectorsynth {

// returned bool indicates whether it's a known function call
std::tuple<std::unique_ptr<IR::Instr>, IR::FnAttrs, std::vector<IR::ParamAttrs>,
           bool>
known_call(llvm::CallInst &i, const llvm::TargetLibraryInfo &TLI,
           IR::BasicBlock &BB, const std::vector<IR::Value*> &args);

}
