#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace TaskStuff
{
    enum class FutureErrorCode : int32_t
    {
        None                    = 0,

        BrokenPromise           = 1,
        FutureAlreadyRetrieved  = 2,
        PromiseAlreadySatisfied = 3,
        NoState                 = 4
    };

    class FutureError : public std::exception
    {
    private:

        FutureErrorCode _error_code_;

    public:

        FutureError(FutureErrorCode errorCode, const char* msg)
            : std::exception(msg)
            , _error_code_(errorCode)
        { }

        FutureErrorCode ErrorCode() const
        {
            return _error_code_;
        }
    };

    template <typename ValueT>
    class Future;

    template <typename ValueT>
    class Promise;

    template <typename ValueT>
    class PromiseFutureState
    {
    private:

        std::atomic_int _ref_count_ = 1;

        struct InternalContinuationHolderIfc
        {
            virtual void Call(ValueT val) = 0;
            virtual void SetException(std::exception_ptr e) = 0;
        };

        std::mutex                                     _mtx_value_;
        std::condition_variable                        _cv_value_;
        std::optional<ValueT>                          _value_;
        std::exception_ptr                             _exception_;
        std::unique_ptr<InternalContinuationHolderIfc> _continuation_;

        void _addRef() { ++_ref_count_; }

        void _release()
        {
            if (0 == --_ref_count_)
            {
                delete this;
            }
        }

        template <typename FnT>
        struct InternalContinuationHolder : public InternalContinuationHolderIfc
        {
            using result_type = std::invoke_result_t< FnT, ValueT>;

            FnT                  _internal_continuation_function_;
            Promise<result_type> _result_promise_;

            InternalContinuationHolder(FnT fn, Promise<result_type> resultPromise)
                : _internal_continuation_function_(std::move(fn))
                , _result_promise_(std::move(resultPromise))
            {
            }

            InternalContinuationHolder(InternalContinuationHolder const&) = delete;
            InternalContinuationHolder& operator=(InternalContinuationHolder const&) = delete;

            void Call(ValueT val) override
            {
                try
                {
                    auto result = _internal_continuation_function_(std::move(val));
                    _result_promise_.SetValue(std::move(result));
                }
                catch (...)
                {
                    _result_promise_.SetException(std::current_exception());
                }
            }

            void SetException(std::exception_ptr e) override
            {
                _result_promise_.SetException(e);
            }
        };

        template <typename FnT, typename PromiseT>
        void _setContinuation(FnT fn, PromiseT prom)
        {
            _continuation_ = std::make_unique<InternalContinuationHolder<FnT>>(std::move(fn), std::move(prom));
        }

        friend class Future<ValueT>;
        friend class Promise<ValueT>;
    };

    template <typename ValueT>
    class Future
    {
    private:

        PromiseFutureState<ValueT>* _state_;

        Future(PromiseFutureState<ValueT>* state)
            : _state_(state)
        { }

        friend class Promise<ValueT>;

    public:

        Future(Future const&) = delete;
        Future& operator=(Future const&) = delete;

        Future(Future&& other) noexcept :
            _state_(other._state_)
        {
            other._state_ = nullptr;
        }

        Future& operator=(Future&& other)
        {
            _state_ = other._state_;
            other._state_ = nullptr;
        }

        Future(ValueT value) :
            _state_(new PromiseFutureState<ValueT>())
        {
            _state_->_value_ = std::move(value);
        }

        ~Future()
        {
            if (_state_)
                _state_->_release();
        }

        ValueT Get()
        {
            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            ValueT val;

            // Scope for lock
            {
                std::unique_lock lck(_state_->_mtx_value_);

                while (!_state_->_value_.has_value() && !_state_->_exception_)
                {
                    _state_->_cv_value_.wait(lck);
                }

                if (_state_->_exception_)
                {
                    std::rethrow_exception(_state_->_exception_);
                }

                val = std::move(*_state_->_value_);
            }

            _state_->_release();
            _state_ = nullptr;

            return val;
        }

        template<typename FnT>
        auto Then(FnT fn)
        {
            using resultType = std::invoke_result_t<FnT, ValueT>;

            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            Promise<resultType> continuationPromise;
            auto continuationFuture = continuationPromise.GetFuture();

            // Scope for lock
            {
                std::unique_lock lck(_state_->_mtx_value_);
                
                if (_state_->_exception_)
                {
                    continuationPromise.SetException(_state_->_exception_);
                }
                else if (_state_->_value_.has_value())
                {
                    // If the promise has already been fulfilled,
                    // call the continuation function immediately
                    try
                    {
                        auto result = fn(std::move(*_state_->_value_));
                        continuationPromise.SetValue(std::move(result));
                    }
                    catch (...)
                    {
                        continuationPromise.SetException(std::current_exception());
                    }
                }
                else
                {
                    _state_->_setContinuation(std::move(fn), std::move(continuationPromise));
                }
            }

            _state_->_release();
            _state_ = nullptr;

            return continuationFuture;
        }
    };

    template <typename ValueT>
    class Promise
    {
    private:

        PromiseFutureState<ValueT>* _state_;
        bool _future_retrieved_;
        bool _value_set_;

        Promise(Promise const&) = delete;
        Promise& operator=(Promise const&) = delete;

        void _clear()
        {
            if (_state_)
            {
                if (!_value_set_)
                {
                    SetException(FutureError(FutureErrorCode::BrokenPromise, "Promise was broken!"));
                }

                _state_->_release();
                _state_ = nullptr;
            }
        }

    public:

        Promise()
            : _state_(new PromiseFutureState<ValueT>())
            , _future_retrieved_(false)
            , _value_set_(false)
        { }

        Promise(Promise && other) noexcept
            : _state_(other._state_)
            , _future_retrieved_(other._future_retrieved_)
            , _value_set_(other._value_set_)
        {
            other._state_ = nullptr;
            other._future_retrieved_ = false;
            other._value_set_ = false;
        }

        Promise& operator=(Promise&& other) noexcept
        {
            _clear();

            _state_ = other._state_;
            _future_retrieved_ = other._future_retrieved_;
            _value_set_ = other._value_set_;

            other._state_ = nullptr;
            other._future_retrieved_ = false;
            other._value_set_ = false;
        }

        ~Promise()
        {
            _clear();
        }

        Future<ValueT> GetFuture()
        {
            if (_future_retrieved_)
            {
                throw FutureError(FutureErrorCode::FutureAlreadyRetrieved, "Future already retrieved!");
            }

            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
            }

            _future_retrieved_ = true;
            _state_->_addRef();

            return Future<ValueT>(_state_);
        }

        void SetValue(ValueT value)
        {
            if (_value_set_)
            {
                throw FutureError(FutureErrorCode::PromiseAlreadySatisfied, "Promise value already set!");
            }

            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
            }

            std::unique_lock lck(_state_->_mtx_value_);
            _value_set_ = true;

            // If a continuation function is set, call it with the value
            if (_state_->_continuation_)
            {
                _state_->_continuation_->Call(std::move(value));
            }
            else // Otherwise set the value in the state normally
            {
                _state_->_value_ = std::move(value);
                _state_->_cv_value_.notify_all();
            }
        }

        void SetException(std::exception_ptr exceptionPtr)
        {
            if (_value_set_)
            {
                throw FutureError(FutureErrorCode::PromiseAlreadySatisfied, "Promise value already set!");
            }

            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
            }

            std::unique_lock lck(_state_->_mtx_value_);
            _value_set_ = true;

            if (_state_->_continuation_)
            {
                _state_->_continuation_->SetException(exceptionPtr);
            }
            else
            {
                _state_->_exception_ = exceptionPtr;
                _state_->_cv_value_.notify_all();
            }
        }

        template <typename ExceptionT>
        void SetException(ExceptionT exception)
        {
            SetException(std::make_exception_ptr(exception));
        }
    };

    template <typename ValueT>
    Future<std::vector<ValueT>> WhenAll(std::span<Future<ValueT>> futures)
    {
        struct WhenAllContext
        {
            std::vector<ValueT> values;
            std::atomic_int countdown;
            Promise<std::vector<ValueT>> promise_all;
        };
        
        auto whenAllContext = std::make_shared<WhenAllContext>();
        whenAllContext->values.resize(futures.size());
        whenAllContext->countdown = futures.size();

        for (size_t i = 0; i < futures.size(); ++i)
        {
            futures[i].Then([whenAllContext = whenAllContext, idx = i](ValueT val)
                {
                    whenAllContext->values[idx] = std::move(val);
                    if (0 == --whenAllContext->countdown) // The last underlying future to complete will set the value in the overall promise
                    {
                        whenAllContext->promise_all.SetValue(std::move(whenAllContext->values));
                    }
                });
        }

        return whenAllContext->promise_all.GetFuture();
    }

    template <size_t index = 0, typename FnT, typename... Types1, typename... Types2>
    void foreach_tuple_pair_element(std::tuple<Types1...>& tup1, std::tuple<Types2...>& tup2, FnT fn)
    {
        // Could we use std::zip for this in C++23?

        static_assert(sizeof...(Types1) == sizeof...(Types2), "Mismatching number of tuple elements");

        if constexpr (index < sizeof...(Types1))
        {
            fn(std::get<index>(tup1), std::get<index>(tup2));
            foreach_tuple_pair_element<index + 1>(tup1, tup2, fn);
        }
    }

    template <typename... ValuesT>
    Future<std::tuple<ValuesT...>> WhenAll(Future<ValuesT>... futures)
    {
        struct WhenAllContext
        {
            std::tuple<ValuesT...> values;
            std::atomic_int countdown;
            Promise<std::tuple<ValuesT...>> promise_all;
        };

        auto whenAllContext = std::make_shared<WhenAllContext>();
        whenAllContext->countdown = sizeof...(ValuesT);

        std::tuple<Future<ValuesT>...> tupleFutures { std::move(futures)... };

        foreach_tuple_pair_element(
            tupleFutures,
            whenAllContext->values,
            [whenAllContext = whenAllContext] <typename T> (Future<T>& f, T& v)
        {
            // TODO: Aggregate exceptions from underlying futures
            f.Then([whenAllContext = whenAllContext, v = &v] (T val)
            {
                *v = std::move(val);
                if (0 == --whenAllContext->countdown) // The last underlying future to complete will set the value in the overall promise
                {
                    whenAllContext->promise_all.SetValue(std::move(whenAllContext->values));
                }

                // TODO: Support for void futures
                return 0;
            });
        });

        return whenAllContext->promise_all.GetFuture();
    }
}
