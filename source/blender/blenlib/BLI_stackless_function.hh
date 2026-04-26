/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <type_traits>
#include <coroutine>
#include <utility>

#include "BLI_assert.h"

namespace blender {

namespace detail {

template<typename T>
struct generator {
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    T value;

    generator get_return_object() { return generator(handle_type::from_promise(*this)); }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() { BLI_assert(false); }

    // void return_void() {}

    template<typename TT>
    void return_value(TT &&value) {
      this->value = std::forward<TT>(value);
    }
  };

  handle_type h_;

  generator(handle_type h) : h_(h) {}
  ~generator() { h_.destroy(); }

  T operator()() &&
  {
    h_();
    return std::move(h_.promise().value);
  }
};

}

/**
 * Stack Overflow is commonly a logical mistake in a code. But there is rarely case when tradeoff between code complexity and ability of explicitly creates stack are in the fight.
 * For example if there is traversal of a directed acyclic graph the way so each node perform some logic, and visite of next nodes must be mixed with it. So one commonly have to explicitly define each step of node logic, store data in some storage and mix that step processing with next nodes visite in a stack quieu container of actions.
 * That the primitive aims to solve problem of stack overflow if one dont want to complicate code logic and plan to simply use real stack. Stackless part of it do imply that current stack is not used to perform closure. It is also not a new thread, so no related overhead. This is just allocation of new stack memory on a heap, execution on it and receiving result. Is it expected from the user to ensure that there is no epidemy of partially used stacks.
 */
inline auto stackless(auto &&func)
{
  using T = typename std::invoke_result<decltype(func)>::type;
  auto invoke = [](auto &&func) -> detail::generator<T> {
    co_return func();
  }(std::move(func));
  return std::move(invoke)();
}

}  // namespace blender
