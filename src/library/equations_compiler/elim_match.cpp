/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <string>
#include "util/flet.h"
#include "util/sexpr/option_declarations.h"
#include "kernel/instantiate.h"
#include "kernel/for_each_fn.h"
#include "kernel/replace_fn.h"
#include "kernel/inductive/inductive.h"
#include "library/trace.h"
#include "library/num.h"
#include "library/string.h"
#include "library/pp_options.h"
#include "library/exception.h"
#include "library/util.h"
#include "library/locals.h"
#include "library/annotation.h"
#include "library/private.h"
#include "library/inverse.h"
#include "library/aux_definition.h"
#include "library/app_builder.h"
#include "library/delayed_abstraction.h"
#include "library/tactic/tactic_state.h"
#include "library/tactic/revert_tactic.h"
#include "library/tactic/clear_tactic.h"
#include "library/tactic/cases_tactic.h"
#include "library/tactic/intro_tactic.h"
#include "library/equations_compiler/equations.h"
#include "library/equations_compiler/util.h"
#include "library/equations_compiler/elim_match.h"

#ifndef LEAN_DEFAULT_EQN_COMPILER_ITE
#define LEAN_DEFAULT_EQN_COMPILER_ITE true
#endif

namespace lean {
static name * g_eqn_compiler_ite = nullptr;

static bool get_eqn_compiler_ite(options const & o) {
    return o.get_bool(*g_eqn_compiler_ite, LEAN_DEFAULT_EQN_COMPILER_ITE);
}

#define trace_match(Code) lean_trace(name({"eqn_compiler", "elim_match"}), Code)
#define trace_match_debug(Code) lean_trace(name({"debug", "eqn_compiler", "elim_match"}), Code)

struct elim_match_fn {
    environment     m_env;
    options         m_opts;
    metavar_context m_mctx;

    expr            m_ref;
    unsigned        m_depth{0};
    buffer<bool>    m_used_eqns;
    bool            m_aux_lemmas;
    bool            m_use_ite;
    /* m_enum is a mapping from inductive type name to flag indicating whether it is
       an enumeration type or not. */
    name_map<bool>  m_enum;

    elim_match_fn(environment const & env, options const & opts,
                  metavar_context const & mctx):
        m_env(env), m_opts(opts), m_mctx(mctx) {
        m_use_ite = get_eqn_compiler_ite(opts);
    }

    struct equation {
        local_context  m_lctx;
        list<expr>     m_patterns;
        expr           m_rhs;
        /* m_renames map variables in this->m_lctx to problem local context */
        hsubstitution  m_subst;
        expr           m_ref; /* for producing error messages */
        unsigned       m_eqn_idx; /* for producing error message */
        /* The following fields are only used for lemma generation */
        list<expr>     m_hs;
        list<expr>     m_vars;
        list<expr>     m_lhs_args;
    };

    struct problem {
        name           m_fn_name;
        expr           m_goal;
        list<expr>     m_var_stack;
        list<equation> m_equations;
    };

    struct lemma {
        local_context  m_lctx;
        list<expr>     m_vars;
        list<expr>     m_hs;
        list<expr>     m_lhs_args;
        expr           m_rhs;
        unsigned       m_eqn_idx;
    };

    [[ noreturn ]] void throw_error(char const * msg) {
        throw generic_exception(m_ref, msg);
    }

    [[ noreturn ]] void throw_error(sstream const & strm) {
        throw generic_exception(m_ref, strm);
    }

    local_context get_local_context(expr const & mvar) {
        return m_mctx.get_metavar_decl(mvar)->get_context();
    }

    local_context get_local_context(problem const & P) {
        return get_local_context(P.m_goal);
    }

    /* (for debugging, i.e., writing assertions) make sure all locals
       in `e` are defined in `lctx` */
    bool check_locals_decl_at(expr const & e, local_context const & lctx) {
        for_each(e, [&](expr const & e, unsigned) {
                if (is_local(e) && !lctx.get_local_decl(e)) {
                    lean_unreachable();
                }
                return true;
            });
        return true;
    }

    /* For debugging */
    bool check_equation(problem const & P, equation const & eqn) {
        lean_assert(length(eqn.m_patterns) == length(P.m_var_stack));
        local_context const & lctx = get_local_context(P);
        eqn.m_subst.for_each([&](name const & n, expr const & e) {
                if (!eqn.m_lctx.get_local_decl(n)) {
                    lean_unreachable();
                }
                if (!check_locals_decl_at(e, lctx)) {
                    lean_unreachable();
                }
            });
        lean_assert(check_locals_decl_at(eqn.m_rhs, eqn.m_lctx));
        for (expr const & p : eqn.m_patterns) {
            if (!check_locals_decl_at(p, eqn.m_lctx)) {
                lean_unreachable();
            }
        }
        return true;
    }

    /* For debugging */
    bool check_problem(problem const & P) {
        local_context const & lctx = get_local_context(P);
        for (expr const & x : P.m_var_stack) {
            if (!check_locals_decl_at(x, lctx)) {
                lean_unreachable();
            }
        }
        for (equation const & eqn : P.m_equations) {
            if (!check_equation(P, eqn)) {
                lean_unreachable();
            }
        }
        return true;
    }

