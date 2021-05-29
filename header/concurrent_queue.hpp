#ifndef CONCURRENT_QUEUE_HPP
#define CONCURRENT_QUEUE_HPP

#include <cassert>
#include <memory>
#include <mutex>
#include <utils.h>

namespace concurrent {

#define CONCURRENT_QUEUE_ALLOCATOR_ERROR_MESSAGE(concurrent_queue_type, type) \
    concurrent_queue_type "requires its allocator type to match " type

    template < class Type, class Allocator = std::allocator< Type > >
    class concurrent_queue;

    template < class Type >
    struct concurrent_queue_iterator {
      public:
        using value_type = concurrent_queue< Type >::value_type;

      private:
        using MyQueue   = concurrent_queue< Type >;
        using MiniQueue = typename MyQueue::MiniQueue;
        using size_type = concurrent_queue< Type >::size_type;

        static constexpr MiniQueue *End_iterator_ptr   = nullptr;
        static constexpr size_type  End_iterator_value = static_cast< size_type >(-1);

        concurrent_queue_iterator(MyQueue *mini_queue) :
            mQueue(mini_queue),
            mMiniQueue(End_iterator_ptr),
            mElementIdxInMiniQueue(End_iterator_value),
            isValid(false) {}
        concurrent_queue_iterator(MyQueue *mini_queue, MiniQueue *node_location, size_type idx_in_node) :
            mQueue(mini_queue), mMiniQueue(node_location), mElementIdxInMiniQueue(idx_in_node), isValid(true) {
            const auto size = mini_queue->unsafe_size();
            if (size == 0) {
                Invalidate();
            }
#ifdef CONCURRENT_QUEUE_DEVELOPER_DEBUG
            if (isValid) {
                data = mMiniQueue->mBegin + mElementIdxInMiniQueue;
            } else {
                data = nullptr;
            }
#endif // CONCURRENT_QUEUE_DEVELOPER_DEBUG
        }

        template < typename TType >
        friend bool operator==(const concurrent_queue_iterator< TType > &lhs,
                               const concurrent_queue_iterator< TType > &rhs);

        template < typename TType >
        friend bool operator!=(const concurrent_queue_iterator< TType > &lhs,
                               const concurrent_queue_iterator< TType > &rhs);

        template < class TType, class Allocator >
        friend class concurrent_queue;

      public:
        using iterator_concept  = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        concurrent_queue_iterator() = default;

        ~concurrent_queue_iterator() = default;

        value_type &operator*() const { return mMiniQueue->mBegin[mElementIdxInMiniQueue]; }

        value_type *operator->() const { return &operator*(); }

        concurrent_queue_iterator &operator++() { // Advancing is not concurrency safe
            Next();
            return *this;
        }

        concurrent_queue_iterator operator++(int) { // Advancing is not concurrency safe
            concurrent_queue_iterator tmp = *this;
            Next();
            return tmp;
        }

      private:
        void Next() noexcept {
            assert(isValid && "Can not advance an end iterator!");

            const auto &&[next_queue, next_idx] = mQueue->Advance(mMiniQueue, mElementIdxInMiniQueue);

            mMiniQueue             = next_queue;
            mElementIdxInMiniQueue = next_idx;
            if (next_queue == End_iterator_ptr) {
                Invalidate();
#ifdef CONCURRENT_QUEUE_DEVELOPER_DEBUG
                data = nullptr;
#endif // CONCURRENT_QUEUE_DEVELOPER_DEBUG
            }
#ifdef CONCURRENT_QUEUE_DEVELOPER_DEBUG
            else {
                data = mMiniQueue->mBegin + mElementIdxInMiniQueue;
            }
#endif // CONCURRENT_QUEUE_DEVELOPER_DEBUG
        }

        void Invalidate() noexcept {
            isValid                = false;
            mMiniQueue             = End_iterator_ptr;
            mElementIdxInMiniQueue = End_iterator_value;
        }

        MyQueue *  mQueue { nullptr };
        MiniQueue *mMiniQueue { nullptr };
        size_type  mElementIdxInMiniQueue { 0 };
#ifdef CONCURRENT_QUEUE_DEVELOPER_DEBUG
        value_type *data { nullptr };
#endif // CONCURRENT_QUEUE_DEVELOPER_DEBUG
        bool isValid { false };
    };

