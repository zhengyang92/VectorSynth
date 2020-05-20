// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.

#include "constantsynth.h"
#include "ir/globals.h"
#include "smt/smt.h"
#include "util/config.h"
#include "util/errors.h"
#include "util/symexec.h"
#include <map>
#include <sstream>

using namespace IR;
using namespace smt;
using namespace tools;
using namespace util;
using namespace std;

// borrowed from alive2
static expr preprocess(Transform &t, const set<expr> &qvars0,
                       const set<expr> &undef_qvars, expr && e) {

  if (hit_half_memory_limit())
    return expr::mkForAll(qvars0, move(e));


  // TODO: benchmark
  if (0) {
    expr var = expr::mkBoolVar("malloc_never_fails");
    e = expr::mkIf(var,
                   e.subst(var, true).simplify(),
                   e.subst(var, false).simplify());
  }

  // eliminate all quantified boolean vars; Z3 gets too slow with those
  auto qvars = qvars0;
  for (auto &var : qvars0) {
    if (!var.isBool())
      continue;
    e = e.subst(var, true).simplify() &&
      e.subst(var, false).simplify();
    qvars.erase(var);
  }
  // TODO: maybe try to instantiate undet_xx vars?
  if (undef_qvars.empty() || hit_half_memory_limit())
    return expr::mkForAll(qvars, move(e));

  // manually instantiate all ty_%v vars
  map<expr, expr> instances({ { move(e), true } });
  map<expr, expr> instances2;

  expr nums[3] = { expr::mkUInt(0, 2), expr::mkUInt(1, 2), expr::mkUInt(2, 2) };

  for (auto &i : t.src.getInputs()) {
    auto in = dynamic_cast<const Input*>(&i);
    if (!in)
      continue;
    auto var = in->getTyVar();

    for (auto &[e, v] : instances) {
      for (unsigned i = 0; i <= 2; ++i) {
        if (config::disable_undef_input && i == 1)
          continue;
        if (config::disable_poison_input && i == 2)
          continue;

        expr newexpr = e.subst(var, nums[i]);
        if (newexpr.eq(e)) {
          instances2[move(newexpr)] = v;
          break;
        }

        newexpr = newexpr.simplify();
        if (newexpr.isFalse())
          continue;

        // keep 'var' variables for counterexample printing
        instances2.try_emplace(move(newexpr), v && var == nums[i]);
      }
    }
    instances = move(instances2);

    // Bail out if it gets too big. It's very likely we can't solve it anyway.
    if (instances.size() >= 128 || hit_half_memory_limit())
      break;
  }
  expr insts(false);
  for (auto &[e, v] : instances) {
    insts |= expr::mkForAll(qvars, move(const_cast<expr&>(e))) && v;
  }

  // TODO: try out instantiating the undefs in forall quantifier

  return insts;
}

static bool is_undef(const expr &e) {
  if (e.isConst())
    return false;
  return check_expr(expr::mkForAll(e.vars(), expr::mkVar("#undef", e) != e)).
           isUnsat();
}

static void print_single_varval(ostream &os, State &st, const Model &m,
                                const Value *var, const Type &type,
                                const StateValue &val) {
  if (!val.isValid()) {
    os << "(invalid expr)";
    return;
  }

  // if the model is partial, we don't know for sure if it's poison or not
  // this happens if the poison constraint depends on an undef
  // however, cexs are usually triggered by the worst case, which is poison
  if (auto v = m.eval(val.non_poison);
      (!v.isConst() || v.isFalse())) {
    os << "poison";
    return;
  }

  if (auto *in = dynamic_cast<const Input*>(var)) {
    uint64_t n;
    ENSURE(m[in->getTyVar()].isUInt(n));
    if (n == 1) {
      os << "undef";
      return;
    }
    assert(n == 0);
  }

  expr partial = m.eval(val.value);
  if (is_undef(partial)) {
    os << "undef";
    return;
  }

  type.printVal(os, st, m.eval(val.value, true));

  // undef variables may not have a model since each read uses a copy
  // TODO: add intervals of possible values for ints at least?
  if (!partial.isConst()) {
    // some functions / vars may not have an interpretation because it's not
    // needed, not because it's undef
    bool found_undef = false;
    for (auto &var : partial.vars()) {
      if ((found_undef = isUndef(var)))
        break;
    }
    if (found_undef)
      os << "\t[based on undef value]";
  }
}

