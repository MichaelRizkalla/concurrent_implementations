//#include <concurrent_queue.h>
#include <concurrent_queue.hpp>

#include <cassert>
#include <future>
#include <iostream>
#include <test_common.hpp>
#include <thread>
#include <vector>

struct Value {
    int x;

    ~Value() { x = -1; }
};
constexpr int MAX_GEN_VALUE = 200000;
template < class Type >
struct ValueGenerator {
    std::pair< Type, bool > operator()() {
        {
            std::scoped_lock Guard { mut };
            if (IsEnough()) {
                return std::make_pair(Type { 0 }, true);
            }
            return std::make_pair(Type { count++ }, false);
        }
    }
    bool isEnough() const noexcept {
        {
            std::scoped_lock Guard { mut };
            return IsEnough();
        }
    }

  private:
    bool               IsEnough() const noexcept { return count == MAX_GEN_VALUE; }
    int                count = 0;
    mutable std::mutex mut;
};

template < class Type >
using T = concurrent::concurrent_queue< Type >;

void test_push_try_pop() {
    T< Value > a {};

    for (auto i = 0; i < 33; ++i) {
        a.push(Value { i });
        Value v;
        a.try_pop(v);

        assert(v.x == i);
    }
}
void test_push_try_pop2() {
    T< Value > a {};

    for (auto i = 0; i < 33; ++i) {
        a.push(Value { i });
    }

    auto fut1 = std::async(std::launch::async, [&]() {
        Value v;
        while (a.try_pop(v)) {
        }
    });
    auto fut2 = std::async(std::launch::async, [&]() {
        Value v;
        while (a.try_pop(v)) {
        }
    });

    fut1.wait();
    fut2.wait();

    assert(a.empty());
}
void test_push_try_pop_multithread() {
    T< Value >              a {};
    ValueGenerator< Value > gen {};
    std::mutex              mut;
    std::atomic_int         counter  = 0;
    auto                    gen_push = [&]() {
        while (true) {
            const auto [value, isEnough] = gen();
            if (isEnough) {
                break;
            }
            a.push(value);
            counter++;
        }
    };
    auto pop = [&]() {
        Value v;
        while (auto f = a.try_pop(v) || !gen.isEnough()) {
            if (f) {
                counter--;
            }
        }
    };

    std::vector< std::future< void > > push_fn {};
    std::vector< std::future< void > > pop_fn {};

    for (auto i = 0; i < 10; ++i) {
        push_fn.emplace_back(std::async(std::launch::async, gen_push));
    }

    for (auto i = 0; i < 10; ++i) {
        push_fn[i].wait();
    }

    for (auto i = 0; i < 10; ++i) {
        pop_fn.emplace_back(std::async(std::launch::async, pop));
    }
    for (auto i = 0; i < 10; ++i) {
        pop_fn[i].wait();
    }

    assert(counter == 0);
    assert(a.empty());
}

int main() {
    test_push_try_pop();
    test_push_try_pop2();
    for (auto i = 0; i < 10; ++i) {
        test_push_try_pop_multithread();
    }
}