    template < typename Type >
    bool operator==(const concurrent_queue_iterator< Type > &lhs, const concurrent_queue_iterator< Type > &rhs) {
        return (lhs.isValid == rhs.isValid) && (lhs.mMiniQueue == rhs.mMiniQueue) &&
               (lhs.mElementIdxInMiniQueue == rhs.mElementIdxInMiniQueue);
    }

    template < typename Type >
    bool operator!=(const concurrent_queue_iterator< Type > &lhs, const concurrent_queue_iterator< Type > &rhs) {
        return !(lhs == rhs);
    }

    template < class Type, class Allocator >
    class concurrent_queue {
        static_assert(std::is_same_v< Type, typename Allocator::value_type >,
                      CONCURRENT_QUEUE_ALLOCATOR_ERROR_MESSAGE("concurrent_queue<T, Allocator>", "T"));

      private:
        using allocator_traits = std::allocator_traits< Allocator >;
        using pointer          = typename allocator_traits::pointer;
        using const_pointer    = typename allocator_traits::const_pointer;

      public:
        using value_type      = Type;
        using allocator_type  = Allocator;
        using size_type       = typename allocator_traits::size_type;
        using difference_type = typename allocator_traits::difference_type;
        using reference       = Type &;
        using const_reference = const Type &;

        using iterator       = concurrent_queue_iterator< Type >;
        using const_iterator = concurrent_queue_iterator< Type >;

      private:
        struct MiniQueue {
            value_type *mElements { nullptr };
            value_type *mBegin { nullptr };
            value_type *mUsedBegin { nullptr };
            value_type *mCurrent { nullptr };
            value_type *mEnd { nullptr };
            MiniQueue * mNextQueue { nullptr };
        };

        using mini_queue_allocator        = std::allocator< MiniQueue >;
        using mini_queue_allocator_traits = std::allocator_traits< mini_queue_allocator >;

        static constexpr size_type Min_queue_size = impl::Min_segment_size_eval< Type, size_type >::value;

        template < class TType >
        friend class concurrent_queue_iterator;

      public:
        explicit concurrent_queue(const allocator_type &allocator = allocator_type {}) : mAllocator(allocator) {}

        concurrent_queue(const concurrent_queue &queue, const allocator_type &allocator = allocator_type {}) :
            mAllocator(allocator) {
            // Copy construct queue
        }

        concurrent_queue(concurrent_queue &&queue, const allocator_type &allocator = allocator_type {}) :
            mQueue(std::exchange(queue.mQueue, nullptr)), mAllocator(allocator) {}

        template < typename InputIter >
        concurrent_queue(InputIter first, InputIter last) {
            // Copy iterator to queue based on iterator tag
        }

        ~concurrent_queue() { Finalise(); }

        bool empty() const {
            {
                std::scoped_lock Guard { mMutex };
                if (!mQueue) {
                    return true;
                }
                return static_cast< bool >(mQueue->mCurrent == mQueue->mUsedBegin);
            }
        }

        allocator_type get_allocator() const {
            std::scoped_lock Guard { mMutex };
            auto             alloc = mAllocator;
            return alloc;
        }

        void push(const Type &value) {
            {
                std::scoped_lock Guard { mMutex };
                Internal_push(value);
            }
        }

        void push(Type &&value) {
            {
                std::scoped_lock Guard { mMutex };
                Internal_push(std::move(value));
            }
        }

        bool try_pop(Type &dest) {
            {
                std::scoped_lock Guard { mMutex };
                if (!mQueue) {
                    dest = Type {};
                    return false;
                }

                if (mQueue->mCurrent == mQueue->mUsedBegin) {
                    dest = Type {};
                    return false;
                }

                return Internal_Pop(dest);
            }
        }

        iterator unsafe_begin() { return iterator(this, mQueue, 0); }

        const_iterator unsafe_begin() const { return iterator(this, 0); }

        iterator unsafe_end() { return iterator(this); }

        const_iterator unsafe_end() const { return iterator(this); }

        size_type unsafe_size() const {
            auto queue = mQueue;
            if (!queue) {
                return static_cast< size_type >(0);
            }

            size_type size = 0;
            while (queue) {
                size += Get_mini_queue_used_size(queue);
                queue = queue->mNextQueue;
            }
            return size;
        }