    type_context mk_type_context(local_context const & lctx) {
        return mk_type_context_for(m_env, m_opts, m_mctx, lctx);
    }

    type_context mk_type_context(expr const & mvar) {
        return mk_type_context(get_local_context(mvar));
    }

    type_context mk_type_context(problem const & P) {
        return mk_type_context(get_local_context(P));
    }

    std::function<format(expr const &)> mk_pp_ctx(local_context const & lctx) {
        options opts = m_opts.update(get_pp_beta_name(), false);
        type_context ctx = mk_type_context_for(m_env, opts, m_mctx, lctx);
        return ::lean::mk_pp_ctx(ctx);
    }

    std::function<format(expr const &)> mk_pp_ctx(problem const & P) {
        return mk_pp_ctx(get_local_context(P));
    }

    format nest(format const & fmt) const {
        return ::lean::nest(get_pp_indent(m_opts), fmt);
    }

    format pp_equation(equation const & eqn) {
        format r;
        auto pp = mk_pp_ctx(eqn.m_lctx);
        bool first = true;
        for (expr const & p : eqn.m_patterns) {
            if (first) first = false; else r += format(" ");
            r += paren(pp(p));
        }
        r += space() + format(":=") + nest(line() + pp(eqn.m_rhs));
        return group(r);
    }

    format pp_problem(problem const & P) {
        format r;
        auto pp = mk_pp_ctx(P);
        r += format("match") + space() + format(P.m_fn_name) + space();
        format v;
        bool first = true;
        for (expr const & x : P.m_var_stack) {
            if (first) first = false; else v += comma() + space();
            v += pp(x);
        }
        r += bracket("[", v, "]");
        for (equation const & eqn : P.m_equations) {
            r += nest(line() + pp_equation(eqn));
        }
        return r;
    }

    optional<name> is_constructor(name const & n) const { return inductive::is_intro_rule(m_env, n); }
    optional<name> is_constructor(expr const & e) const {
        if (!is_constant(e)) return optional<name>();
        return is_constructor(const_name(e));
    }
    optional<name> is_constructor_app(expr const & e) const { return is_constructor(get_app_fn(e)); }

    bool is_inductive(name const & n) const { return static_cast<bool>(inductive::is_inductive_decl(m_env, n)); }
    bool is_inductive(expr const & e) const { return is_constant(e) && is_inductive(const_name(e)); }
    bool is_inductive_app(expr const & e) const { return is_inductive(get_app_fn(e)); }

    /* Return true iff `e` is of the form (I.below ...) or (I.ibelow ...) where `I` is an inductive datatype.
       Move to a different module? */
    bool is_below_type(expr const & e) const {
        expr const & fn = get_app_fn(e);
        if (!is_constant(fn)) return false;
        name const & fn_name = const_name(fn);
        if (fn_name.is_atomic() || !fn_name.is_string()) return false;
        std::string s(fn_name.get_string());
        return is_inductive(fn_name.get_prefix()) && (s == "below" || s == "ibelow");
    }

    /* Return true iff I_name is an enumeration type with more than 2 elements.

       \remark It is not worth to apply the if-then-else compilation step if the enumeration
       types has only 2 (or 1) element. */
    bool is_nontrivial_enum(name const & I_name) {
        if (auto r = m_enum.find(I_name))
            return *r;
        buffer<name> c_names;
        get_constructors_of(I_name, c_names);
        bool result = true;
        if (c_names.size() <= 2) {
            result = false;
        } else {
            for (name const & c_name : c_names) {
                declaration d = m_env.get(c_name);
                expr type = d.get_type();
                if (!is_constant(type) || const_name(type) != I_name) {
                    result = false;
                    break;
                }
            }
        }
        m_enum.insert(I_name, result);
        return result;
    }

    bool is_value(expr const & e) {
        if (!m_use_ite) return false;
        if (to_num(e) || to_char(e) || to_string(e)) return true;
        if (optional<name> I_name = is_constructor(e)) return is_nontrivial_enum(*I_name);
        return false;
    }

    bool is_invertible_app(expr const & e) {
        if (!is_app(e)) return false;
        expr const & fn = get_app_fn(e);
        if (!is_constant(fn)) return false;
        optional<inverse_info> info = has_inverse(m_env, const_name(fn));
        if (!info) return false;
        return info->m_arity == get_app_num_args(e);
    }

    unsigned get_inductive_num_params(name const & n) const { return *inductive::get_num_params(m_env, n); }
    unsigned get_inductive_num_params(expr const & I) const { return get_inductive_num_params(const_name(I)); }

    void get_constructors_of(name const & n, buffer<name> & c_names) const {
        lean_assert(is_inductive(n));
        get_intro_rule_names(m_env, n, c_names);
    }

    /* Return the number of constructor parameters.
       That is, the fixed parameters used in the inductive declaration. */
    unsigned get_constructor_num_params(expr const & n) const {
        lean_assert(is_constructor(n));
        name I_name = *inductive::is_intro_rule(m_env, const_name(n));
        return get_inductive_num_params(I_name);
    }

    /* Normalize until head is constructor or value */
    expr whnf_pattern(type_context & ctx, expr const & e) {
        if (is_inaccessible(e)) {
            return e;
        } else {
            return ctx.whnf_pred(e, [&](expr const & e) {
                    return !is_constructor_app(e) && !is_value(e) && !is_invertible_app(e);
                });
        }
    }