static void print_varval(ostream &os, State &st, const Model &m,
                         const Value *var, const Type &type,
                         const StateValue &val) {
  if (!type.isAggregateType()) {
    print_single_varval(os, st, m, var, type, val);
    return;
  }

  os << (type.isStructType() ? "{ " : "< ");
  auto agg = type.getAsAggregateType();
  for (unsigned i = 0, e = agg->numElementsConst(); i < e; ++i) {
    if (i != 0)
      os << ", ";
    print_varval(os, st, m, var, agg->getChild(i), agg->extract(val, i));
  }
  os << (type.isStructType() ? " }" : " >");
}

using print_var_val_ty = function<void(ostream&, const Model&)>;

static void error(Errors &errs, State &src_state, State &tgt_state,
                  const Result &r, const Value *var,
                  const char *msg, bool check_each_var,
                  print_var_val_ty print_var_val) {

  if (r.isInvalid()) {
    errs.add("Invalid expr", false);
    return;
  }

  if (r.isTimeout()) {
    errs.add("Timeout", false);
    return;
  }

  if (r.isError()) {
    errs.add("SMT Error: " + r.getReason(), false);
    return;
  }

  if (r.isSkip()) {
    errs.add("Skip", false);
    return;
  }

  stringstream s;
  string empty;
  auto &var_name = var ? var->getName() : empty;
  auto &m = r.getModel();

  s << msg;
  if (!var_name.empty())
    s << " for " << *var;
  s << "\n\nExample:\n";

  for (auto &[var, val, used] : src_state.getValues()) {
    (void)used;
    if (!dynamic_cast<const Input*>(var) &&
        !dynamic_cast<const ConstantInput*>(var))
      continue;
    s << *var << " = ";
    print_varval(s, src_state, m, var, var->getType(), val.first);
    s << '\n';
  }

  set<string> seen_vars;
  for (auto st : { &src_state, &tgt_state }) {
    if (!check_each_var) {
      if (st->isSource()) {
        s << "\nSource:\n";
      } else {
        s << "\nTarget:\n";
      }
    }

    for (auto &[var, val, used] : st->getValues()) {
      (void)used;
      auto &name = var->getName();
      if (name == var_name)
        break;

      if (name[0] != '%' ||
          dynamic_cast<const Input*>(var) ||
          (check_each_var && !seen_vars.insert(name).second))
        continue;

      s << *var << " = ";
      print_varval(s, const_cast<State&>(*st), m, var, var->getType(),
                   val.first);
      s << '\n';
    }

    st->getMemory().print(s, m);
  }

  print_var_val(s, m);
  errs.add(s.str(), true);
}