        void clear() { Destroy_all_elements(); }

#ifndef CONCURRENT_QUEUE_DEVELOPER_DEBUG
      private:
#endif // CONCURRENT_QUEUE_DEVELOPER_DEBUG
        template < class... Args >
        void Internal_push(Args &&...args) {
            if (mQueueEnd == mQueueCurrent) {
                if (!mQueue) {
                    auto queue = Allocate_mini_queue(1, 0);
                    Emplace_back_mini_queue(queue);
                } else if (mQueueCurrent->mCurrent == mQueueCurrent->mEnd) {
                    const auto size = unsafe_size();

                    auto queue = Allocate_mini_queue(size + 1, size);
                    Emplace_back_mini_queue(queue);
                }
            }

            allocator_traits::construct(mAllocator, unfancy_ptr(mQueueCurrent->mCurrent),
                                        std::forward< Args >(args)...);
            mQueueCurrent->mCurrent++;

            if (mQueueCurrent->mCurrent == mQueueCurrent->mEnd && mQueueCurrent != mQueueEnd) {
                mQueueCurrent->mCurrent = mQueueCurrent->mNextQueue->mBegin;
                mQueueCurrent           = mQueueCurrent->mNextQueue;
            }
        }

        bool Internal_Pop(Type &dest) {
            dest = *mQueue->mUsedBegin;
            allocator_traits::destroy(mAllocator, unfancy_ptr(mQueue->mUsedBegin));
            mQueue->mUsedBegin++;

            if (mQueue->mUsedBegin == mQueue->mEnd) {
                mQueue->mUsedBegin = mQueue->mBegin;
                mQueue->mCurrent   = mQueue->mBegin;

                if (mQueue != mQueueEnd) {
                    mQueueEnd->mNextQueue = mQueue;
                    mQueueEnd             = mQueue;
                    mQueue                = std::exchange(mQueue->mNextQueue, nullptr);
                }
            }
            return true;
        }

        auto Get_mini_queue_size(const MiniQueue *const queue) const noexcept { // User shall guarantee no nullptr;
            assert(queue);
            return static_cast< size_type >(queue->mCurrent - queue->mBegin);
        }

        auto Get_mini_queue_capacity(const MiniQueue *const queue) const noexcept { // User shall guarantee no nullptr;
            assert(queue);
            return static_cast< size_type >(queue->mEnd - queue->mBegin);
        }

        /// <summary>
        /// Shall be used on the first node
        /// </summary>
        auto Get_mini_queue_used_size(const MiniQueue *const queue) const noexcept { // User shall guarantee no nullptr;
            assert(queue);
            return static_cast< size_type >(queue->mCurrent - queue->mUsedBegin);
        }

        /// <summary>
        /// Shall be used on the first node
        /// </summary>
        auto Get_mini_queue_used_capacity(
            const MiniQueue *const queue) const noexcept { // User shall guarantee no nullptr;
            assert(queue);
            return static_cast< size_type >(queue->mEnd - queue->mUsedBegin);
        }

        size_type Unsafe_capacity() const noexcept {
            auto queue = mQueue;
            if (!queue) {
                return static_cast< size_type >(0);
            }

            size_type capacity = 0;
            while (queue) {
                capacity += Get_mini_queue_used_capacity(queue);
                queue = queue->mNextQueue;
            }

            return capacity;
        }

        auto Unsafe_size_and_capacity() const noexcept {
            auto      queue    = mQueue;
            size_type capacity = 0;
            size_type size     = 0;
            if (!queue) {
                return std::make_pair(size, capacity);
            }

            while (queue) {
                capacity += Get_mini_queue_used_capacity(queue);
                size += Get_mini_queue_used_size(queue);
                queue = queue->mNextQueue;
            }
            return std::make_pair(size, capacity);
        }

        void Emplace_back_mini_queue(MiniQueue *queue) {
            if (!mQueue) {
                mQueue        = queue;
                mQueueCurrent = queue;
                mQueueEnd     = queue;

                return;
            }

            mQueueEnd->mNextQueue = queue;
            mQueueEnd             = queue;

            if (mQueueCurrent->mCurrent == mQueueCurrent->mEnd) {
                mQueueCurrent = mQueueCurrent->mNextQueue;
            }
        }

        template < class Func >
        void Apply_for_each_element_front_to_back(Func &&func) noexcept(
            std::declval< Func >()(std::declval< Type * >())) {
            auto queue = mQueue;
            while (queue) {
                const auto size = Get_mini_queue_size(queue);
                for (auto i = 0; i < size; ++i) {
                    func(&(queue->mElements[i]));
                }
                queue = queue->mNextQueue;
            }
        }