    /* Normalize until head is constructor */
    expr whnf_constructor(type_context & ctx, expr const & e) {
        return ctx.whnf_pred(e, [&](expr const & e) {
                return !is_constructor_app(e);
            });
    }

    /* Normalize until head is an inductive datatype */
    expr whnf_inductive(type_context & ctx, expr const & e) {
        return ctx.whnf_pred(e, [&](expr const & e) {
                return !is_inductive_app(e);
            });
    }

    optional<equation> mk_equation(local_context const & lctx, expr const & eqn, unsigned idx) {
        expr it = eqn;
        it = binding_body(it); /* consume fn header */
        if (is_no_equation(it)) return optional<equation>();
        type_context ctx = mk_type_context(lctx);
        buffer<expr> locals;
        while (is_lambda(it)) {
            expr type  = instantiate_rev(binding_domain(it), locals);
            expr local = ctx.push_local(binding_name(it), type);
            locals.push_back(local);
            it = binding_body(it);
        }
        lean_assert(is_equation(it));
        equation E;
        E.m_vars = to_list(locals);
        E.m_lctx = ctx.lctx();
        E.m_rhs  = instantiate_rev(equation_rhs(it), locals);
        /* The function being defined is not recursive. So, E.m_rhs
           must be closed even if we "consumed" the fn header in
           the beginning of the method. */
        lean_assert(closed(E.m_rhs));
        buffer<expr> lhs_args;
        get_app_args(equation_lhs(it), lhs_args);
        buffer<expr> patterns;
        for (expr & arg : lhs_args) {
            arg = instantiate_rev(arg, locals);
            patterns.push_back(whnf_pattern(ctx, arg));
        }
        E.m_lhs_args = to_list(lhs_args);
        E.m_patterns = to_list(patterns);
        E.m_ref      = eqn;
        E.m_eqn_idx  = idx;
        return optional<equation>(E);
    }

    list<equation> mk_equations(local_context const & lctx, buffer<expr> const & eqns) {
        buffer<equation> R;
        unsigned idx = 0;
        for (expr const & eqn : eqns) {
            if (auto r = mk_equation(lctx, eqn, idx)) {
                R.push_back(*r);
                lean_assert(length(R[0].m_patterns) == length(r->m_patterns));
            } else {
                lean_assert(eqns.size() == 1);
                return list<equation>();
            }
            m_used_eqns.push_back(false);
            idx++;
        }
        return to_list(R);
    }

    unsigned get_eqns_arity(local_context const & lctx, expr const & eqns) {
        /* Naive way to retrieve the arity of the function being defined */
        lean_assert(is_equations(eqns));
        type_context ctx = mk_type_context(lctx);
        unpack_eqns ues(ctx, eqns);
        return ues.get_arity_of(0);
    }

    pair<problem, expr> mk_problem(local_context const & lctx, expr const & e) {
        lean_assert(is_equations(e));
        buffer<expr> eqns;
        to_equations(e, eqns);
        problem P;
        P.m_fn_name    = binding_name(eqns[0]);
        expr fn_type   = binding_domain(eqns[0]);
        expr mvar      = m_mctx.mk_metavar_decl(lctx, fn_type);
        unsigned arity = get_eqns_arity(lctx, e);
        buffer<name> var_names;
        optional<expr> goal = intron(m_env, m_opts, m_mctx, mvar,
                                     arity, var_names);
        if (!goal) throw_ill_formed_eqns();
        P.m_goal       = *goal;
        local_context goal_lctx = get_local_context(*goal);
        buffer<expr> vars;
        for (name const & n : var_names)
            vars.push_back(goal_lctx.get_local_decl(n)->mk_ref());
        P.m_var_stack  = to_list(vars);
        P.m_equations  = mk_equations(lctx, eqns);
        return mk_pair(P, mvar);
    }

    /* Return true iff the next element in the variable stack is a variable.
       \remark It may not be because of dependent pattern matching. */
    bool is_next_var(problem const & P) {
        lean_assert(P.m_var_stack);
        expr const & x = head(P.m_var_stack);
        return is_local(x);
    }

    template<typename Pred>
    bool all_next_pattern(problem const & P, Pred && p) const {
        for (equation const & eqn : P.m_equations) {
            lean_assert(eqn.m_patterns);
            if (!p(head(eqn.m_patterns)))
                return false;
        }
        return true;
    }

    /* Return true iff the next pattern in all equations is a variable. */
    bool is_variable_transition(problem const & P) const {
        return all_next_pattern(P, is_local);
    }

    /* Return true iff the next pattern in all equations is an inaccessible term. */
    bool is_inaccessible_transition(problem const & P) const {
        return all_next_pattern(P, is_inaccessible);
    }

    /* Return true iff the next pattern in all equations is a constructor. */
    bool is_constructor_transition(problem const & P) {
        return all_next_pattern(P, [&](expr const & p) {
                return is_constructor_app(p) || is_value(p);
            });
    }

