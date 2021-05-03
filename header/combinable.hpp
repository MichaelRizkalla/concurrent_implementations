#ifndef COMBINABLE_HPP
#define COMBINABLE_HPP

#include <array>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>
#include <type_traits>
#include <utility>

namespace concurrent {

    template < class Type, std::size_t ThreadCount = 8 >
    class combinable {
      public:
        using value_type = Type;

      private:
        struct Storage {
            std::thread::id mThreadID;
            value_type      mValue;
            Storage*        nextStorage;

            constexpr Storage(std::thread::id threadID, value_type value) : mThreadID(threadID), mValue(value) {}
            constexpr ~Storage() = default;
        };

        template < std::size_t TCount >
        struct Bucket_size_eval {
          private:
            template < std::size_t Count >
            static constexpr std::size_t eval() {
                return 8 > Count / 8 ? 8 : Count / 8;
            }

          public:
            static constexpr std::size_t value = eval< TCount >();
        };

        static constexpr auto Min_size = Bucket_size_eval< ThreadCount >::value;

      public:
        constexpr combinable() { mBuckets.fill(nullptr); }

        constexpr ~combinable() { Finalise(); }

        constexpr void clear() { Finalise(); }

        template < class Functor >
        requires(std::is_invocable_r_v< value_type, Functor, value_type, value_type >) value_type
            combine(Functor func) const {
            Storage*    starting_storage = nullptr;
            std::size_t starting_index   = 0;

            for (auto bucket : mBuckets) { // Look for the first initialized bucket
                ++starting_index;
                if (bucket) {
                    starting_storage = bucket;
                    break;
                }
            }

            if (!starting_storage) { // If no initialized bucket, return default-constructed value
                return value_type {};
            }

            value_type result = starting_storage->mValue;
            starting_storage  = starting_storage->nextStorage;
            while (starting_storage) {
                result           = func(result, starting_storage->mValue);
                starting_storage = starting_storage->nextStorage;
            }

            for (; starting_index < Min_size; ++starting_index) {
                starting_storage = mBuckets[starting_index];
                while (starting_storage) {
                    result           = func(result, starting_storage->mValue);
                    starting_storage = starting_storage->nextStorage;
                }
            }

            return result;
        }

        template < typename Functor >
        requires(std::is_invocable_r_v< void, Functor, value_type >) void combine_each(Functor func) const {
            for (auto bucket : mBuckets) {
                auto storage_unit = bucket;
                while (storage_unit) {
                    func(storage_unit->mValue);
                    storage_unit = storage_unit->nextStorage;
                }
            }
        }

        constexpr value_type& local() {
            auto thread_id                    = std::this_thread::get_id();
            auto [storage_unit, bucket_index] = Get_local_storage_unit(thread_id);

            if (!storage_unit) {
                storage_unit = Add_local_storage_unit(thread_id, bucket_index);
            }

            return storage_unit->mValue;
        }

        constexpr value_type& local(bool& exists) {
            auto thread_id                    = std::this_thread::get_id();
            auto [storage_unit, bucket_index] = Get_local_storage_unit(thread_id);

            if (!storage_unit) {
                storage_unit = Add_local_storage_unit(thread_id, bucket_index);
                exists       = false;
            } else {
                exists = true;
            }

            return storage_unit->mValue;
        }

      private:
        void Finalise() noexcept {
            for (auto& bucket : mBuckets) {
                Delete_storage_units_in_bucket_ptr(bucket);
            }
        }

        void Delete_storage_units_in_bucket_ptr(Storage* bucket) noexcept {
            auto alloc        = std::allocator< Storage > {};
            auto storage_unit = bucket;
            bucket            = nullptr;
            while (storage_unit) {
                auto delete_unit = storage_unit;
                storage_unit     = storage_unit->nextStorage;

                alloc.deallocate(delete_unit, 1);
            }
        }

        constexpr auto Get_local_storage_unit(std::thread::id thread_id) const {
            Storage*    storage_unit = nullptr;
            std::size_t bucket_index = std::hash< std::thread::id > {}(thread_id) % Min_size;

            Storage* current_unit = mBuckets[bucket_index];

            while (current_unit) {
                if (current_unit->mThreadID == thread_id) {
                    return std::make_pair(current_unit, bucket_index);
                }
            }

            return std::make_pair(storage_unit, bucket_index);
        }

        constexpr auto Add_local_storage_unit(std::thread::id thread_id, std::size_t bucket_index) {
            auto alloc  = std::allocator< Storage > {};
            auto bucket = mBuckets[bucket_index];

            auto new_storage = alloc.allocate(1);
            std::allocator_traits< std::allocator< Storage > >::construct(alloc, new_storage, thread_id, value_type {});

            new_storage->nextStorage = bucket;
            mBuckets[bucket_index]   = new_storage;

            return new_storage;
        }

        std::array< Storage*, Min_size > mBuckets;
    };

} // namespace concurrent

#endif // COMBINABLE_HPP