        template < class Func, class... Args >
        void Apply_for_each_element_front_to_back(Func &&func, Args &&...args) const
            noexcept(std::declval< Func >()(std::declval< Args >()...)) {
            auto queue = mQueue;
            while (queue) {
                const auto size = Get_mini_queue_size(queue);
                for (auto i = 0; i < size; ++i) {
                    func(args...);
                }
                queue = queue->mNextQueue;
            }
        }

        void Destroy_all_elements() {
            auto queue = mQueue;
            while (queue) {
                Destroy_all_elements_in_mini_queue(queue);
                queue->mCurrent   = queue->mBegin;
                queue->mUsedBegin = queue->mBegin;
                queue             = queue->mNextQueue;
            }
            mQueueCurrent = mQueue;
        }

        void Destroy_all_element_in_mini_queue(MiniQueue *queue) {
            for (auto i = queue->mUsedBegin; i < queue->mCurrent; ++i) {
                allocator_traits::destroy(mAllocator, i);
            }
        }

        std::pair< MiniQueue *, size_type > Advance(MiniQueue *output_queue, size_type element_idx) const noexcept {
            const auto current_queue_size     = Get_mini_queue_size(output_queue);
            const auto current_queue_capacity = Get_mini_queue_capacity(output_queue);

            if (current_queue_size == 0 && element_idx != current_queue_capacity - 1) {
                return std::make_pair(nullptr, 0);
            }

            if (element_idx < current_queue_size - 1) {
                return std::make_pair(output_queue, element_idx + 1);
            }

            const bool last_queue_element = element_idx == current_queue_size - 1;
            if (last_queue_element && current_queue_size < current_queue_capacity) {
                // last element in queue, and queue is not full.
                return std::make_pair(nullptr, 0);
            }

            if (last_queue_element && output_queue->mNextQueue == nullptr) {
                // last element in queue, queue is full, and no other queues.
                return std::make_pair(nullptr, 0);
            }

            auto       next_queue      = output_queue->mNextQueue;
            const auto next_queue_size = Get_mini_queue_size(output_queue);
            if (next_queue_size == 0) {
                return std::make_pair(nullptr, 0);
            }

            return std::make_pair(next_queue, 0);
        }

        void Finalise() {
            auto queue = mQueue;
            while (queue) {
                auto to_delete = queue;
                queue          = queue->mNextQueue;
                Destroy_all_element_in_mini_queue(to_delete);
                Deallocate_mini_queue(to_delete);
            }
        }

        MiniQueue *Allocate_mini_queue(size_type requested_new_size, size_type current_size) {
            auto new_capacity = Calculate_new_capacity(requested_new_size);
            auto new_size     = std::max(Min_queue_size, new_capacity - current_size);

            auto alloc = mini_queue_allocator {};
            auto queue = alloc.allocate(1);

            mini_queue_allocator_traits::construct(alloc, queue);
            queue->mElements  = mAllocator.allocate(new_size);
            queue->mBegin     = queue->mElements;
            queue->mUsedBegin = queue->mBegin;
            queue->mCurrent   = queue->mBegin;
            queue->mEnd       = queue->mBegin + new_size;

            return queue;
        }

        void Deallocate_mini_queue(MiniQueue *queue) {
            const auto queue_size = Get_mini_queue_capacity(queue);

            mAllocator.deallocate(queue->mBegin, queue_size);

            auto alloc = mini_queue_allocator {};
            mini_queue_allocator_traits::destroy(alloc, queue);
            alloc.deallocate(queue, 1);
        }

        [[nodiscard]] inline static constexpr size_type max_size() noexcept {
            return static_cast< size_type >(std::numeric_limits< size_type >::max());
        }

        [[nodiscard]] inline static constexpr auto Calculate_new_capacity(const size_type new_size) noexcept {
            // Geometry growth MSVC's vector algorithm
            constexpr const auto size_limit = max_size();
            const auto           current_capacity =
                new_size - 1; // Queue would allocate if and only if an element is pushed when it's full

            if (current_capacity > size_limit - current_capacity / 2) {
                return std::max(Min_queue_size, size_limit);
            }

            const size_type Size = current_capacity + current_capacity / 2;

            if (Size < new_size) {
                return std::max(Min_queue_size, new_size);
            }

            return std::max(Min_queue_size, Size);
        }

        MiniQueue *mQueue { nullptr };
        MiniQueue *mQueueCurrent { nullptr };
        MiniQueue *mQueueEnd { nullptr };

        allocator_type     mAllocator {};
        mutable std::mutex mMutex {};
    };

} // namespace concurrent

#endif // CONCURRENT_QUEUE_HPP