    /* Return true iff the next pattern in all equations is the same invertible function. */
    bool is_invertible_transition(problem const & P) {
        if (!P.m_equations) return false;
        optional<name> fn_name;
        return all_next_pattern(P, [&](expr const & p) {
                if (!is_invertible_app(p)) return false;
                name const & curr_name = const_name(get_app_fn(p));
                if (fn_name) {
                    return *fn_name == curr_name;
                } else {
                    fn_name = curr_name;
                    return true;
                }
            });
    }

    /* Return true iff the next pattern of every equation is a constructor or variable,
       and there are at least one equation where it is a variable and another where it is a
       constructor. */
    bool is_complete_transition(problem const & P) const {
        bool has_variable    = false;
        bool has_constructor = false;
        bool r = all_next_pattern(P, [&](expr const & p) {
                if (is_local(p)) {
                    has_variable = true; return true;
                } else if (is_constructor_app(p)) {
                    has_constructor = true; return true;
                } else {
                    return false;
                }
            });
        return r && has_variable && has_constructor;
    }

    /* Return true iff the next pattern of every equation is a value or variable,
       and there are at least one equation where it is a variable and another where it is a
       value. */
    bool is_value_transition(problem const & P) {
        bool has_value    = false;
        bool has_variable = false;
        bool r = all_next_pattern(P, [&](expr const & p) {
                if (is_local(p)) {
                    has_variable = true; return true;
                } else if (is_value(p)) {
                    has_value    = true; return true;
                } else {
                    return false;
                }
            });
        return r && has_value && has_variable;
    }

    /** Return true iff the next pattern of some equations is an inaccessible term, and
        others are not */
    bool some_inaccessible(problem const & P) const {
        bool found_inaccessible     = false;
        bool found_not_inaccessible = false;
        for (equation const & eqn : P.m_equations) {
            lean_assert(eqn.m_patterns);
            expr const & p = head(eqn.m_patterns);
            if (is_inaccessible(p))
                found_inaccessible = true;
            else
                found_not_inaccessible = true;
        }
        return found_inaccessible && found_not_inaccessible;
    }

    /** Replace local `x` in `e` with `renaming.find(x)` */
    expr apply_renaming(expr const & e, name_map<expr> const & renaming) {
        return replace(e, [&](expr const & e, unsigned) {
                if (is_local(e)) {
                    if (auto new_e = renaming.find(mlocal_name(e)))
                        return some_expr(*new_e);
                }
                return none_expr();
            });
    }

    expr get_next_pattern_of(list<equation> const & eqns, unsigned eqn_idx) {
        for (equation const & eqn : eqns) {
            lean_assert(eqn.m_patterns);
            if (eqn.m_eqn_idx == eqn_idx)
                return head(eqn.m_patterns);
        }
        lean_unreachable();
    }

    hsubstitution add_subst(hsubstitution subst, expr const & src, expr const & target) {
        lean_assert(is_local(src));
        if (!subst.contains(mlocal_name(src)))
            subst.insert(mlocal_name(src), target);
        return subst;
    }

    /* Variable and Inaccessible transition are very similar, this method implements both. */
    list<lemma> process_variable_inaccessible(problem const & P, bool is_var_transition) {
        lean_assert(is_variable_transition(P) || is_inaccessible_transition(P));
        lean_assert(is_var_transition == is_variable_transition(P));
        problem new_P;
        new_P.m_fn_name   = P.m_fn_name;
        new_P.m_goal      = P.m_goal;
        new_P.m_var_stack = tail(P.m_var_stack);
        buffer<equation> new_eqns;
        for (equation const & eqn : P.m_equations) {
            equation new_eqn   = eqn;
            new_eqn.m_patterns = tail(eqn.m_patterns);
            if (is_var_transition) {
                new_eqn.m_subst  = add_subst(eqn.m_subst, head(eqn.m_patterns), head(P.m_var_stack));
            }
            new_eqns.push_back(new_eqn);
        }
        new_P.m_equations = to_list(new_eqns);
        return process(new_P);
    }

    list<lemma> process_variable(problem const & P) {
        trace_match(tout() << "step: variables only\n";);
        return process_variable_inaccessible(P, true);
    }

    list<lemma> process_inaccessible(problem const & P) {
        trace_match(tout() << "step: inaccessible terms only\n";);
        return process_variable_inaccessible(P, false);
    }

    /* Make sure then next pattern of each equation is a constructor application. */
    list<equation> normalize_next_pattern(list<equation> const & eqns) {
        buffer<equation> R;
        for (equation const & eqn : eqns) {
            lean_assert(eqn.m_patterns);
            type_context ctx = mk_type_context(eqn.m_lctx);
            expr pattern     = whnf_constructor(ctx, head(eqn.m_patterns));
            if (!is_constructor_app(pattern)) {
                throw_error("equation compiler failed, pattern is not a constructor "
                            "(use 'set_option trace.eqn_compiler.elim_match true' for additional details)");
            }
            equation new_eqn   = eqn;
            new_eqn.m_patterns = cons(pattern, tail(eqn.m_patterns));
            R.push_back(new_eqn);
        }
        return to_list(R);
    }

    /* Append `ilist` and `var_stack`. Due to dependent pattern matching, ilist may contain terms that
       are not local constants. */
    list<expr> update_var_stack(list<expr> const & ilist, list<expr> const & var_stack, hsubstitution const & subst) {
        buffer<expr> new_var_stack;
        for (expr const & e : ilist) {
            new_var_stack.push_back(e);
        }
        for (expr const & v : var_stack) {
            new_var_stack.push_back(apply(v, subst));
        }
        return to_list(new_var_stack);
    }

