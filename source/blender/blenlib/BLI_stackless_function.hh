/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <iostream>

#include <coroutine>
#include <type_traits>
#include <utility>

#include "BLI_assert.h"
#include "BLI_utildefines.h"

namespace blender {

namespace detail {

template<typename T> struct generator {
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  handle_type h_;

  struct promise_type {
    T value;

    auto get_return_object()
    {
      return generator(handle_type::from_promise(*this));
    }

    static std::suspend_never initial_suspend() noexcept
    {
      return {};
    }

    static std::suspend_always final_suspend() noexcept
    {
      return {};
    }

    static void unhandled_exception()
    {
      BLI_assert(false);
    }

    template<typename TT> void return_value(TT &&value_)
    {
      this->value = std::forward<TT>(value_);
    }
  };

  generator(handle_type h) : h_(h) {}
  ~generator()
  {
    h_.destroy();
  }

  T operator *() &&
  {
    return std::move(h_.promise().value);
  }
};

struct generator_void {
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  handle_type h_;

  struct promise_type {
    auto get_return_object()
    {
      return generator_void(handle_type::from_promise(*this));
    }

    static std::suspend_never initial_suspend() noexcept
    {
      return {};
    }

    static std::suspend_always final_suspend() noexcept
    {
      return {};
    }

    static void unhandled_exception()
    {
      BLI_assert(false);
    }

    void return_void() {}
  };

  generator_void(handle_type h) : h_(h) {}
  ~generator_void()
  {
    h_.destroy();
  }
};

}  // namespace detail

/**
 * Stack Overflow is commonly a logical mistake in a code. But there is rarely case when tradeoff
 * between code complexity and ability of explicitly creates stack are in the fight. For example if
 * there is traversal of a directed acyclic graph the way so each node perform some logic, and
 * visite of next nodes must be mixed with it. So one commonly have to explicitly define each step
 * of node logic, store data in some storage and mix that step processing with next nodes visite in
 * a stack quieu container of actions. That the primitive aims to solve problem of stack overflow
 * if one dont want to complicate code logic and plan to simply use real stack. Stackless part of
 * it do imply that current stack is not used to perform closure. It is also not a new thread, so
 * no related overhead. This is just allocation of new stack memory on a heap, execution on it and
 * receiving result. Is it expected from the user to ensure that there is no epidemy of partially
 * used stacks.
 */
inline auto stackless(const auto &func)
{
  using T = typename std::invoke_result<decltype(func)>::type;
  if constexpr (std::is_same_v<T, void>) {
    return ([&]() -> detail::generator_void { co_return func(); }());
  } else {
    return *([&]() -> detail::generator<T> { co_return func(); }());    
  }
}

}  // namespace blender
