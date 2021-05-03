#include <array>

#define EXPECTED_RESULT(type, count_, ...)                                                      \
    auto                                  results = std::array< type, count_ > { __VA_ARGS__ }; \
    std::array< type, count_ >::size_type count   = 0;

#define CHECK_RESULT(checked_output)       \
    for (auto element : checked_output) {  \
        assert(element == results[count]); \
        count++;                           \
    }                                      \
    assert(count = results.size());

#define CHECK_RESULT_MEMBER(checked_output, member_variable) \
    for (auto element : checked_output) {                    \
        assert(element.member_variable == results[count]);   \
        count++;                                             \
    }                                                        \
    assert(count = results.size());
    