    /* eqns is the data-structured returned by distribute_constructor_equations.
       This method selects the ones such that eqns[i].first == C.
       It also updates eqns[i].second.m_subst using \c new_subst.
       It also "replaces" the next pattern (a constructor) with its fields.

       The map \c new_subst is produced by the `cases` tactic.
       It is needed because the `cases` tactic may revert and reintroduce hypothesis that
       depend on the hypothesis being destructed. */
    list<equation> get_equations_for(name const & C, unsigned nparams, hsubstitution const & new_subst,
                                     list<equation> const & eqns) {
        buffer<equation> R;
        for (equation const & eqn : eqns) {
            expr pattern = head(eqn.m_patterns);
            buffer<expr> pattern_args;
            expr const & C2 = get_app_args(pattern, pattern_args);
            if (!is_constant(C2, C)) continue;
            equation new_eqn   = eqn;
            new_eqn.m_subst    = apply(eqn.m_subst, new_subst);
            /* Update patterns */
            type_context ctx   = mk_type_context(eqn.m_lctx);
            for (unsigned i = nparams; i < pattern_args.size(); i++)
                pattern_args[i] = whnf_pattern(ctx, pattern_args[i]);
            new_eqn.m_patterns = to_list(pattern_args.begin() + nparams, pattern_args.end(), tail(eqn.m_patterns));
            R.push_back(new_eqn);
        }
        return to_list(R);
    }

    list<lemma> process_constructor(problem const & P) {
        trace_match(tout() << "step: constructors only\n";);
        lean_assert(is_constructor_transition(P));
        type_context ctx   = mk_type_context(P);
        expr x             = head(P.m_var_stack);
        expr x_type        = whnf_inductive(ctx, ctx.infer(x));
        lean_assert(is_inductive_app(x_type));
        name I_name        = const_name(get_app_fn(x_type));
        unsigned I_nparams = get_inductive_num_params(I_name);
        intros_list ilist;
        hsubstitution_list slist;
        list<expr> new_goals;
        list<name> new_goal_cnames;
        try {
            list<name> ids;
            std::tie(new_goals, new_goal_cnames) =
                cases(m_env, m_opts, transparency_mode::Semireducible, m_mctx,
                      P.m_goal, x, ids, &ilist, &slist);
            lean_assert(length(new_goals) == length(new_goal_cnames));
            lean_assert(length(new_goals) == length(ilist));
            lean_assert(length(new_goals) == length(slist));
        } catch (exception & ex) {
            throw nested_exception("equation compiler failed (use 'set_option trace.eqn_compiler.elim_match true' "
                                   "for additional details)", ex);
        }
        if (empty(new_goals)) return list<lemma>();
        list<equation> eqns = normalize_next_pattern(P.m_equations);
        buffer<lemma> new_Ls;
        while (new_goals) {
            lean_assert(new_goal_cnames && ilist && slist);
            problem new_P;
            new_P.m_fn_name        = name(P.m_fn_name, head(new_goal_cnames).get_string());
            expr new_goal          = head(new_goals);
            new_P.m_var_stack      = update_var_stack(head(ilist), tail(P.m_var_stack), head(slist));
            new_P.m_goal           = new_goal;
            name const & C         = head(new_goal_cnames);
            new_P.m_equations      = get_equations_for(C, I_nparams, head(slist), eqns);
            to_buffer(process(new_P), new_Ls);
            new_goals       = tail(new_goals);
            new_goal_cnames = tail(new_goal_cnames);
            ilist           = tail(ilist);
            slist           = tail(slist);
        }
        return to_list(new_Ls);
    }

