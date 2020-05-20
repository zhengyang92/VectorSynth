// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.

#pragma once

#include "ir/function.h"
#include "tools/transform.h"

namespace vectorsynth {

class ConstantSynth {
  tools::Transform &t;
  std::unordered_map<std::string, const IR::Instr*> tgt_instrs;
  bool check_each_var;

public:
  ConstantSynth(tools::Transform &t, bool check_each_var);
  util::Errors synthesize(std::unordered_map<const IR::Input*, smt::expr> &result) const;
};

}
