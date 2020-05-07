#include <torch/csrc/jit/passes/specialize_autogradzero.h>
#include <torch/csrc/jit/runtime/graph_executor.h>

namespace torch {
namespace jit {

// propagate autograd zero information through a gradient graph and
// remove grad_of blocks if present.
// Note: this is a very limited pass. It only propagates autograd zeros for
// operations generated by the symbolic autodiff code and cleans up
// AutogradAdds when possible. Outputs of other nodes are conservatively
// marked Unknown and not optimized.
void specializeAutogradZero(Graph& g) {
  enum class State { Nonzero, Zero, Unknown };
  std::unordered_map<Value*, State> state;

  for (Value* input : g.inputs()) {
    const auto& tp = input->type();
    if (auto tt = tp->cast<TensorType>()) {
      if (tt->undefined()) {
        if (*tt->undefined()) {
          state[input] = State::Zero;
        } else {
          state[input] = State::Nonzero;
        }
      } else {
        state[input] = State::Unknown;
      }
    } else if (
        tp->isSubtypeOf(TensorType::get()) ||
        tp->isSubtypeOf(ListType::ofTensors())) {
      state[input] = State::Nonzero;
    } else {
      state[input] = State::Unknown;
    }
  }

  for (auto it = g.nodes().begin(); it != g.nodes().end(); ++it) {
    auto n = *it;

    switch (n->kind()) {
      case prim::AutogradAdd: {
        auto a = n->input(0);
        auto b = n->input(1);
        // if one is Autograd zero, we can just drop the add
        if (state[a] == State::Zero) {
          // Zero + b == b
          n->output()->replaceAllUsesWith(b);
          it.destroyCurrent();
        } else if (state[b] == State::Zero) {
          // a + Zero == a
          n->output()->replaceAllUsesWith(a);
          it.destroyCurrent();
        } else if (state[a] == State::Nonzero && state[b] == State::Nonzero) {
          // when both are Nonzero, we can use a normal, optimizable add
          // instruction
          WithInsertPoint guard(n);
          auto* g = n->owningGraph();
          auto* cOne = g->insertConstant(1);
          auto* add_node = g->insertNode(g->create(aten::add, 1));
          add_node->addInput(a);
          add_node->addInput(b);
          add_node->addInput(cOne);
          auto* add_output = add_node->output();
          add_output->setType(n->output()->type());
          state[add_output] = State::Nonzero;
          n->output()->replaceAllUsesWith(add_output);
          it.destroyCurrent();
        } else {
          // otherwise we have conditionally-Nonzero things, and we need
          // to actually run an AutogradAdd which will guard for Zeros
          // so we leave the op as is
          state[n->output()] = State::Unknown;
        }
      } break;
      case prim::AutogradZero: {
        state[n->output()] = State::Zero;
      } break;
      case prim::profile: {
        // if prim::profile doesn't have an input
        // it's a counter to keep track how many times
        // a graph was profiled
        if (n->inputs().size() > 0) {
          state[n->output()] = State::Unknown;
          // state[n->input()];
        }
        break;
      }
      case prim::BailOut: {
        if (auto ptt = n->output()->type()->expect<TensorType>()) {
          state[n->output()] = ptt->undefined()
              ? *ptt->undefined() ? State::Zero : State::Nonzero
              : State::Unknown;
        }
      } break;
      case prim::Guard: {
        if (auto ptt = n->output()->type()->expect<TensorType>()) {
          state[n->output()] = ptt->undefined()
              ? *ptt->undefined() ? State::Zero : State::Nonzero
              : State::Unknown;
        }
      } break;
      // Lowered GradOf block
      case prim::If: {
        auto if_input = n->input(0)->node();
        if (if_input->kind() == prim::AutogradAnyNonZero) {
          auto all_zeros = std::all_of(
              if_input->inputs().begin(),
              if_input->inputs().end(),
              [&](Value* v) { return state[v] == State::Zero; });

          auto all_nonzeros = std::all_of(
              if_input->inputs().begin(),
              if_input->inputs().end(),
              [&](Value* v) { return state[v] == State::Nonzero; });
          // Property 1: if all the gradInputs to the GradOf are Zero
          // then the gradOutputs are also zero and will be represented as
          // AutogradZero nodes
          if (all_zeros) {
            auto zero = g.createAutogradZero()->insertAfter(n)->output();
            state[zero] = State::Zero;
            for (auto o : n->outputs()) {
              o->replaceAllUsesWith(zero);
            }
            it.destroyCurrent();
            break;
          }

          if (all_nonzeros) {
            auto body = n->blocks().at(0);
            // hoist the nodes in the GradOf body to be before the linear block
            for (auto it = body->nodes().begin(); it != body->nodes().end();) {
              auto block_node = *it++;
              block_node->moveBefore(n);
            }

            for (size_t i = 0; i < n->outputs().size(); ++i) {
              n->outputs().at(i)->replaceAllUsesWith(body->outputs().at(i));
              state[body->outputs().at(i)] = State::Nonzero;
            }
            it.destroyCurrent();
            break;
          }
        }

        for (auto o : n->outputs()) {
          state[o] = State::Unknown;
        }
        break;
      }
      default:
        for (auto o : n->outputs()) {
          state[o] = State::Unknown;
        }
        break;
    }
  }
}

} // namespace jit
} // namespace torch
