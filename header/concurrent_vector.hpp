#ifndef CONCURRENT_VECTOR_HPP
#define CONCURRENT_VECTOR_HPP

#include <atomic>
#include <compare>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>

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

#define ALLOCATOR_ERROR_MESSAGE(concurrent_vector_type, type) \
    concurrent_vector_type "requires its allocator type to match " type

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

    template < class Type, class Allocator = std::allocator< Type > >
    class concurrent_vector;

    template < class TConVector, bool IsConst >
    struct concurrent_vector_iterator {
        using iterator_concept  = std::contiguous_iterator_tag;
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = typename TConVector::value_type;
        using difference_type   = typename TConVector::difference_type;
        using pointer           = typename TConVector::const_pointer;
        using reference         = std::conditional_t< IsConst, const value_type &, value_type & >;

      private:
        using Size_type = typename TConVector::size_type;

        template < class Type, class Allocator >
        friend class concurrent_vector;

      public:
        constexpr concurrent_vector_iterator() noexcept : mIndex(-1), mVector(nullptr), mValue(nullptr) {}

        constexpr concurrent_vector_iterator(TConVector *vector, Size_type index) noexcept :
            mIndex(index), mVector(vector), mValue(nullptr) {}

        constexpr concurrent_vector_iterator &operator=(const concurrent_vector_iterator &) noexcept = default;

        [[nodiscard]] constexpr reference operator*() const noexcept {
            if (mValue) {
                return *mValue;
            }
            mValue = std::addressof(mVector->Get_value_at(mIndex));

            return *mValue;
        }
        [[nodiscard]] constexpr pointer operator->() const noexcept { return &operator*(); }

        constexpr concurrent_vector_iterator &operator++() noexcept {
            ++mIndex;
            mValue = nullptr;
            return *this;
        }
        constexpr concurrent_vector_iterator &operator++(int) noexcept {
            concurrent_vector_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        constexpr concurrent_vector_iterator &operator--() noexcept {
            --mIndex;
            mValue = nullptr;
            return *this;
        }
        constexpr concurrent_vector_iterator &operator--(int) noexcept {
            concurrent_vector_iterator tmp = *this;
            --*this;
            return tmp;
        }

        constexpr concurrent_vector_iterator &operator+=(const difference_type offset) noexcept {
            mIndex += offset;
            mValue = nullptr;
            return *this;
        }
        constexpr concurrent_vector_iterator &operator-=(const difference_type offset) noexcept {
            return *this += -offset;
        }

        [[nodiscard]] constexpr concurrent_vector_iterator operator+(const difference_type offset) const noexcept {
            return concurrent_vector_iterator { mVector, mIndex + offset };
        }
        [[nodiscard]] constexpr concurrent_vector_iterator operator-(const difference_type offset) const noexcept {
            return concurrent_vector_iterator { mVector, mIndex - offset };
        }

        [[nodiscard]] constexpr difference_type operator-(const concurrent_vector_iterator &rhs) const noexcept {
            return mIndex - rhs.mIndex;
        }

        [[nodiscard]] constexpr reference operator[](const difference_type offset) const noexcept {
            mValue = std::addressof(mVector->Get_value_at(mIndex + offset));

            return *mValue;
        }

        [[nodiscard]] constexpr bool operator==(const concurrent_vector_iterator &rhs) const noexcept {
            return mIndex == rhs.mIndex;
        }

        [[nodiscard]] constexpr std::strong_ordering operator<=>(const concurrent_vector_iterator &rhs) const noexcept {
            return mIndex <=> rhs.mIndex;
        }

        [[nodiscard]] constexpr bool is_valid() const noexcept { return mIndex != static_cast< Size_type >(-1); }

      private:
        Size_type           mIndex;
        mutable value_type *mValue;
        TConVector *        mVector;
    };

    template < class Type, class Allocator >
    class concurrent_vector {
        static_assert(std::is_same_v< Type, typename Allocator::value_type >,
                      ALLOCATOR_ERROR_MESSAGE("vector<T, Allocator>", "T"));

      private:
        using allocator_traits = std::allocator_traits< Allocator >;

      public:
        using value_type      = Type;
        using allocator_type  = Allocator;
        using size_type       = typename allocator_traits::size_type;
        using difference_type = typename allocator_traits::difference_type;
        using reference       = Type &;
        using const_reference = const Type &;
        using pointer         = typename allocator_traits::pointer;
        using const_pointer   = typename allocator_traits::const_pointer;

        using iterator               = concurrent_vector_iterator< concurrent_vector< Type, Allocator >, false >;
        using const_iterator         = concurrent_vector_iterator< concurrent_vector< Type, Allocator >, true >;
        using reverse_iterator       = std::reverse_iterator< iterator >;
        using const_reverse_iterator = std::reverse_iterator< const_iterator >;

      private:
        template < class TVec >
        struct Finaliser_If_Failed {
            constexpr Finaliser_If_Failed(TVec *ptr) noexcept : toBeFinalised(ptr) {}

            constexpr ~Finaliser_If_Failed() noexcept {
                if (toBeFinalised) {
                    toBeFinalised->Finalise();
                }
            }

            constexpr void NoFinalise() noexcept { toBeFinalised = nullptr; }

          private:
            TVec *toBeFinalised;
        };

        template < class Alloc >
        struct Temp_Object {
            using value_type   = typename Alloc::value_type;
            using alloc_traits = std::allocator_traits< Alloc >;

            template < class... Args >
            constexpr Temp_Object(Alloc &alloc, Args &&...args) noexcept(
                noexcept(alloc_traits::construct(alloc, std::addressof(this->mValue), std::forward< Args >(args)...))) :
                mAlloc(alloc) {
                alloc_traits::construct(alloc, std::addressof(this->mValue), std::forward< Args >(args)...);
            }
            constexpr ~Temp_Object() { alloc_traits::destroy(mAlloc, std::addressof(mValue)); }

            Temp_Object(const Temp_Object &) = delete;
            Temp_Object(Temp_Object &&)      = default;
            Temp_Object &operator=(const Temp_Object &) = delete;
            Temp_Object &operator=(Temp_Object &&) = default;

            Alloc &    mAlloc;
            value_type mValue;
        };

        struct move_in_place {};
        struct copy_in_place {};

        struct Data_Segment {
            pointer       mBegin { nullptr };
            pointer       mCurrent { nullptr };
            pointer       mEnd { nullptr };
            Data_Segment *mSegBefore { nullptr };
            Data_Segment *mSegAfter { nullptr };
        };

        // TODO: update with a new allocator class
        using segment_allocator =
            std::allocator< Data_Segment >; // allocator_traits::template rebind_alloc< Data_Segment >;
        using segment_allocator_traits = std::allocator_traits< segment_allocator >;
        using pointer_traits           = std::pointer_traits< pointer >;

        template < class EvalType >
        struct Min_segment_size_eval {
          private:
            static constexpr size_type eval(EvalType *) {
                if constexpr (sizeof(EvalType) >= 32) {
                    return 8;
                } else if constexpr (sizeof(EvalType) >= 16) {
                    return 16;
                } else {
                    return 32;
                }
            }

          public:
            static constexpr size_type value = eval(0);
        };

        static constexpr size_type Min_segment_size =
            Min_segment_size_eval< Type >::value; // Arbitrary starting size ~ 1KB

        template < class TConVector, bool IsConst >
        friend struct concurrent_vector_iterator;

      public:
        // Constructors - not concurrency-safe
        constexpr concurrent_vector() noexcept(noexcept(std::is_nothrow_constructible_v< Allocator >)) {}

        constexpr explicit concurrent_vector(const Allocator &alloc) noexcept : mAllocator(alloc) {}

        constexpr concurrent_vector(size_type count, const Type &value, const Allocator &alloc = Allocator {}) :
            mAllocator(alloc) {
            Construct_in_place_n(count, value);
        }

        constexpr explicit concurrent_vector(size_type count, const Allocator &alloc = Allocator {}) requires(
            std::is_default_constructible_v< Type >) :
            mAllocator(alloc) {
            Construct_in_place_n(count, Type {});
        }

        template < class InputIt >
        requires(traits::legacy_input_iterator< InputIt >) constexpr concurrent_vector(
            InputIt first, InputIt last, const Allocator &alloc = Allocator {}) :
            mAllocator(alloc) {
            Construct_range(first, last, traits::iterator_category< InputIt > {});
        }

        constexpr concurrent_vector(const concurrent_vector &rhs) :
            mAllocator(allocator_traits::select_on_container_copy_construction(rhs.mAllocator)) {
            {
                std::scoped_lock Guard { rhs.mMutex };
                const auto       rhsBegin   = rhs.begin();
                const auto       rhsCurrent = rhs.End();

                if (rhsBegin != rhsCurrent) {
                    Do_in_place_n< copy_in_place >(static_cast< size_type >(rhsCurrent - rhsBegin), rhsBegin,
                                                   rhsCurrent);
                }
            }
        }

        constexpr concurrent_vector(const concurrent_vector &rhs, const Allocator &alloc) {
            {
                std::scoped_lock Guard { rhs.mMutex };
                const auto       rhsBegin   = rhs.begin();
                const auto       rhsCurrent = rhs.End();

                if (rhsBegin != rhsCurrent) {
                    Do_in_place_n< copy_in_place >(static_cast< size_type >(rhsCurrent - rhsBegin), rhsBegin,
                                                   rhsCurrent);
                }
            }
        }

        constexpr concurrent_vector(concurrent_vector &&other) noexcept :
            mAllocator(std::move(other.mAllocator)),
            mSegmentAllocator(std::move(other.mSegmentAllocator)),
            mSegment(std::exchange(other.mSegment, nullptr)),
            mCurrent(std::exchange(other.mCurrent, nullptr)),
            mEnd(std::exchange(other.mEnd, nullptr)),
            mMutex(std::exchange(other.mMutex, nullptr)) {}

        constexpr concurrent_vector(concurrent_vector &&other, const Allocator &alloc) noexcept(
            noexcept(allocator_traits::is_always_equal::value)) :
            mAllocator(alloc) {
            move_construct(std::move(other), typename allocator_traits::is_always_equal::type {});
        }

        constexpr concurrent_vector(std::initializer_list< Type > init, const Allocator &alloc = Allocator {}) :
            mAllocator(alloc) {
            auto first = init.begin();
            auto last  = init.end();

            Construct_range(first, last, std::forward_iterator_tag {});
        }

        // Destructors
        constexpr ~concurrent_vector() noexcept { Finalise(); }

        // Operator=
        constexpr concurrent_vector &operator=(const concurrent_vector &rhs) {
            if (this != std::addressof(rhs)) {
                copy_assignment(rhs, typename allocator_traits::propagate_on_container_copy_assignment::type {});
            }

            return *this;
        }

        constexpr concurrent_vector &operator=(concurrent_vector &&rhs) noexcept(
            noexcept(allocator_traits::propagate_on_container_move_assignment::value) ||
            noexcept(allocator_traits::is_always_equal::value)) {
            // TODO: implement move assignment
            return *this;
        }

        constexpr concurrent_vector &operator=(std::initializer_list< Type > iList) {
            // TODO: implement assignment from initializer list
            return *this;
        }

        // assign - not concurrency-safe
        constexpr void assign(const size_type count, const Type &value) {
            {
                auto current_size = Get_size();

                if (count > current_size) {
                    const auto current_capacity = Get_capacity();
                    if (count > current_capacity) {
                        Deallocate_then_allocate(count);

                        mSegment.mCurrent = std::fill_n(begin(), count, value);
                    }

                    auto location_iter = std::fill_n(begin(), count, value);
                    mCurrent           = Adjust_segments_and_get_last_address_for(location_iter.mIndex);

                } else {
                    const auto new_mCurrent = begin() + count;
                    (void)std::fill_n(begin(), count, value);

                    Destruct(new_mCurrent, End());
                    mCurrent = Adjust_segments_and_get_last_address_for(new_mCurrent.mIndex);
                }
            }
        }

        template < class InputIt >
        requires(traits::legacy_input_iterator< InputIt >) constexpr void assign(InputIt first, InputIt last) {
            { Assign_range(first, last, traits::iterator_category< InputIt > {}); }
        }

        constexpr void assign(std::initializer_list< Type > iList) {
            { Assign_range(iList.begin(), iList.end(), std::forward_iterator_tag {}); }
        }

        // get allocator - concurrency-safe
        [[nodiscard]] constexpr allocator_type get_allocator() const noexcept {
            std::scoped_lock Guard { mMutex };
            return mAllocator;
        }

        ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
        [[nodiscard]] constexpr reference at(size_type pos) {
            std::scoped_lock Guard { mMutex };

            if (Get_size() <= pos) {
                throw_range_exception();
            }

            return Get_value_at(pos);
        }

        [[nodiscard]] constexpr const_reference at(size_type pos) const {
            std::scoped_lock Guard { mMutex };

            if (Get_size() <= pos) {
                throw_range_exception();
            }

            return Get_value_at(pos);
        }

        [[nodiscard]] constexpr reference operator[](size_type pos) noexcept {
            std::scoped_lock Guard { mMutex };
            return Get_value_at(pos);
        }

        [[nodiscard]] constexpr const_reference operator[](size_type pos) const noexcept {
            std::scoped_lock Guard { mMutex };
            return Get_value_at(pos);
        }

        [[nodiscard]] constexpr reference front() noexcept {
            std::scoped_lock Guard { mMutex };
            return mSegment.mBegin[0];
        }

        [[nodiscard]] constexpr const_reference front() const noexcept {
            std::scoped_lock Guard { mMutex };
            return mSegment.mBegin[0];
        }

        [[nodiscard]] constexpr reference back() noexcept {
            std::scoped_lock Guard { mMutex };
            if (!mCurrent)
                return Type {}; // undefined-behaviour
            return mCurrent->mCurrent[-1];
        }

        [[nodiscard]] constexpr const_reference back() const noexcept {
            std::scoped_lock Guard { mMutex };
            if (!mCurrent)
                return Type {}; // undefined-behaviour
            return mCurrent->mCurrent[-1];
        }

        /// Iterators - concurrency-safe
        ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
        constexpr iterator begin() noexcept { return Return_iterator(0); }

        constexpr const_iterator begin() const noexcept { return Return_const_iterator(0); }

        constexpr const_iterator cbegin() const noexcept { return begin(); }

        constexpr iterator end() noexcept { return Return_iterator(size()); }

        constexpr const_iterator end() const noexcept { return Return_const_iterator(size()); }

        constexpr const_iterator cend() const noexcept { return end(); }

        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator { end() }; }

        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator { end() }; }

        constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }

        constexpr reverse_iterator rend() noexcept { return reverse_iterator { begin() }; }

        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator { begin() }; }

        constexpr const_reverse_iterator crend() const noexcept { return rend(); }

        /// Capacity - concurrency-safe
        ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

        [[nodiscard]] constexpr bool empty() const noexcept {
            std::scoped_lock Guard { mMutex };
            return mSegment.mBegin == mSegment.mCurrent;
        }

        [[nodiscard]] constexpr size_type size() const noexcept {
            {
                std::scoped_lock Guard { mMutex };
                return Get_size();
            }
        }

        [[nodiscard]] constexpr size_type max_size() const noexcept {
            return static_cast< size_type >(std::numeric_limits< size_type >::max());
        }

        // Not concurrency-safe
        constexpr void reserve(const size_type new_cap) {
            if (new_cap > Get_capacity()) {
                Append_new_segment(new_cap);
            }
        }

        [[nodiscard]] constexpr size_type capacity() const noexcept {
            {
                std::scoped_lock Guard { mMutex };
                return Get_capacity();
            }
        }

        // Not concurrency-safe
        constexpr void shrink_to_fit() { // invalidates all the iterators
            {
                if (mCurrent != mEnd || mCurrent->mCurrent != mCurrent->mEnd) {
                    if (&mSegment == mCurrent && mSegment.mBegin == mSegment.mCurrent) {
                        Finalise_no_lock();
                        return;
                    }
                    Reallocate_to_fit_n(Get_size());
                }
            }
        }

        /// Modifiers - not concurrency-safe
        ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

        constexpr void clear() noexcept {
            // clear does not free internal arrays.
            // To free internal arrays, call the function shrink_to_fit after clear
            Destruct();
            mCurrent = Adjust_segments_and_get_last_address_for(0);
        }

        // Concurrency-safe
        template < class... Args >
        constexpr reference emplace_back(Args &&...args) {
            {
                std::scoped_lock Guard { mMutex };
                if ((mCurrent != mEnd) || (mCurrent->mCurrent != mCurrent->mEnd)) {
                    auto result = Emplace_at_without_reallocation(Get_size(), std::forward< Args >(args)...);
                    mCurrent->mCurrent++;
                    return *result;
                }

                return *Emplace_back_with_reallocation(std::forward< Args >(args)...);
            }
        }

        // Concurrency-safe
        constexpr void push_back(const Type &value) { emplace_back(value); }

        // Concurrency-safe
        constexpr void push_back(Type &&value) { emplace_back(std::move(value)); }

        constexpr void grow_by(const size_type count) { grow_by(count, Type {}); }

        constexpr void grow_by(const size_type count, const Type &value) {
            {
                std::scoped_lock Guard { mMutex };
                const auto       current_size = Get_size();
                const auto       new_size     = current_size + count;

                (void)Check_size< size_type >(static_cast< size_t >(new_size));

                // Grow
                const auto current_capacity = Get_capacity();

                if (new_size > current_capacity) {
                    // Append new segment
                    Append_new_segment(Calculate_new_segment_size(new_size));
                }

                const auto newCurrent = begin() + new_size;
                Fill_n(End(), count, value);
                mCurrent = Adjust_segments_and_get_last_address_for(newCurrent.mIndex);

                return;
            }
        }

        constexpr void swap(concurrent_vector &rhs) noexcept {
            static_assert(allocator_traits::propagate_on_container_swap::value,
                          "container's allocator can not be swapped!");
            if (this != std::addressof(rhs)) {
                std::swap(mAllocator, rhs.mAllocator);
                std::swap(mSegment, rhs.mSegment);
                std::swap(mCurrent, rhs.mCurrent);
                std::swap(mEnd, rhs.mEnd);
            }
        }

      private:
        [[nodiscard]] inline constexpr size_type Get_size() const noexcept {
            size_type result = 0;
            auto      iter   = &mSegment;
            do {
                result += static_cast< size_type >(iter->mCurrent - iter->mBegin);
                iter = iter->mSegAfter;
            } while (iter && iter != mCurrent);

            if (iter) {
                result += static_cast< size_type >(iter->mCurrent - iter->mBegin);
            }

            return result;
        }

        [[nodiscard]] inline constexpr size_type Get_capacity() const noexcept {
            size_type result = 0;
            auto      iter   = &mSegment;
            do {
                result += static_cast< size_type >(iter->mEnd - iter->mBegin);
                iter = iter->mSegAfter;
            } while (iter);

            return result;
        }

        inline constexpr reference Get_value_at(size_type index) noexcept { // index must be less than size()
            Data_Segment *target_segment = &mSegment;
            for (;;) {
                const auto segment_size = static_cast< size_type >(target_segment->mEnd - target_segment->mBegin);
                if (index < segment_size) {
                    return target_segment->mBegin[index];
                } else {
                    index -= segment_size;
                }
                target_segment = target_segment->mSegAfter;
            }
        }

        inline constexpr Data_Segment *
            Get_segment_address(size_type index) const noexcept { // index must be less than size()
            // Traverse
            Data_Segment *target_segment = &mSegment;
            for (;;) {
                const auto segment_size = static_cast< size_type >(target_segment->mEnd - target_segment->mBegin);
                if (index < segment_size) {
                    break;
                } else {
                    index -= segment_size;
                }
                target_segment = target_segment->mSegAfter;
            }

            return target_segment;
        }

        inline constexpr Data_Segment *
            Adjust_segments_and_get_last_address_for(size_type index) noexcept { // index must be equal to size()
            // Traverse
            Data_Segment *target_segment = &mSegment;
            while (target_segment) {
                const auto segment_size = static_cast< size_type >(target_segment->mEnd - target_segment->mBegin);
                if (index < segment_size) {
                    target_segment->mCurrent = target_segment->mBegin + index;
                    break;
                } else {
                    index -= segment_size;
                }
                target_segment->mCurrent = target_segment->mEnd;
                target_segment           = target_segment->mSegAfter;
            }

            if (target_segment) {
                auto reseted_segments = target_segment->mSegAfter;
                for (; reseted_segments; reseted_segments = reseted_segments->mSegAfter) {
                    reseted_segments->mCurrent = reseted_segments->mBegin;
                }
            }

            return target_segment;
        }

        inline constexpr void Append_new_segment(size_type seg_size) {
            if (mCurrent == &mSegment && !mSegment.mBegin) {
                const auto newBegin = mAllocator.allocate(seg_size);
                mSegment.mBegin     = newBegin;
                mSegment.mCurrent   = newBegin;
                mSegment.mEnd       = newBegin + seg_size;
                mSegment.mSegBefore = nullptr;
                mSegment.mSegAfter  = nullptr;

                return;
            }

            auto       newSegment = mSegmentAllocator.allocate(1);
            const auto newBegin   = mAllocator.allocate(seg_size);

            newSegment->mBegin     = newBegin;
            newSegment->mCurrent   = newBegin;
            newSegment->mEnd       = newBegin + seg_size;
            newSegment->mSegBefore = mCurrent;
            newSegment->mSegAfter  = nullptr;

            mCurrent->mSegAfter = newSegment;
            mEnd                = newSegment;
        }

        inline constexpr size_type Calculate_new_segment_size(size_type new_capacity) const noexcept {
            const auto [new_capacity_, current_capacity] = Calculate_new_capacity(new_capacity);
            return std::max(Min_segment_size, new_capacity_ - current_capacity);
        }

        template < class InputIt >
        inline constexpr void Construct_range(InputIt first, InputIt last, std::forward_iterator_tag) {
            const auto count = Check_size< size_type >(static_cast< size_t >(std::distance(first, last)),
                                                       "input iterator range is too long!");

            Do_in_place_n< copy_in_place >(count, first, last);
        }
        template < class InputIt >
        inline constexpr void Construct_range(InputIt first, InputIt last,
                                              std::input_iterator_tag) noexcept(noexcept(emplace_back(*first))) {
            Finaliser_If_Failed< concurrent_vector > finaliser { this };

            for (; first != last; ++first) {
                emplace_back(*first);
            }

            finaliser.NoFinalise();
        }

        template < class InputIt >
        inline constexpr void Assign_range(InputIt first, InputIt last, std::forward_iterator_tag) {
            const auto current_size = Get_size();
            const auto new_size     = Check_size< size_type >(static_cast< size_t >(std::distance(first, last)));

            if (new_size > current_size) {
                const auto current_capacity = Get_capacity();
                if (new_size > current_capacity) {
                    Deallocate_then_allocate(new_size);
                }

                Copy_n(begin(), first, last);
                mCurrent = Adjust_segments_and_get_last_address_for(new_size);
            } else {
                const auto begin_it       = begin();
                const auto new_current_it = begin_it + new_size;
                Copy_n(begin_it, first, last);
                Destruct(new_current_it, End());
                mCurrent = Adjust_segments_and_get_last_address_for(new_current_it.mIndex);
            }
        }
        template < class InputIt >
        inline constexpr void Assign_range(InputIt first, InputIt last, std::input_iterator_tag) {
            iterator target  = begin();
            iterator current = End();

            // Copy only within used range
            for (; first != last && target != current; ++first, (void)++target) {
                *target = *first;
            }

            Destruct(target, current);
            mCurrent = Adjust_segments_and_get_last_address_for(target.mIndex);

            // If more elements need to be copied, emplace_back them
            for (; first != last; ++first) {
                emplace_back(*first);
            }
        }

        template < class... Args >
        inline constexpr auto
            Emplace_at_without_reallocation(const size_type location, Args &&...args) noexcept(noexcept(
                std::is_nothrow_move_assignable_v< Type >) &&noexcept(std::is_nothrow_move_constructible_v< Type >)) {
            iterator location_iter { this, location };

            if (location == End().mIndex) {
                allocator_traits::construct(mAllocator, unfancy_ptr(location_iter), std::forward< Args >(args)...);
            } else {
                // if the required location has been occupied by an existing element,
                // the inserted element is constructed at another location at first
                auto                     end_iter        = End();
                auto                     before_end_iter = end_iter - 1;
                Temp_Object< Allocator > mTempObj { mAllocator, std::forward< Args >(args)... };

                allocator_traits::construct(mAllocator, unfancy_ptr(end_iter), std::move(*before_end_iter));
                Move_backward_n(end_iter, location_iter, before_end_iter);
                *location_iter = std::move(mTempObj.mValue);
            }

            return pointer_traits::pointer_to(*location_iter);
        }

        template < class... Args >
        inline constexpr auto Emplace_back_with_reallocation(Args &&...args) { // more nodes needed
            const auto current_size = Get_size();
            if (current_size == max_size()) {
                throw_length_exception();
            }

            const auto new_size         = current_size + 1;
            const auto new_segment_size = Calculate_new_segment_size(new_size);

            Append_new_segment(new_segment_size);
            if (current_size != 0) {
                mCurrent = mCurrent->mSegAfter;
            }

            allocator_traits::construct(mAllocator, unfancy_ptr(mCurrent->mCurrent), std::forward< Args >(args)...);

            mCurrent->mCurrent++;

            return pointer_traits::pointer_to(*(End() - 1));
        }

        inline constexpr void Construct_in_place_n(size_type count, const Type &val) {
            if (count != 0) {
                Allocate_new_n(count);

                Finaliser_If_Failed< concurrent_vector > finaliser { this };
                Fill_n(mCurrent->mCurrent, count, val);
                mCurrent->mCurrent += count;
                finaliser.NoFinalise();
            }
        }
        template < class TCommand, class InputIt >
        inline constexpr void Do_in_place_n(size_type count, InputIt first, InputIt last) {
            static_assert(
                std::disjunction_v< std::is_same< TCommand, copy_in_place >, std::is_same< TCommand, move_in_place > >);

            if (count != 0) {
                Allocate_new_n(count);

                Finaliser_If_Failed< concurrent_vector > finaliser { this };

                if constexpr (std::same_as< TCommand, copy_in_place >) {
                    Copy_n(mCurrent->mCurrent, first, last);
                } else if constexpr (std::same_as< TCommand, move_in_place >) {
                    Move_n(mCurrent->mCurrent, first, last);
                }
                mCurrent->mCurrent += count;

                finaliser.NoFinalise();
            }
        }
        inline constexpr void Allocate_new_n(const size_type new_cap) {
            (void)Check_size< size_type >(static_cast< std::size_t >(new_cap));

            const auto new_vec  = mAllocator.allocate(new_cap);
            mSegment.mBegin     = new_vec;
            mSegment.mCurrent   = new_vec;
            mSegment.mEnd       = new_vec + new_cap;
            mSegment.mSegBefore = nullptr;
            mSegment.mSegAfter  = nullptr;
            mCurrent            = &mSegment;
            mEnd                = &mSegment;
        }
        inline constexpr void Reallocate_to_fit_n(const size_type new_cap) {
            (void)Check_size< size_type >(static_cast< std::size_t >(new_cap));

            const auto data_size = Get_size();
            const auto newBegin  = mAllocator.allocate(new_cap);

            try {
                Move_or_copy_n(newBegin, begin(), End());
            } catch (...) {
                mAllocator.deallocate(newBegin, new_cap);
                throw;
            }

            Exchange_data(newBegin, data_size, new_cap);
        }

        // These set of functions have to be used on a contiguous target block or a forward iterator
        template < class TargetIt >
        inline constexpr void Fill_n(TargetIt target, size_type count, const Type &val) {
            for (; count > 0; --count) {
                allocator_traits::construct(mAllocator, unfancy_ptr(target), val);
                ++target;
            }
        }
        template < class TargetIt, class InputIt >
        inline constexpr void Copy_n(TargetIt target, InputIt first, InputIt last) {
            for (; first < last; ++first) {
                allocator_traits::construct(mAllocator, unfancy_ptr(target), *unfancy_ptr(first));
                ++target;
            }
        }
        template < class TargetIt, class InputIt >
        inline constexpr void Move_n(TargetIt target, InputIt first, InputIt last) {
            for (; first < last; ++first) {
                allocator_traits::construct(mAllocator, unfancy_ptr(target),
                                            std::forward< Type >(std::move(*unfancy_ptr(first))));
                ++target;
            }
        }
        template < class TargetIt, class InputIt >
        inline constexpr void Move_backward_n(TargetIt target, InputIt first, InputIt last) {
            while (first != last) {
                *--target = std::move(*--last);
            }
        }
        template < class TargetIt, class InputIt >
        inline constexpr void Move_or_copy_n(TargetIt target, InputIt first, InputIt last) {
            if constexpr (std::is_nothrow_move_constructible_v< Type > || !std::is_copy_constructible_v< Type >) {
                Move_n(target, first, last);
            } else {
                Copy_n(target, first, last);
            }
        }

        inline constexpr void Deallocate_then_allocate(const size_type new_size) {
            if (new_size > max_size()) {
                throw_length_exception();
            }
            Finaliser_If_Failed< concurrent_vector > finaliser { this };

            const auto [new_cap, _] = Calculate_new_capacity(new_size);

            Finalise_no_lock();
            Allocate_new_n(new_cap);

            finaliser.NoFinalise();
        }

        inline constexpr void move_construct(concurrent_vector &&other, std::true_type) noexcept {
            mSegment = std::exchange(other.mSegment, nullptr);
            mCurrent = std::exchange(other.mCurrent, nullptr);
            mEnd     = std::exchange(other.mEnd, nullptr);
        }
        inline constexpr void move_construct(concurrent_vector &&other, std::false_type) {
            if constexpr (!allocator_traits::is_always_equal::value) {
                if (mAllocator != other.mAllocator) {
                    const auto rhsBegin   = other.begin();
                    const auto rhsCurrent = other.End();
                    if (rhsBegin != rhsCurrent) {
                        Do_in_place_n< move_in_place >(static_cast< size_type >(rhsCurrent - rhsBegin), begin(), End());
                    }
                    return;
                }
            }

            move_construct(std::move(other), std::true_type {});
        }

        inline constexpr void copy_assignment(const concurrent_vector &rhs, std::true_type) {
            if (mAllocator != rhs.mAllocator) {
                Finalise_no_lock();
            }
            copy_assignment(rhs, std::false_type {});
        }
        inline constexpr void copy_assignment(const concurrent_vector &rhs, std::false_type) {
            if constexpr (allocator_traits::propagate_on_container_copy_assignment::value) {
                mAllocator = rhs.mAllocator;
            }
            assign(rhs.begin(), rhs.End());
        }

        inline constexpr void Finalise_no_lock() noexcept {
            auto segment_tracker = mEnd;
            while (segment_tracker) {
                Destruct_contiguous(segment_tracker->mBegin, segment_tracker->mCurrent);
                mAllocator.deallocate(segment_tracker->mBegin,
                                      static_cast< size_type >(segment_tracker->mEnd - segment_tracker->mBegin));

                segment_tracker->mBegin    = nullptr;
                segment_tracker->mCurrent  = nullptr;
                segment_tracker->mEnd      = nullptr;
                segment_tracker->mSegAfter = nullptr;

                Data_Segment *delete_segment = nullptr;
                if (segment_tracker->mSegBefore) {
                    delete_segment = segment_tracker;
                }

                segment_tracker = segment_tracker->mSegBefore;

                if (delete_segment) {
                    mSegmentAllocator.deallocate(delete_segment, 1);
                }
            }

            mEnd     = &mSegment;
            mCurrent = &mSegment;
        }
        inline constexpr void Finalise() noexcept {
            {
                std::scoped_lock Guard { mMutex };
                Finalise_no_lock();
            }
        }

        inline constexpr void Destruct() noexcept { Destruct(begin(), End()); }
        inline constexpr void Destruct(iterator first, iterator last) noexcept {
            for (; first != last; ++first) {
                allocator_traits::destroy(mAllocator, unfancy_ptr(first));
            }
        }
        inline constexpr void Destruct_contiguous(pointer first, pointer last) noexcept {
            auto begin_   = first;
            auto current_ = last;
            for (; begin_ != current_; ++begin_) {
                allocator_traits::destroy(mAllocator, unfancy_ptr(begin_));
            }
        }
        inline constexpr void Exchange_data(pointer newBegin, size_type dataSize, size_type dataCapacity) {
            Finalise_no_lock();

            mSegment.mBegin   = newBegin;
            mSegment.mCurrent = newBegin + dataSize;
            mSegment.mEnd     = newBegin + dataCapacity;
        }

        inline constexpr iterator       End() noexcept { return Return_iterator(Get_size()); }
        inline constexpr const_iterator End() const noexcept { return Return_const_iterator(Get_size()); }

        [[nodiscard]] inline constexpr iterator Return_iterator(const size_type offset) noexcept {
            return iterator { this, offset };
        }
        [[nodiscard]] inline constexpr iterator Return_const_iterator(const size_type offset) const noexcept {
            return const_iterator { this, offset };
        }

        template < class SizeType >
        [[nodiscard]] inline constexpr SizeType Check_size(const std::size_t size,
                                                           const char *message = "vector size is too long!") const {
            if (size > std::numeric_limits< SizeType >::max()) {
                throw_length_exception(message);
            }

            return static_cast< SizeType >(size);
        }
        [[nodiscard]] inline constexpr auto Calculate_new_capacity(const size_type new_size) const noexcept {
            // Geometry growth MSVC's vector algorithm
            const size_type current_capacity = Get_capacity();
            const auto      size_limit       = max_size();

            if (current_capacity > size_limit - current_capacity / 2) {
                return std::make_pair(size_limit, current_capacity);
            }

            const size_type Size = current_capacity + current_capacity / 2;

            if (Size < new_size) {
                return std::make_pair(new_size, current_capacity);
            }

            return std::make_pair(Size, current_capacity);
        }
        [[noreturn]] static void throw_length_exception(const char *message = "vector size is too long!") {
            throw std::length_error(message);
        }
        [[noreturn]] static void throw_range_exception(const char *message = "index out of range!") {
            throw std::out_of_range(message);
        }

        Data_Segment       mSegment {};
        Data_Segment *     mCurrent { &mSegment };
        Data_Segment *     mEnd { &mSegment };
        mutable std::mutex mMutex {};
        allocator_type     mAllocator {};
        segment_allocator  mSegmentAllocator {};
    };

} // namespace concurrent

#endif // !CONCURRENT_VECTOR_HPP
