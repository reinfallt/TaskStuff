#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant> // Needed for std::monostate (Should perhaps just define my own. It is literally just an empty struct.)
#include <vector>

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

    class FutureError : public std::runtime_error
    {
    private:

        FutureErrorCode _error_code_;

    public:

        FutureError(FutureErrorCode errorCode, const char* msg)
            : std::runtime_error(msg)
            , _error_code_(errorCode)
        { }

        FutureErrorCode ErrorCode() const
        {
            return _error_code_;
        }
    };

    class ExceptionAggregate : public std::exception
    {
    private:

        std::vector<std::exception_ptr> _exceptions_;

    public:

        ExceptionAggregate()
        { }

        void Add(std::exception_ptr e)
        {
            _exceptions_.push_back(e);
        }
    };

    template <typename ValueT>
    class _InternalFutureBase;

    template <typename ValueT>
    class _InternalPromiseBase;

    template <typename ValueT>
    class Future;

    template <typename ValueT>
    class Promise;

    template <typename ValueT>
    class PersistentFuture;

    template <typename ValueT>
    struct _InternalContinuationHolderIfc
    {
        virtual void Call(ValueT val) = 0;
        virtual void SetException(std::exception_ptr e) = 0;
        virtual ~_InternalContinuationHolderIfc() {}
    };

    template <>
    struct _InternalContinuationHolderIfc<void>
    {
        virtual void Call() = 0;
        virtual void SetException(std::exception_ptr e) = 0;
        virtual ~_InternalContinuationHolderIfc() {}
    };

    template <typename FnT, typename ValueT>
    struct _InternalContinuationHolder : public _InternalContinuationHolderIfc<ValueT>
    {
        using result_type = std::invoke_result_t<FnT, ValueT>;

        FnT                  _internal_continuation_function_;
        Promise<result_type> _result_promise_;

        _InternalContinuationHolder(FnT fn, Promise<result_type> resultPromise)
            : _internal_continuation_function_(std::move(fn))
            , _result_promise_(std::move(resultPromise))
        {
        }

        _InternalContinuationHolder(_InternalContinuationHolder const&) = delete;
        _InternalContinuationHolder& operator=(_InternalContinuationHolder const&) = delete;

        void Call(ValueT val) override
        {
            try
            {
                if constexpr (std::is_same_v<result_type, void>)
                {
                    if constexpr (std::is_reference_v<ValueT>)
                        _internal_continuation_function_(val);
                    else
                        _internal_continuation_function_(std::move(val));

                    _result_promise_.SetDone();
                }
                else
                {
                    if constexpr (std::is_reference_v<ValueT>)
                        _result_promise_.SetValue(_internal_continuation_function_(val));
                    else
                        _result_promise_.SetValue(_internal_continuation_function_(std::move(val)));
                }
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

    // When the continuation function itself returns another Future object
    template <typename FnT, typename ValueT>
    struct _InternalChainedContinuationHolder : public _InternalContinuationHolderIfc<ValueT>
    {
        using internal_future_type = std::invoke_result_t<FnT, ValueT>;
        using result_type = typename internal_future_type::value_type;

        FnT                  _internal_continuation_function_;
        Promise<result_type> _result_promise_;

        _InternalChainedContinuationHolder(FnT fn, Promise<result_type> resultPromise)
            : _internal_continuation_function_(std::move(fn))
            , _result_promise_(std::move(resultPromise))
        {
        }

        _InternalChainedContinuationHolder(_InternalChainedContinuationHolder const&) = delete;
        _InternalChainedContinuationHolder& operator=(_InternalChainedContinuationHolder const&) = delete;

        void Call(ValueT val) override
        {
            internal_future_type lowerFuture;
            try
            {
                if constexpr (std::is_reference_v<ValueT>)
                    lowerFuture = _internal_continuation_function_(val);
                else
                    lowerFuture = _internal_continuation_function_(std::move(val));
            }
            catch (...)
            {
                _result_promise_.SetException(std::current_exception());
                return;
            }

            // "Chain" our promise to the Future returned from the continuation function
            lowerFuture._setChainedPromise(std::move(_result_promise_));
        }

        void SetException(std::exception_ptr e) override
        {
            _result_promise_.SetException(e);
        }
    };

    template <typename FnT>
    struct _InternalContinuationHolder<FnT, void> : public _InternalContinuationHolderIfc<void>
    {
        using result_type = std::invoke_result_t<FnT>;

        FnT                  _internal_continuation_function_;
        Promise<result_type> _result_promise_;

        _InternalContinuationHolder(FnT fn, Promise<result_type> resultPromise)
            : _internal_continuation_function_(std::move(fn))
            , _result_promise_(std::move(resultPromise))
        {
        }

        _InternalContinuationHolder(_InternalContinuationHolder const&) = delete;
        _InternalContinuationHolder& operator=(_InternalContinuationHolder const&) = delete;

        void Call() override
        {
            try
            {
                if constexpr (std::is_same_v<result_type, void>)
                {
                    _internal_continuation_function_();
                    _result_promise_.SetDone();
                }
                else
                {
                    auto result = _internal_continuation_function_();
                    _result_promise_.SetValue(std::move(result));
                }
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

    // When the continuation function itself returns another Future object
    template <typename FnT>
    struct _InternalChainedContinuationHolder<FnT, void> : public _InternalContinuationHolderIfc<void>
    {
        using internal_future_type = std::invoke_result_t<FnT>;
        using result_type = typename internal_future_type::value_type;

        FnT                  _internal_continuation_function_;
        Promise<result_type> _result_promise_;

        _InternalChainedContinuationHolder(FnT fn, Promise<result_type> resultPromise)
            : _internal_continuation_function_(std::move(fn))
            , _result_promise_(std::move(resultPromise))
        {
        }

        _InternalChainedContinuationHolder(_InternalChainedContinuationHolder const&) = delete;
        _InternalChainedContinuationHolder& operator=(_InternalChainedContinuationHolder const&) = delete;

        void Call() override
        {
            internal_future_type lowerFuture;
            try
            {
                lowerFuture = _internal_continuation_function_();
            }
            catch (...)
            {
                _result_promise_.SetException(std::current_exception());
                return;
            }

            // "Chain" our promise to the Future returned from the continuation function
            lowerFuture._setChainedPromise(std::move(_result_promise_));
        }

        void SetException(std::exception_ptr e) override
        {
            _result_promise_.SetException(e);
        }
    };

    template <typename T>
    struct _is_future { static constexpr bool value = false; };

    template <typename T>
    struct _is_future<Future<T>> { static constexpr bool value = true; };

    template <typename T>
    struct _is_not_future { static constexpr bool value = true; };

    template <typename T>
    struct _is_not_future<Future<T>> { static constexpr bool value = false; };

    template <typename ValueT>
    class PromiseFutureState;

    template <typename ValueT>
    class _InternalFutureBase
    {
    protected:

        PromiseFutureState<ValueT>* _state_;

        template <typename T>
        friend class PromiseFutureState;

        template <typename T, typename U>
        friend class _InternalChainedContinuationHolder;

        void _setChainedPromise(Promise<ValueT> chainedPromise)
        {
            if (!_state_)
            {
                chainedPromise.SetException(FutureError(FutureErrorCode::NoState, "Future has no state!"));
                return;
            }

            // Scope for lock
            {
                std::unique_lock lck(_state_->_mtx_value_);

                if (_state_->_exception_)
                {
                    chainedPromise.SetException(_state_->_exception_);
                }
                else if (_state_->_value_.has_value())
                {
                    if constexpr (std::is_same_v<ValueT, void>)
                        chainedPromise.SetDone();
                    else
                        chainedPromise.SetValue(std::move(*_state_->_value_));
                }
                else
                {
                    _state_->_chained_promise_ = std::move(chainedPromise);
                }
            }

            _state_->_release();
            _state_ = nullptr;
        }

        _InternalFutureBase(_InternalFutureBase const&) = delete;
        _InternalFutureBase& operator=(_InternalFutureBase const&) = delete;

        _InternalFutureBase() noexcept
            : _state_(nullptr)
        { }

    public:

        using value_type = ValueT;

        ~_InternalFutureBase()
        {
            if (_state_)
                _state_->_release();
        }

        bool Valid() const
        {
            return _state_ != nullptr;
        }

        ValueT Get()
        {
            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            std::conditional_t<std::is_same_v<ValueT, void>, std::monostate, ValueT> val;

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

            if constexpr (!std::is_same_v<ValueT, void>)
                return val;
        }

        // If the continuation function itself returns another Future object,
        // we don't want to end up with something that looks like this on the top level: Future<Future<Future<Future<int>>>>.
        // This specialization causes the Future on the top level to still be a simple Future<int> that can be awaited.
        template<typename FnT>
        std::enable_if_t<
            _is_future<std::invoke_result_t<FnT, ValueT>>::value,
            std::invoke_result_t<FnT, ValueT>> Then(FnT fn)
        {
            using resultType = typename std::invoke_result_t<FnT, ValueT>::value_type;

            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            Future<resultType> continuationFuture;

            // Scope for lock
            {
                std::unique_lock lck(_state_->_mtx_value_);

                if (_state_->_exception_)
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();
                    continuationPromise.SetException(_state_->_exception_);
                }
                else if (_state_->_value_.has_value())
                {
                    // If the promise has already been fulfilled,
                    // call the continuation function immediately
                    try
                    {
                        continuationFuture = fn(std::move(*_state_->_value_));
                    }
                    catch (...)
                    {
                        Promise<resultType> continuationPromise;
                        continuationFuture = continuationPromise.GetFuture();
                        continuationPromise.SetException(std::current_exception());
                    }
                }
                else
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();
                    _state_->_setChainedContinuation(std::move(fn), std::move(continuationPromise));
                }
            }

            _state_->_release();
            _state_ = nullptr;

            return continuationFuture;
        }

        template<typename FnT>
        std::enable_if_t<
            _is_not_future<std::invoke_result_t<FnT, ValueT>>::value,
            Future<std::invoke_result_t<FnT, ValueT>>> Then(FnT fn)
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
                        if constexpr (std::is_same_v<resultType, void>)
                        {
                            fn(std::move(*_state_->_value_));
                            continuationPromise.SetDone();
                        }
                        else
                        {
                            auto result = fn(std::move(*_state_->_value_));
                            continuationPromise.SetValue(std::move(result));
                        }
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
    class Future : public _InternalFutureBase<ValueT>
    {
    private:

        Future(PromiseFutureState<ValueT>* state)
        {
            _InternalFutureBase<ValueT>::_state_ = state;
        }

        friend class _InternalPromiseBase<ValueT>;

    public:

        Future(Future const&) = delete;
        Future& operator=(Future const&) = delete;

        Future() noexcept
        { }

        Future(Future&& other) noexcept
        {
            _InternalFutureBase<ValueT>::_state_ = other._state_;
            other._state_ = nullptr;
        }

        Future& operator=(Future&& other) noexcept
        {
            if (_InternalFutureBase<ValueT>::_state_)
                _InternalFutureBase<ValueT>::_state_->_release();

            _InternalFutureBase<ValueT>::_state_ = other._state_;
            other._state_ = nullptr;
            return *this;
        }

        Future(ValueT value)
        {
            _InternalFutureBase<ValueT>::_state_ = new PromiseFutureState<ValueT>();
            _InternalFutureBase<ValueT>::_state_->_value_ = std::move(value);
        }
    };

    template <>
    class Future<void> : public _InternalFutureBase<void>
    {
    private:

        Future(PromiseFutureState<void>* state)
        {
            _state_ = state;
        }

        friend class _InternalPromiseBase<void>;

    public:

        Future(Future const&) = delete;
        Future& operator=(Future const&) = delete;

        Future() noexcept
        { }

        Future(Future&& other) noexcept
        {
            _state_ = other._state_;
            other._state_ = nullptr;
        }

        Future& operator=(Future&& other) noexcept;

        template<typename FnT>
        void OnException(FnT fn);
    };

    template <typename ValueT>
    class _InternalPromiseBase
    {
    protected:

        PromiseFutureState<ValueT>* _state_;
        bool _future_retrieved_;
        bool _value_set_;

        _InternalPromiseBase(_InternalPromiseBase const&) = delete;
        _InternalPromiseBase& operator=(_InternalPromiseBase const&) = delete;

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

        _InternalPromiseBase()
            : _state_(new PromiseFutureState<ValueT>())
            , _future_retrieved_(false)
            , _value_set_(false)
        {
        }

    public:

        ~_InternalPromiseBase()
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
            else if (_state_->_chained_promise_)
            {
                _state_->_chained_promise_->SetException(exceptionPtr);
            }
            else if (_state_->_on_exception_)
            {
                (*_state_->_on_exception_)(exceptionPtr);
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
    class Promise : public _InternalPromiseBase<ValueT>
    {
    public:

        Promise()
        { }

        Promise(Promise&& other) noexcept
        {
            _InternalPromiseBase<ValueT>::_state_ = other._state_;
            _InternalPromiseBase<ValueT>::_future_retrieved_ = other._future_retrieved_;
            _InternalPromiseBase<ValueT>::_value_set_ = other._value_set_;

            other._state_ = nullptr;
            other._future_retrieved_ = false;
            other._value_set_ = false;
        }

        Promise& operator=(Promise&& other) noexcept
        {
            _InternalPromiseBase<ValueT>::_clear();

            _InternalPromiseBase<ValueT>::_state_ = other._state_;
            _InternalPromiseBase<ValueT>::_future_retrieved_ = other._future_retrieved_;
            _InternalPromiseBase<ValueT>::_value_set_ = other._value_set_;

            other._state_ = nullptr;
            other._future_retrieved_ = false;
            other._value_set_ = false;

            return *this;
        }

        void SetValue(ValueT value)
        {
            if (_InternalPromiseBase<ValueT>::_value_set_)
            {
                throw FutureError(FutureErrorCode::PromiseAlreadySatisfied, "Promise value already set!");
            }

            if (!_InternalPromiseBase<ValueT>::_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
            }

            std::unique_lock lck(_InternalPromiseBase<ValueT>::_state_->_mtx_value_);
            _InternalPromiseBase<ValueT>::_value_set_ = true;

            // If a continuation function is set, call it with the value
            if (_InternalPromiseBase<ValueT>::_state_->_continuation_)
            {
                _InternalPromiseBase<ValueT>::_state_->_continuation_->Call(std::move(value));
            }
            else if (_InternalPromiseBase<ValueT>::_state_->_chained_promise_)
            {
                _InternalPromiseBase<ValueT>::_state_->_chained_promise_->SetValue(std::move(value));
            }
            else // Otherwise set the value in the state normally
            {
                _InternalPromiseBase<ValueT>::_state_->_value_ = std::move(value);
                _InternalPromiseBase<ValueT>::_state_->_cv_value_.notify_all();
            }
        }
    };

    template <>
    class Promise<void> : public _InternalPromiseBase<void>
    {
    public:

        Promise()
        {
        }

        Promise(Promise&& other) noexcept
        {
            _state_ = other._state_;
            _future_retrieved_ = other._future_retrieved_;
            _value_set_ = other._value_set_;

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

            return *this;
        }

        void SetDone();
    };

    template <typename ValueT>
    class PromiseFutureState
    {
    private:

        std::atomic_int _ref_count_ = 1;

        std::mutex                                                                              _mtx_value_;
        std::condition_variable                                                                 _cv_value_;
        std::optional<std::conditional_t<std::is_same_v<ValueT, void>, std::monostate, ValueT>> _value_;
        std::exception_ptr                                                                      _exception_;
        std::unique_ptr<_InternalContinuationHolderIfc<ValueT>>                                 _continuation_;
        std::optional<Promise<ValueT>>                                                          _chained_promise_;
        std::optional<std::function<void(std::exception_ptr)>>                                  _on_exception_;

        void _addRef() { ++_ref_count_; }

        void _release()
        {
            if (0 == --_ref_count_)
            {
                delete this;
            }
        }


        template <typename FnT>
        void _setContinuation(FnT fn, Promise<std::invoke_result_t<FnT, ValueT>> prom)
        {
            _continuation_ = std::make_unique<_InternalContinuationHolder<FnT, ValueT>>(std::move(fn), std::move(prom));
        }

        template <typename FnT>
        void _setChainedContinuation(FnT fn, Promise<typename std::invoke_result_t<FnT, ValueT>::value_type> prom)
        {
            _continuation_ = std::make_unique<_InternalChainedContinuationHolder<FnT, ValueT>>(std::move(fn), std::move(prom));
        }

        friend class _InternalFutureBase<ValueT>;
        friend class _InternalPromiseBase<ValueT>;
        friend class Future<ValueT>;
        friend class Promise<ValueT>;
        //friend class PersistentFuture<ValueT>;
    };

    template<typename FnT>
    void Future<void>::OnException(FnT fn)
    {
        if (!_state_)
        {
            // Not really sure if we should throw here or call the function
            fn(std::make_exception_ptr(FutureError(FutureErrorCode::NoState, "Future has no state!")));
            return;
        }

        // Scope for lock
        {
            std::unique_lock lck(_state_->_mtx_value_);

            if (_state_->_exception_)
            {
                fn(_state_->_exception_);
            }
            else if (_state_->_value_.has_value())
            {
                // Already complete
            }
            else
            {
                _state_->_on_exception_ = std::move(fn);
            }
        }

        _state_->_release();
        _state_ = nullptr;
    }

    template <typename ValueT>
    Future<std::vector<ValueT>> WhenAll(std::span<Future<ValueT>> futures)
    {
        struct WhenAllContext
        {
            std::vector<ValueT> values;
            std::atomic_int countdown;
            Promise<std::vector<ValueT>> promise_all;
            std::vector<std::exception_ptr> exceptions;
            std::atomic_int exception_count;
        };

        auto whenAllContext = std::make_shared<WhenAllContext>();
        whenAllContext->values.resize(futures.size());
        whenAllContext->exceptions.resize(futures.size());
        whenAllContext->countdown = futures.size();
        whenAllContext->exception_count = 0;

        for (size_t i = 0; i < futures.size(); ++i)
        {
            futures[i].Then([whenAllContext = whenAllContext, idx = i](ValueT val)
                {
                    whenAllContext->values[idx] = std::move(val);
                    if (0 == --whenAllContext->countdown) // The last underlying future to complete will set the value in the overall promise
                    {
                        if (whenAllContext->exception_count > 0)
                        {
                            ExceptionAggregate exceptionAggregate;
                            for (std::exception_ptr e : whenAllContext->exceptions)
                            {
                                if (e)
                                {
                                    exceptionAggregate.Add(e);
                                }
                            }
                            whenAllContext->promise_all.SetException(std::move(exceptionAggregate));
                        }
                        else
                        {
                            whenAllContext->promise_all.SetValue(std::move(whenAllContext->values));
                        }
                    }
                }).OnException([whenAllContext = whenAllContext, idx = i](std::exception_ptr e)
                    {
                        whenAllContext->exceptions[idx] = e;
                        ++whenAllContext->exception_count;

                        // The regular continuation won't be called if there is an exception so we need to do the countdown here to not get hanged
                        if (0 == --whenAllContext->countdown)
                        {
                            ExceptionAggregate exceptionAggregate;
                            for (std::exception_ptr e : whenAllContext->exceptions)
                            {
                                if (e)
                                {
                                    exceptionAggregate.Add(e);
                                }
                            }
                            whenAllContext->promise_all.SetException(std::move(exceptionAggregate));
                        }
                    });
        }

        return whenAllContext->promise_all.GetFuture();
    }

    template <size_t current, size_t end, typename FnT>
    void foreach_number(FnT fn)
    {
        if constexpr (current < end)
        {
            fn(std::integral_constant<size_t, current>());
            foreach_number<current + 1, end, FnT>(fn);
        }
    }

    template <typename... ValuesT>
    Future<std::tuple<ValuesT...>> WhenAll(Future<ValuesT>... futures)
    {
        struct WhenAllContext
        {
            std::tuple<Future<ValuesT>...> tuple_futures;
            std::tuple<ValuesT...> values;
            std::atomic_int countdown;
            Promise<std::tuple<ValuesT...>> promise_all;
            std::array<std::exception_ptr, sizeof...(ValuesT)> exceptions;
            std::atomic_int exception_count;
        };

        auto whenAllContext = std::make_shared<WhenAllContext>();
        whenAllContext->countdown = sizeof...(ValuesT);
        whenAllContext->tuple_futures = std::tuple<Future<ValuesT>...>{ std::move(futures)... };
        whenAllContext->exception_count = 0;

        foreach_number<0, sizeof...(ValuesT)>([whenAllContext = whenAllContext](auto idx)
            {
                auto& current_future = std::get<idx>(whenAllContext->tuple_futures);
                auto& current_value = std::get<idx>(whenAllContext->values);

                current_future.Then([whenAllContext = whenAllContext, v = &current_value](auto val)
                    {
                        *v = std::move(val);
                        if (0 == --whenAllContext->countdown) // The last underlying future to complete will set the value in the overall promise
                        {
                            if (whenAllContext->exception_count > 0)
                            {
                                ExceptionAggregate exceptionAggregate;
                                for (std::exception_ptr e : whenAllContext->exceptions)
                                {
                                    if (e)
                                    {
                                        exceptionAggregate.Add(e);
                                    }
                                }
                                whenAllContext->promise_all.SetException(std::move(exceptionAggregate));
                            }
                            else
                            {
                                whenAllContext->promise_all.SetValue(std::move(whenAllContext->values));
                            }
                        }
                    }).OnException([whenAllContext = whenAllContext, idx = idx](std::exception_ptr e)
                        {
                            whenAllContext->exceptions[idx] = e;
                            ++whenAllContext->exception_count;

                            // The regular continuation won't be called if there is an exception so we need to do the countdown here to not get hanged
                            if (0 == --whenAllContext->countdown)
                            {
                                ExceptionAggregate exceptionAggregate;
                                for (std::exception_ptr e : whenAllContext->exceptions)
                                {
                                    if (e)
                                    {
                                        exceptionAggregate.Add(e);
                                    }
                                }
                                whenAllContext->promise_all.SetException(std::move(exceptionAggregate));
                            }
                        });
            });

        return whenAllContext->promise_all.GetFuture();
    }

    // "Persistent" future that can be accessed multiple times and have multiple continuation functions
    template <typename ValueT>
    class PersistentFuture
    {
    private:

        struct _persistentState
        {
            std::mutex              _mtx_value_;
            std::condition_variable _cv_value_;
            std::optional<ValueT>   _value_;
            std::exception_ptr      _exception_;

            std::vector<std::unique_ptr<_InternalContinuationHolderIfc<ValueT const&>>> _continuations_;
        };

        std::shared_ptr<_persistentState> _persistent_state_;

        template <typename FnT>
        void _addContinuation(FnT fn, Promise<std::invoke_result_t<FnT, ValueT const&>> prom)
        {
            _persistent_state_->_continuations_.push_back(
                std::make_unique<_InternalContinuationHolder<FnT, ValueT const&>>(std::move(fn), std::move(prom)));
        }

        template <typename FnT>
        void _addChainedContinuation(FnT fn, Promise<typename std::invoke_result_t<FnT, ValueT const&>::value_type> prom)
        {
            _persistent_state_->_continuations_.push_back(
                std::make_unique<_InternalChainedContinuationHolder<FnT, ValueT const&>>(std::move(fn), std::move(prom)));
        }

    public:

        PersistentFuture()
            : _persistent_state_(nullptr)
        { }

        PersistentFuture(Future<ValueT> fut)
            : _persistent_state_(std::make_shared<_persistentState>())
        {
            // Set a "proxy" continuation function on the base future that will set
            // the value in the persistent state and call all continuation functions.
            fut.Then([persistent_state = _persistent_state_](ValueT value)
                {
                    std::unique_lock lock(persistent_state->_mtx_value_);
                    persistent_state->_value_ = std::move(value);

                    for (auto& fn : persistent_state->_continuations_)
                        fn->Call(*persistent_state->_value_);

                    persistent_state->_continuations_.clear();

                    persistent_state->_cv_value_.notify_all();
                });
        }

        ValueT const& Get()
        {
            std::unique_lock lck(_persistent_state_->_mtx_value_);

            while (!_persistent_state_->_value_.has_value() && _persistent_state_->_exception_)
            {
                _persistent_state_->_cv_value_.wait(lck);
            }

            if (_persistent_state_->_exception_)
                std::rethrow_exception(_persistent_state_->_exception_);

            return *_persistent_state_->_value_;
        }

        // If the continuation function itself returns another Future object,
        // we don't want to end up with something that looks like this on the top level: Future<Future<Future<Future<int>>>>.
        // This specialization causes the Future on the top level to still be a simple Future<int> that can be awaited.
        template<typename FnT>
        std::enable_if_t<
            _is_future<std::invoke_result_t<FnT, ValueT const&>>::value,
            std::invoke_result_t<FnT, ValueT>> Then(FnT fn)
        {
            using resultType = typename std::invoke_result_t<FnT, ValueT const&>::value_type;

            if (!_persistent_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            Future<resultType> continuationFuture;

            // Scope for lock
            {
                std::unique_lock lck(_persistent_state_->_mtx_value_);

                if (_persistent_state_->_exception_)
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();
                    continuationPromise.SetException(_persistent_state_->_exception_);
                }
                else if (_persistent_state_->_value_.has_value())
                {
                    // If the promise has already been fulfilled,
                    // call the continuation function immediately
                    try
                    {
                        continuationFuture = fn(*_persistent_state_->_value_);
                    }
                    catch (...)
                    {
                        Promise<resultType> continuationPromise;
                        continuationFuture = continuationPromise.GetFuture();
                        continuationPromise.SetException(std::current_exception());
                    }
                }
                else
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();
                    _addChainedContinuation(std::move(fn), std::move(continuationPromise));
                }
            }

            return continuationFuture;
        }

        template<typename FnT>
        std::enable_if_t<
            _is_not_future<std::invoke_result_t<FnT, ValueT const&>>::value,
            Future<std::invoke_result_t<FnT, ValueT>>> Then(FnT fn)
        {
            using resultType = std::invoke_result_t<FnT, ValueT const&>;

            if (!_persistent_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            Promise<resultType> continuationPromise;
            auto continuationFuture = continuationPromise.GetFuture();

            std::unique_lock lck(_persistent_state_->_mtx_value_);

            if (_persistent_state_->_exception_)
            {
                continuationPromise.SetException(_persistent_state_->_exception_);
            }
            else if (_persistent_state_->_value_.has_value())
            {
                // If the promise has already been fulfilled,
                // call the continuation function immediately
                try
                {
                    if constexpr (std::is_same_v<resultType, void>)
                    {
                        fn(*_persistent_state_->_value_);
                        continuationPromise.SetDone();
                    }
                    else
                    {
                        auto result = fn(*_persistent_state_->_value_);
                        continuationPromise.SetValue(std::move(result));
                    }
                }
                catch (...)
                {
                    continuationPromise.SetException(std::current_exception());
                }
            }
            else
            {
                _addContinuation(std::move(fn), std::move(continuationPromise));
            }

            return continuationFuture;
        }
    };
}
