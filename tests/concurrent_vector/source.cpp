#include <cassert>
#include <concurrent_vector.hpp>
#include <test_common.hpp>

void test_iteration() {
    using namespace concurrent;

    concurrent_vector< long > vec {};

    vec.assign({ 1, 3, 5, 7, 11, 13, 17, 19 });

    EXPECTED_RESULT(long, 8, 1, 3, 5, 7, 11, 13, 17, 19);
    CHECK_RESULT(vec);
}

void test_shrink_push_grow() {
    using namespace concurrent;

    concurrent_vector< int > v { 2, 3, 4 };
    v.clear();
    v.shrink_to_fit();

    v.push_back(21);
    v.push_back(22);

    auto first = v[0];

    v.grow_by(5, 23);

    assert(first == 21);

    EXPECTED_RESULT(int, 7, 21, 22, 23, 23, 23, 23, 23);
    CHECK_RESULT(v);
}

int main() {
    test_iteration();
    test_shrink_push_grow();
}
