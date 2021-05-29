#ifndef CONCURRENT_UTILS
#define CONCURRENT_UTILS

namespace concurrent {

    namespace traits {

        template < class TIter >
        using iterator_category = typename std::iterator_traits< TIter >::iterator_category;

        template < class TIter >
        struct is_legacy_input_iterator {
            static constexpr bool value = std::is_same_v< iterator_category< TIter >, std::input_iterator_tag > ||
                                          std::is_convertible_v< iterator_category< TIter >, std::input_iterator_tag >;
        };

        template < class TIter >
        inline constexpr bool is_legacy_input_iterator_v = is_legacy_input_iterator< TIter >::value;

        template < class TIter >
        concept legacy_input_iterator = is_legacy_input_iterator_v< TIter >;

        // Checks if the allocator has destroy function
        template < class Allocator, class PtrType >
        concept has_destroy = requires(Allocator alloc, PtrType type) {
            alloc.destroy(type);
        };

    } // namespace traits

    /// <summary>
    /// unfancy_ptr works similar to _Unfancy in MSVC STL
    /// Unwrap a pointer-like (fancy pointer) object or return if plain pointer
    /// </summary>
    /// <typeparam name="Type"></typeparam>
    /// <param name="as_ptr"></param>
    /// <returns></returns>
    template < class Type >
    [[nodiscard]] constexpr auto unfancy_ptr(Type as_ptr) noexcept {
        return std::addressof(*as_ptr);
    }
    template < class Type >
    [[nodiscard]] constexpr auto unfancy_ptr_with_null(Type as_ptr) noexcept {
        return as_ptr ? std::addressof(*as_ptr) : nullptr;
    }
    template < class TPtr >
    [[nodiscard]] constexpr TPtr *unfancy_ptr(TPtr *ptr) noexcept {
        return ptr;
    }

    namespace impl {
        template < class EvalType, typename ReturnType >
        struct Min_segment_size_eval {
          private:
            static constexpr ReturnType eval(EvalType *) {
                if constexpr (sizeof(EvalType) >= 32) {
                    return 8;
                } else if constexpr (sizeof(EvalType) >= 16) {
                    return 16;
                } else {
                    return 32;
                }
            }

          public:
            static constexpr ReturnType value = eval(0);
        };
    } // namespace impl
} // namespace concurrent

#endif // CONCURRENT_UTILS