    list<lemma> process_value(problem const & P) {
        trace_match(tout() << "step: if-then-else\n";);
        bool is_last       = !tail(P.m_var_stack);
        expr x             = head(P.m_var_stack);
        local_context lctx = get_local_context(P.m_goal);
        type_context ctx   = mk_type_context(P);
        expr goal_type     = ctx.infer(P.m_goal);
        expr else_goal     = ctx.mk_metavar_decl(lctx, goal_type);
        buffer<expr> values;
        buffer<expr> value_goals;
        buffer<expr> eqs;
        for (equation const & eqn : P.m_equations) {
            expr const & p = head(eqn.m_patterns);
            if (is_last && is_local(p))
                break;
            if (!is_local(p) &&
                std::find(values.begin(), values.end(), p) == values.end()) {
                values.push_back(p);
                value_goals.push_back(ctx.mk_metavar_decl(lctx, goal_type));
                expr const & eq = mk_eq(ctx, x, p);
                eqs.push_back(eq);
            }
        }
        expr goal_val = else_goal;
        unsigned i    = value_goals.size();
        while (i > 0) {
            --i;
            goal_val  = mk_ite(ctx, eqs[i], value_goals[i], goal_val);
        }
        m_mctx = ctx.mctx();
        m_mctx.assign(P.m_goal, goal_val);
        buffer<lemma> new_Ls;
        for (unsigned i = 0; i < values.size(); i++) {
            /* Process equations for values[i] */
            problem new_P;
            expr val          = values[i];
            new_P.m_fn_name   = name(P.m_fn_name, "_ite_val");
            new_P.m_goal      = value_goals[i];
            new_P.m_var_stack = tail(P.m_var_stack);
            buffer<equation> new_eqns;
            for (equation const & eqn : P.m_equations) {
                expr const & p = head(eqn.m_patterns);
                if (p == val) {
                    equation new_eqn    = eqn;
                    new_eqn.m_patterns  = tail(new_eqn.m_patterns);
                    new_eqns.push_back(new_eqn);
                    if (is_last) break;
                } else if (is_local(p)) {
                    /* Replace variable `p` with `val` in this equation */
                    type_context ctx   = mk_type_context(eqn.m_lctx);
                    buffer<expr> from;
                    buffer<expr> to;
                    buffer<expr> new_vars;
                    for (expr const & curr : eqn.m_vars) {
                        if (curr == p) {
                            from.push_back(p);
                            to.push_back(val);
                        } else {
                            expr curr_type     = ctx.infer(curr);
                            expr new_curr_type = replace_locals(curr_type, from, to);
                            if (curr_type == new_curr_type) {
                                new_vars.push_back(curr);
                            } else {
                                expr new_curr = ctx.push_local(local_pp_name(curr), new_curr_type);
                                from.push_back(curr);
                                to.push_back(new_curr);
                                new_vars.push_back(new_curr);
                            }
                        }
                    }
                    equation new_eqn   = eqn;
                    new_eqn.m_vars     = to_list(new_vars);
                    new_eqn.m_lctx     = ctx.lctx();
                    new_eqn.m_lhs_args = map(eqn.m_lhs_args, [&](expr const & arg) {
                            return replace_locals(arg, from, to); });
                    new_eqn.m_rhs      = replace_locals(eqn.m_rhs, from, to);
                    new_eqn.m_patterns = map(tail(eqn.m_patterns), [&](expr const & p) {
                            return replace_locals(p, from, to); });
                    new_eqns.push_back(new_eqn);
                    if (is_last) break;
                }
            }
            new_P.m_equations = to_list(new_eqns);
            to_buffer(process(new_P), new_Ls);
        }
        {
            /* Else-case */
            problem new_P;
            new_P.m_fn_name   = name(P.m_fn_name, "_ite_else");
            new_P.m_goal      = else_goal;
            new_P.m_var_stack = tail(P.m_var_stack);
            buffer<equation> new_eqns;
            for (equation const & eqn : P.m_equations) {
                expr const & p = head(eqn.m_patterns);
                if (is_local(p)) {
                    equation new_eqn = eqn;
                    new_eqn.m_patterns = tail(new_eqn.m_patterns);
                    new_eqn.m_subst    = add_subst(eqn.m_subst, p, x);
                    type_context ctx   = mk_type_context(eqn.m_lctx);
                    new_eqn.m_hs       = eqn.m_hs;
                    unsigned idx       = length(eqn.m_hs) + 1;
                    for (unsigned i = 0; i < values.size(); i++) {
                        expr eq        = mk_eq(ctx, p, values[i]);
                        expr ne        = mk_not(ctx, eq);
                        expr H         = ctx.push_local(name("_h").append_after(idx), ne);
                        idx++;
                        new_eqn.m_hs   = cons(H, new_eqn.m_hs);
                    }
                    new_eqn.m_lctx     = ctx.lctx();
                    new_eqns.push_back(new_eqn);
                    if (is_last) break;
                }
            }
            new_P.m_equations = to_list(new_eqns);
            to_buffer(process(new_P), new_Ls);
        }
        return to_list(new_Ls);
    }