namespace vectorsynth {

ConstantSynth::ConstantSynth(Transform &t, bool check_each_var) :
  t(t), check_each_var(check_each_var) {
  if (check_each_var) {
    for (auto &i : t.tgt.instrs()) {
      tgt_instrs.emplace(i.getName(), &i);
    }
  }
}

Errors ConstantSynth::synthesize(unordered_map<const Input*, expr> &result) const {
  //  calculateAndInitConstants(t);
  State::resetGlobals();
  IR::State src_state(t.src, true);
  util::sym_exec(src_state);
  IR::State tgt_state(t.tgt, false);
  util::sym_exec(tgt_state);
  auto pre_src_and = src_state.getPre();
  auto &pre_tgt_and = tgt_state.getPre();

  // optimization: rewrite "tgt /\ (src -> foo)" to "tgt /\ foo" if src = tgt
  pre_src_and.del(pre_tgt_and);
  expr pre_src = pre_src_and();
  expr pre_tgt = pre_tgt_and();
  expr axioms_expr = expr(true);

  IR::State::ValTy sv = src_state.returnVal(), tv = tgt_state.returnVal();

  auto uvars = sv.second;
  set<expr> qvars;

  Errors errs;

  for (auto &[var, val, used] : src_state.getValues()) {
    (void)used;
    if (!dynamic_cast<const Input*>(var))
      continue;

    auto &ty = var->getType();

    if (ty.isIntType()) {
      qvars.insert(val.first.value);
      continue;
    }

    if (ty.isVectorType() && ty.getAsAggregateType()->getChild(0).isIntType()) {
      auto aty = ty.getAsAggregateType();
      for (unsigned I = 0; I < aty->numElementsConst(); ++I) {
        qvars.insert(aty->extract(val.first, I, false).value);
      }
      continue;
    }

    config::dbg()<<"constant synthesizer now only supports synthesizing integers and vector of integers"<<std::endl;
    return errs;
  }
  /*
  for (auto e : src_state.getForAlls()) {
    qvars.insert(e);
    }*/

  auto dom_a = src_state.returnDomain()();
  auto dom_b = tgt_state.returnDomain()();

  expr dom = dom_a && dom_b;

  auto mk_fml = [&](expr &&refines) -> expr {
    // from the check above we already know that
    // \exists v,v' . pre_tgt(v') && pre_src(v) is SAT (or timeout)
    // so \forall v . pre_tgt && (!pre_src(v) || refines) simplifies to:
    // (pre_tgt && !pre_src) || (!pre_src && false) ->   [assume refines=false]
    // \forall v . (pre_tgt && !pre_src(v)) ->  [\exists v . pre_src(v)]
    // false
    if (refines.isFalse())
      return move(refines);

    auto fml = pre_tgt && pre_src.implies(refines);
    return axioms_expr && preprocess(t, qvars, uvars, move(fml));
  };


  const Type &ty = t.src.getType();
  auto [poison_cnstr, value_cnstr] = ty.refines(src_state, tgt_state, sv.first, tv.first);
  if (config::debug) {
    config::dbg()<<"SV"<<std::endl;
    config::dbg()<<sv.first<<std::endl;
    config::dbg()<<"TV"<<std::endl;
    config::dbg()<<tv.first<<std::endl;
    config::dbg()<<"Value Constraints"<<std::endl;
    config::dbg()<<value_cnstr<<std::endl;
    config::dbg()<<"Poison Constraints"<<std::endl;
    config::dbg()<<poison_cnstr<<std::endl;
  }

  const Value *var = nullptr;
  bool check_each_var = false;

  auto err = [&](const Result &r, print_var_val_ty print, const char *msg) {
    error(errs, src_state, tgt_state, r, var, msg, check_each_var, print);
  };

  Solver::check({
      { mk_fml(dom_a.notImplies(dom_b)),
          [&](const Result &r) {
          err(r, [](ostream&, const Model&){},
              "Source is more defined than target");
      }},
      { mk_fml(dom && value_cnstr && poison_cnstr),
          [&](const Result &r) {
          if (r.isInvalid()) {
            errs.add("Invalid expr", false);
            return;
          }

          if (r.isTimeout()) {
            errs.add("Timeout", false);
            return;
          }

          if (r.isError()) {
            errs.add("SMT Error: " + r.getReason(), false);
            return;
          }

          if (r.isSkip()) {
            errs.add("Skip", false);
            return;
          }

          if (r.isUnsat()) {
            errs.add("Unsat", false);
            return;
          }


          stringstream s;
          auto &m = r.getModel();
          s << ";result\n";
          for (auto &[var, val, used] : tgt_state.getValues()) {
            (void)used;
            if (!dynamic_cast<const Input*>(var) &&
                !dynamic_cast<const ConstantInput*>(var))
                continue;

            if (var->getName().rfind("%_reservedc", 0) == 0) {
              auto In = static_cast<const Input *>(var);
              result[In] = m.eval(val.first.value);
              s << *var << " = ";
              print_varval(s, src_state, m, var, var->getType(), val.first);
              s << '\n';
            }
          }
          config::dbg()<<s.str();
      }}
    });
  return errs;
}

}