    list<lemma> process_complete(problem const & P) {
        lean_assert(is_complete_transition(P));
        trace_match(tout() << "step: variables and constructors\n";);
        /* The next pattern of every equation is a constructor or variable.
           We split the equations where the next pattern is a variable into cases.
           That is, we are reducing this case to the compile_constructor case. */
        buffer<equation> new_eqns;
        for (equation const & eqn : P.m_equations) {
            expr const & pattern = head(eqn.m_patterns);
            if (is_local(pattern)) {
                type_context ctx  = mk_type_context(eqn.m_lctx);
                expr pattern_type = whnf_inductive(ctx, ctx.infer(pattern));
                buffer<expr> I_args;
                expr const & I      = get_app_args(pattern_type, I_args);
                name const & I_name = const_name(I);
                levels const & I_ls = const_levels(I);
                unsigned nparams    = get_inductive_num_params(I_name);
                buffer<expr> I_params;
                I_params.append(nparams, I_args.data());
                buffer<name> constructor_names;
                get_constructors_of(I_name, constructor_names);
                for (name const & c_name : constructor_names) {
                    buffer<expr> c_vars;
                    expr c  = mk_app(mk_constant(c_name, I_ls), I_params);
                    expr it = whnf_inductive(ctx, ctx.infer(c));
                    while (is_pi(it)) {
                        expr new_arg = ctx.push_local(binding_name(it), binding_domain(it));
                        c_vars.push_back(new_arg);
                        c  = mk_app(c, new_arg);
                        it = whnf_inductive(ctx, instantiate(binding_body(it), new_arg));
                    }
                    if (ctx.is_def_eq(pattern_type, it)) {
                        expr var = pattern;
                        /* We are replacing `var` with `c` */
                        buffer<expr> from;
                        buffer<expr> to;
                        buffer<expr> new_vars;
                        for (expr const & curr : eqn.m_vars) {
                            if (curr == var) {
                                from.push_back(var);
                                to.push_back(c);
                                new_vars.append(c_vars);
                            } else {
                                expr curr_type     = ctx.infer(curr);
                                expr new_curr_type = replace_locals(curr_type, from, to);
                                if (curr_type == new_curr_type) {
                                    new_vars.push_back(curr);
                                } else {
                                    expr new_curr = ctx.push_local(local_pp_name(curr), new_curr_type);
                                    from.push_back(curr);
                                    to.push_back(new_curr);
                                    new_vars.push_back(new_curr);
                                }
                            }
                        }
                        equation new_eqn   = eqn;
                        new_eqn.m_lctx     = ctx.lctx();
                        new_eqn.m_vars     = to_list(new_vars);
                        new_eqn.m_lhs_args = map(eqn.m_lhs_args, [&](expr const & arg) {
                                return replace_locals(arg, from, to); });
                        new_eqn.m_rhs      = replace_locals(eqn.m_rhs, from, to);
                        new_eqn.m_patterns =
                            cons(c, map(tail(eqn.m_patterns), [&](expr const & p) {
                                        return replace_locals(p, from, to); }));
                        new_eqns.push_back(new_eqn);
                    }
                }
            } else {
                new_eqns.push_back(eqn);
            }
        }
        problem new_P = P;
        new_P.m_equations = to_list(new_eqns);
        return process(new_P);
    }

    list<lemma> process_no_equation(problem const & P) {
        if (!is_next_var(P)) {
            return process_variable(P);
        } else {
            type_context ctx = mk_type_context(P);
            expr x           = head(P.m_var_stack);
            expr arg_type    = ctx.infer(x);
            if (is_below_type(arg_type))
                return process_variable(P);
            else if (is_inductive_app(whnf_inductive(ctx, arg_type)))
                return process_constructor(P);
            else
                return process_variable(P);
        }
    }

    list<lemma> process_non_variable(problem const & P) {
        trace_match(tout() << "step: filter equations using constructor\n";);
        type_context ctx   = mk_type_context(P);
        expr p = head(P.m_var_stack);
        lean_assert(!is_local(p));
        p      = whnf_constructor(ctx, p);
        if (!is_constructor_app(p)) {
            throw_error("dependent pattern matching result is not a constructor application "
                        "(use 'set_option trace.eqn_compiler.elim_match true' "
                        "for additional details)");
        }
        expr p_type         = whnf_inductive(ctx, ctx.infer(p));
        lean_assert(is_inductive_app(p_type));
        name I_name         = const_name(get_app_fn(p_type));
        unsigned I_nparams  = get_inductive_num_params(I_name);
        buffer<expr> C_args;
        expr const & C      = get_app_args(p, C_args);
        list<equation> eqns = normalize_next_pattern(P.m_equations);
        problem new_P;
        new_P.m_fn_name     = P.m_fn_name;
        new_P.m_goal        = P.m_goal;
        buffer<expr> new_var_stack;
        for (unsigned i = I_nparams; i < C_args.size(); i++)
            new_var_stack.push_back(C_args[i]);
        to_buffer(tail(P.m_var_stack), new_var_stack);
        new_P.m_var_stack   = to_list(new_var_stack);
        new_P.m_equations   = get_equations_for(const_name(C), I_nparams, hsubstitution(), eqns);
        return process(new_P);
    }

    list<lemma> process_invertible(problem const & P) {
        trace_match(tout() << "step: invertible function\n";);
        type_context ctx  = mk_type_context(P);
        expr const & x    = head(P.m_var_stack);
        /* make inverse */
        expr const & p    = head(head(P.m_equations).m_patterns);
        lean_assert(is_invertible_app(p));
        expr const & fn   = get_app_fn(p);
        inverse_info info = *has_inverse(m_env, const_name(fn));
        buffer<bool> mask;
        mask.resize(info.m_inv_arity - 1, false);
        mask.push_back(true);
        expr inv;
        try {
            inv = mk_app(ctx, info.m_inv, mask.size(), mask.data(), &x);
        } catch (exception &) {
            throw_error(sstream() << "equation compiler failed, when trying to inverse function "
                        << "'" << const_name(fn) << "' (use 'set_option trace.eqn_compiler.elim_match true' "
                        << "for additional details)");
        }
        expr inv_type      = ctx.infer(inv);
        local_context lctx = ctx.lctx();
        name y_name("_y");
        expr y             = lctx.mk_local_decl(y_name, inv_type, inv);
        expr goal_type     = ctx.infer(P.m_goal);
        expr new_goal      = ctx.mk_metavar_decl(lctx, goal_type);
        expr new_val       = mk_let(y_name, inv_type, inv, mk_delayed_abstraction(new_goal, mlocal_name(y)));
        m_mctx             = ctx.mctx();
        m_mctx.assign(P.m_goal, new_val);
        problem new_P;
        new_P.m_fn_name    = name(P.m_fn_name, "_inv");
        new_P.m_var_stack  = cons(y, tail(P.m_var_stack));
        new_P.m_goal       = new_goal;
        buffer<equation> new_eqns;
        for (equation const & eqn : P.m_equations) {
            equation new_eqn   = eqn;
            expr const & p     = head(eqn.m_patterns);
            expr new_p         = app_arg(p);
            new_eqn.m_patterns = cons(new_p, tail(eqn.m_patterns));
            new_eqns.push_back(new_eqn);
        }
        new_P.m_equations  = to_list(new_eqns);
        return process(new_P);
    }

    list<lemma> process_leaf(problem const & P) {
        if (!P.m_equations) {
            throw_error("invalid non-exhaustive set of equations (use 'set_option trace.eqn_compiler.elim_match true' "
                        "for additional details)");
        }
        equation const & eqn       = head(P.m_equations);
        m_used_eqns[eqn.m_eqn_idx] = true;
        expr rhs                   = apply(eqn.m_rhs, eqn.m_subst);
        m_mctx.assign(P.m_goal, rhs);
        if (m_aux_lemmas) {
            lemma L;
            L.m_lctx     = eqn.m_lctx;
            L.m_vars     = eqn.m_vars;
            L.m_hs       = eqn.m_hs;
            L.m_lhs_args = erase_inaccessible_annotations(eqn.m_lhs_args);
            L.m_rhs      = eqn.m_rhs;
            L.m_eqn_idx  = eqn.m_eqn_idx;
            return to_list(L);
        } else {
            return list<lemma>();
        }
    }

    list<lemma> process(problem const & P) {
        flet<unsigned> inc_depth(m_depth, m_depth+1);
        trace_match(tout() << "depth [" << m_depth << "]\n" << pp_problem(P) << "\n";);
        lean_assert(check_problem(P));
        if (P.m_var_stack) {
            if (!P.m_equations) {
                return process_no_equation(P);
            } else if (!is_next_var(P)) {
                return process_non_variable(P);
            } else if (is_variable_transition(P)) {
                return process_variable(P);
            } else if (is_complete_transition(P)) {
                return process_complete(P);
            } else if (is_value_transition(P)) {
                return process_value(P);
            } else if (is_constructor_transition(P)) {
                return process_constructor(P);
            } else if (is_invertible_transition(P)) {
                return process_invertible(P);
            } else if (is_inaccessible_transition(P)) {
                return process_inaccessible(P);
            } else if (some_inaccessible(P)) {
                throw_error("invalid equations, inconsistent use of inaccessible term annotation, "
                            "in some equations pattern is an inaccessible term and in others it is not");
            } else {
                trace_match(tout() << "compilation failed at\n" << pp_problem(P) << "\n";);
                throw_error("equation compiler failed (use 'set_option trace.eqn_compiler.elim_match true' "
                            "for additional details)");
            }
        } else {
            return process_leaf(P);
        }
    }

    expr finalize_lemma(expr const & fn, lemma const & L) {
        buffer<expr> args;
        to_buffer(L.m_lhs_args, args);
        type_context ctx = mk_type_context(L.m_lctx);
        expr lhs = mk_app(fn, args);
        expr eq  = mk_eq(ctx, lhs, L.m_rhs);
        buffer<expr> locals;
        to_buffer(L.m_vars, locals);
        to_buffer(L.m_hs, locals);
        return ctx.mk_pi(locals, eq);
    }

    list<expr> finalize_lemmas(expr const & fn, list<lemma> const & Ls) {
        return map2<expr>(Ls, [&](lemma const & L) { return finalize_lemma(fn, L); });
    }

    elim_match_result operator()(local_context const & lctx, expr const & eqns) {
        lean_assert(equations_num_fns(eqns) == 1);
        DEBUG_CODE({
                type_context ctx = mk_type_context(lctx);
                lean_assert(!is_recursive_eqns(ctx, eqns));
            });
        m_aux_lemmas             = get_equations_header(eqns).m_aux_lemmas;
        m_ref                    = eqns;
        problem P; expr fn;
        std::tie(P, fn)          = mk_problem(lctx, eqns);
        lean_assert(check_problem(P));
        list<lemma> pre_Ls       = process(P);
        fn                       = m_mctx.instantiate_mvars(fn);
        trace_match_debug(tout() << "code:\n" << fn << "\n";);
        list<expr> Ls            = finalize_lemmas(fn, pre_Ls);
        return elim_match_result(fn, Ls);
    }
};

elim_match_result elim_match(environment & env, options const & opts, metavar_context & mctx,
                             local_context const & lctx, expr const & eqns) {
    elim_match_fn elim(env, opts, mctx);
    auto r = elim(lctx, eqns);
    env = elim.m_env;
    return r;
}

void initialize_elim_match() {
    register_trace_class({"eqn_compiler", "elim_match"});
    register_trace_class({"debug", "eqn_compiler", "elim_match"});

    g_eqn_compiler_ite = new name{"eqn_compiler", "ite"};
    register_bool_option(*g_eqn_compiler_ite, LEAN_DEFAULT_EQN_COMPILER_ITE,
                         "(equation compiler) use if-then-else terms when pattern matching on simple values "
                         "(e.g., strings and characters)");
}
void finalize_elim_match() {
    delete g_eqn_compiler_ite;
}
}