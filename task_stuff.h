#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace TaskStuff
{
    template <typename T>
    class ReferenceCounted;

    template <typename T>
    ReferenceCounted<T> MakeReferenceCounted();

    template <typename T>
    class ReferenceCounted
    {
    private:

        struct _internalHolder
        {
            std::atomic_int _ref_count_;
            T               _obj_;

            _internalHolder(T obj)
                : _obj_(std::move(obj))
                , _ref_count_(1)
            {
            }

            _internalHolder()
                : _obj_()
                , _ref_count_(1)
            {
            }

            _internalHolder(_internalHolder const&) = delete;
            _internalHolder& operator=(_internalHolder const&) = delete;

            void _addRef() { ++_ref_count_; }

            void _release()
            {
                if (0 == --_ref_count_)
                {
                    delete this;
                }
            }
        };

        _internalHolder* _holder_;

        ReferenceCounted(_internalHolder* holder)
            : _holder_(holder)
        {
        }

        friend ReferenceCounted<T> MakeReferenceCounted<T>();

    public:

        ReferenceCounted(T obj)
            : _holder_(new _internalHolder(std::move(obj)))
        { }

        ReferenceCounted()
            : _holder_(nullptr)
        {
        }

        ~ReferenceCounted()
        {
            if (_holder_)
                _holder_->_release();

            _holder_ = nullptr;
        }

        ReferenceCounted(ReferenceCounted const& other)
            : _holder_(other._holder_)
        {
            if (_holder_)
                _holder_->_addRef();
        }

        ReferenceCounted& operator=(ReferenceCounted const& other)
        {
            if (_holder_)
                _holder_->_release();

            _holder_ = other._holder_;

            if (_holder_)
                _holder_->_addRef();

            return *this;
        }

        T& operator*()
        {
            return _holder_->_obj_;
        }

        const T& operator*() const
        {
            return _holder_->_obj_;
        }

        T* operator->()
        {
            return &_holder_->_obj_;
        }

        const T* operator->() const
        {
            return &_holder_->_obj_;
        }

        operator bool() const
        {
            return !!_holder_;
        }
    };

    template <typename T>
    ReferenceCounted<T> MakeReferenceCounted()
    {
        return ReferenceCounted<T>(new ReferenceCounted<T>::_internalHolder());
    }

    struct VoidPlaceHolder {};

    template <typename FnT, typename ArgumentT>
    struct _internal_invoke_result
    {
        using type = std::invoke_result_t<FnT, ArgumentT>;
    };

    template <typename FnT>
    struct _internal_invoke_result<FnT, void>
    {
        using type = std::invoke_result_t<FnT>;
    };

    template <typename FnT, typename ArgumentT>
    using _internal_invoke_result_t = typename _internal_invoke_result<FnT, ArgumentT>::type;

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

    class _InternalCallableHolder
    {
    private:

        static const size_t INTERNAL_BUFFER_SIZE = 128;
        using InternalBufferType = std::array<uint8_t, INTERNAL_BUFFER_SIZE>;

        class _InternalIfc
        {
        public:

            virtual void Call() = 0;
            virtual void SetException(std::exception_ptr e) = 0;
            virtual _InternalIfc* MoveTo(InternalBufferType& buf) = 0;
            virtual ~_InternalIfc() {}
        };

        _InternalIfc* _internal_instance_;

        InternalBufferType _buf_;

        _InternalCallableHolder(_InternalCallableHolder const& other) = delete;
        _InternalCallableHolder& operator=(_InternalCallableHolder const& other) = delete;

        bool _inBuffer(void* ptr)
        {
            return
                (ptr >= _buf_.data()) &&
                (ptr < (_buf_.data() + _buf_.size()));
        }

        void _clear()
        {
            if (_inBuffer(static_cast<void*>(_internal_instance_)))
                _internal_instance_->~_InternalIfc();
            else
                delete _internal_instance_;

            _internal_instance_ = nullptr;
        }

        template <typename T>
        static void* _getBufferPtr(InternalBufferType& buf)
        {
            const auto alignment = std::alignment_of_v<T>;
            uint8_t* ptr = (uint8_t*)((((uint64_t)buf.data() + alignment - 1) / alignment) * alignment);

            if ((ptr + sizeof(T)) <= (buf.data() + buf.size()))
            {
                return ptr;
            }

            return nullptr;
        }

    public:

        _InternalCallableHolder()
            : _internal_instance_(nullptr)
            , _buf_({})
        {

        }

        _InternalCallableHolder(_InternalCallableHolder&& other) noexcept
            : _internal_instance_(nullptr)
            , _buf_({})
        {
            if (other._inBuffer(other._internal_instance_))
            {
                _internal_instance_ = other._internal_instance_->MoveTo(_buf_);
                other._internal_instance_->~_InternalIfc();
                other._internal_instance_ = nullptr;
            }
            else
            {
                // If the other's internal instance is not allocated in its local buffer we can grab the pointer directly
                _internal_instance_ = other._internal_instance_;
                other._internal_instance_ = nullptr;
            }
        }

        _InternalCallableHolder& operator=(_InternalCallableHolder&& other) noexcept
        {
            _clear();

            if (other._inBuffer(other._internal_instance_))
            {
                _internal_instance_ = other._internal_instance_->MoveTo(_buf_);
                other._internal_instance_->~_InternalIfc();
                other._internal_instance_ = nullptr;
            }
            else
            {
                // If the other's internal instance is not allocated in its local buffer we can grab the pointer directly
                _internal_instance_ = other._internal_instance_;
                other._internal_instance_ = nullptr;
            }

            return *this;
        }

        ~_InternalCallableHolder()
        {
            _clear();
        }

        void Call()
        {
            _internal_instance_->Call();
        }

        void SetException(std::exception_ptr e)
        {
            _internal_instance_->SetException(e);
        }

        template <typename ArgumentT>
        class _ArgumentHolder
        {
        protected:

            ArgumentT _argument_value_;

        public:

            _ArgumentHolder()
                : _argument_value_(ArgumentT())
            { }

            void SetValue(ArgumentT val)
            {
                _argument_value_ = std::move(val);
            }
        };

        template <>
        class _ArgumentHolder<void>
        { };

        template <typename FnT, typename ArgumentT>
        class _FunctionHolder final : public _InternalIfc, public _ArgumentHolder<ArgumentT>
        {
        private:

            using result_type = _internal_invoke_result_t<FnT, ArgumentT>;

            FnT                  _fn_;
            Promise<result_type> _result_promise_;

            _FunctionHolder(_FunctionHolder const&) = delete;
            _FunctionHolder& operator=(_FunctionHolder const&) = delete;

        public:

            _FunctionHolder(FnT fn, Promise<result_type> resultPromise)
                : _fn_(std::move(fn))
                , _result_promise_(std::move(resultPromise))
            { }

            _FunctionHolder(_FunctionHolder&& other) noexcept
                : _fn_(std::move(other._fn_))
                , _result_promise_(std::move(other._result_promise_))
            {
                if constexpr (!std::is_same_v<ArgumentT, void>)
                    _ArgumentHolder<ArgumentT>::_argument_value_ = std::move(other._argument_value_);
            }

            _FunctionHolder& operator=(_FunctionHolder&& other)
            {
                if constexpr (!std::is_same_v<ArgumentT, void>)
                    _ArgumentHolder<ArgumentT>::_argument_value_ = std::move(other._argument_value_);

                _fn_ = std::move(other._fn_);
                _result_promise_ = std::move(other._result_promise_);

                return *this;
            }

            void Call() override
            {
                try
                {
                    if constexpr (std::is_same_v<result_type, void>)
                    {
                        if constexpr (std::is_same_v<ArgumentT, void>)
                        {
                            _fn_();
                        }
                        else
                        {
                            _fn_(std::move(_ArgumentHolder<ArgumentT>::_argument_value_));
                        }

                        _result_promise_.SetDone();
                    }
                    else
                    {
                        if constexpr (std::is_same_v<ArgumentT, void>)
                        {
                            _result_promise_.SetValue(_fn_());
                        }
                        else
                        {
                            _result_promise_.SetValue(_fn_(std::move(_ArgumentHolder<ArgumentT>::_argument_value_)));
                        }
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

            _InternalIfc* MoveTo(InternalBufferType& buf) override
            {
                void* ptr = _getBufferPtr<_FunctionHolder<FnT, ArgumentT>>(buf);
                if (ptr)
                    return new (ptr) _FunctionHolder<FnT, ArgumentT>(std::move(*this));
                else
                    return new _FunctionHolder<FnT, ArgumentT>(std::move(*this));
            }
        };

        template <typename FnT, typename ArgumentT>
        class _ChainedFunctionHolder final : public _InternalIfc, public _ArgumentHolder<ArgumentT>
        {
        private:

            using lower_future_type = _internal_invoke_result_t<FnT, ArgumentT>;
            using result_type = typename lower_future_type::value_type;

            FnT                  _fn_;
            Promise<result_type> _result_promise_;

            _ChainedFunctionHolder(_ChainedFunctionHolder const&) = delete;
            _ChainedFunctionHolder& operator=(_ChainedFunctionHolder const&) = delete;

        public:

            _ChainedFunctionHolder(FnT fn, Promise<result_type> resultPromise)
                : _fn_(std::move(fn))
                , _result_promise_(std::move(resultPromise))
            {
            }

            _ChainedFunctionHolder(_ChainedFunctionHolder&& other) noexcept
                : _fn_(std::move(other._fn_))
                , _result_promise_(std::move(other._result_promise_))
            {
                if constexpr (!std::is_same_v<ArgumentT, void>)
                    _ArgumentHolder<ArgumentT>::_argument_value_ = std::move(other._argument_value_);
            }

            _ChainedFunctionHolder& operator=(_ChainedFunctionHolder&& other)
            {
                if constexpr (!std::is_same_v<ArgumentT, void>)
                    _ArgumentHolder<ArgumentT>::_argument_value_ = std::move(other._argument_value_);

                _fn_ = std::move(other._fn_);
                _result_promise_ = std::move(other._result_promise_);

                return *this;
            }

            void Call() override
            {
                lower_future_type lowerFuture;
                try
                {
                    if constexpr (std::is_same_v<ArgumentT, void>)
                        lowerFuture = _fn_();
                    else
                        lowerFuture = _fn_(std::move(_ArgumentHolder<ArgumentT>::_argument_value_));
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

            _InternalIfc* MoveTo(InternalBufferType& buf) override
            {
                void* ptr = _getBufferPtr<_ChainedFunctionHolder<FnT, ArgumentT>>(buf);
                if (ptr)
                    return new (ptr) _ChainedFunctionHolder<FnT, ArgumentT>(std::move(*this));
                else
                    return new _ChainedFunctionHolder<FnT, ArgumentT>(std::move(*this));
            }
        };

        template <typename FnT, typename ValueT>
        _FunctionHolder<FnT, ValueT>* Init(FnT fn, Promise<_internal_invoke_result_t<FnT, ValueT>> resultPromise)
        {
            _clear();

            _FunctionHolder<FnT, ValueT>* ret = nullptr;

            void* ptr = _getBufferPtr<_FunctionHolder<FnT, ValueT>>(_buf_);

            if (ptr)
            {
                ret = new (ptr) _FunctionHolder<FnT, ValueT>(std::move(fn), std::move(resultPromise));
            }
            else
            {
                ret = new _FunctionHolder<FnT, ValueT>(std::move(fn), std::move(resultPromise));
            }

            _internal_instance_ = ret;
            return ret;
        }

        template <typename FnT, typename ValueT>
        _ChainedFunctionHolder<FnT, ValueT>* InitChained(FnT fn, Promise<typename _internal_invoke_result_t<FnT, ValueT>::value_type> resultPromise)
        {
            _clear();

            _ChainedFunctionHolder<FnT, ValueT>* ret = nullptr;

            void* ptr = _getBufferPtr<_ChainedFunctionHolder<FnT, ValueT>>(_buf_);

            if (ptr)
            {
                ret = new (ptr) _ChainedFunctionHolder<FnT, ValueT>(std::move(fn), std::move(resultPromise));
            }
            else
            {
                ret = new _ChainedFunctionHolder<FnT, ValueT>(std::move(fn), std::move(resultPromise));
            }

            _internal_instance_ = ret;
            return ret;
        }
    };

    class ThreadPool
    {

    private:

        std::mutex                          _queue_mtx_;
        std::condition_variable             _queue_cv_;
        std::deque<_InternalCallableHolder> _queue_;
        bool                                _stopped_;

        std::vector<std::thread>            _threads_;

        std::optional<_InternalCallableHolder> _popWork()
        {
            std::unique_lock lock(_queue_mtx_);
            while (_queue_.empty() && !_stopped_)
            {
                _queue_cv_.wait(lock);
            }

            if (_queue_.empty())
                return std::nullopt;

            auto ret = std::move(_queue_.front());
            _queue_.pop_front();

            return ret;
        }

        static void _threadProcess(ThreadPool* threadPool)
        {
            while (true)
            {
                auto work = threadPool->_popWork();
                if (!work)
                    return;

                work->Call();
            }
        }

    public:

        ThreadPool()
            : _stopped_(false)
        {
        }

        ~ThreadPool()
        {
            Stop();
        }

        void Start(int threadCount)
        {
            _threads_.resize(threadCount);

            for (auto& t : _threads_)
            {
                t = std::thread(_threadProcess, this);
            }
        }

        void Stop()
        {
            // Scope for lock
            {
                std::unique_lock lock(_queue_mtx_);
                _stopped_ = true;
            }

            _queue_cv_.notify_all();

            for (auto& t : _threads_)
            {
                t.join();
            }

            _threads_.clear();
        }

        void PushWork(_InternalCallableHolder holder)
        {
            std::unique_lock lock(_queue_mtx_);
            _queue_.push_back(std::move(holder));
            _queue_cv_.notify_one();
        }

        template <typename FnT>
        Future<std::invoke_result_t<FnT>> PushWork(FnT fn)
        {
            Promise<std::invoke_result_t<FnT>> prom;
            auto fut = prom.GetFuture();

            _InternalCallableHolder holder;
            holder.Init<FnT, void>(std::move(fn), std::move(prom));

            // Scope for lock
            {
                std::unique_lock lock(_queue_mtx_);
                _queue_.push_back(std::move(holder));
            }

            _queue_cv_.notify_one();

            return fut;
        }
    };

    template <typename T>
    struct _is_future { static constexpr bool value = false; };

    template <typename T>
    struct _is_future<Future<T>> { static constexpr bool value = true; };

    template <typename T>
    constexpr bool _is_future_v = _is_future<T>::value;

    template <typename T>
    constexpr bool _is_not_future_v = !_is_future<T>::value;

    template <typename ValueT>
    class PromiseFutureState;

    template <typename ValueT>
    class _InternalFutureBase
    {
    protected:

        ReferenceCounted<PromiseFutureState<ValueT>> _state_;

        template <typename T>
        friend class PromiseFutureState;

        friend class _InternalCallableHolder;

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

            _state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
        }

        _InternalFutureBase(_InternalFutureBase const&) = delete;
        _InternalFutureBase& operator=(_InternalFutureBase const&) = delete;

        _InternalFutureBase() noexcept
        { }

    public:

        using value_type = ValueT;

        ~_InternalFutureBase()
        {
        }

        bool Valid() const
        {
            return (bool)_state_;
        }

        ValueT Get()
        {
            if (!_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            std::conditional_t<std::is_same_v<ValueT, void>, VoidPlaceHolder, ValueT> val;

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

            _state_ = ReferenceCounted<PromiseFutureState<ValueT>>();

            if constexpr (!std::is_same_v<ValueT, void>)
                return val;
        }

        // If the continuation function itself returns another Future object,
        // we don't want to end up with something that looks like this on the top level: Future<Future<Future<Future<int>>>>.
        // This specialization causes the Future on the top level to still be a simple Future<int> that can be awaited.
        template<typename FnT>
        std::enable_if_t<
            _is_future_v<_internal_invoke_result_t<FnT, ValueT>>,
            _internal_invoke_result_t<FnT, ValueT>> Then(std::shared_ptr<ThreadPool> threadPool, FnT fn)
        {
            using resultType = typename _internal_invoke_result_t<FnT, ValueT>::value_type;

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

                    if (threadPool)
                    {
                        Promise<resultType> continuationPromise;
                        continuationFuture = continuationPromise.GetFuture();

                        _InternalCallableHolder continuationCallable;
                        auto continuationArgumentHolder = continuationCallable.InitChained<FnT, ValueT>(
                            std::move(fn), std::move(continuationPromise));

                        if constexpr (!std::is_same_v<ValueT, void>)
                        {
                            continuationArgumentHolder->SetValue(std::move(*_state_->_value_));
                        }

                        threadPool->PushWork(std::move(continuationCallable));
                    }
                    else
                    {
                        try
                        {
                            if constexpr (std::is_same_v<ValueT, void>)
                            {
                                continuationFuture = fn();
                            }
                            else
                            {
                                continuationFuture = fn(std::move(*_state_->_value_));
                            }
                        }
                        catch (...)
                        {
                            Promise<resultType> continuationPromise;
                            continuationFuture = continuationPromise.GetFuture();
                            continuationPromise.SetException(std::current_exception());
                        }
                    }
                }
                else
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();

                    _state_->_continuation_.emplace();
                    _state_->_continuation_argument_holder_ = _state_->_continuation_->InitChained<FnT, ValueT>(
                        std::move(fn), std::move(continuationPromise));
                    _state_->_continuation_thread_pool_ = threadPool;
                }
            }

            _state_ = ReferenceCounted<PromiseFutureState<ValueT>>();

            return continuationFuture;
        }

        template<typename FnT>
        std::enable_if_t<
            _is_not_future_v<_internal_invoke_result_t<FnT, ValueT>>,
            Future<_internal_invoke_result_t<FnT, ValueT>>> Then(std::shared_ptr<ThreadPool> threadPool, FnT fn)
        {
            using resultType = _internal_invoke_result_t<FnT, ValueT>;

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

                    if (threadPool)
                    {
                        _InternalCallableHolder continuationCallable;
                        auto continuationArgumentHolder = continuationCallable.Init<FnT, ValueT>(
                            std::move(fn), std::move(continuationPromise));

                        if constexpr (!std::is_same_v<ValueT, void>)
                        {
                            continuationArgumentHolder->SetValue(std::move(*_state_->_value_));
                        }

                        threadPool->PushWork(std::move(continuationCallable));
                    }
                    else
                    {
                        try
                        {
                            if constexpr (std::is_same_v<resultType, void>)
                            {
                                if constexpr (std::is_same_v<ValueT, void>)
                                {
                                    fn();
                                }
                                else
                                {
                                    fn(std::move(*_state_->_value_));
                                }

                                continuationPromise.SetDone();
                            }
                            else
                            {
                                resultType result;

                                if constexpr (std::is_same_v<ValueT, void>)
                                {
                                    result = fn();
                                }
                                else
                                {
                                    result = fn(std::move(*_state_->_value_));
                                }

                                continuationPromise.SetValue(std::move(result));
                            }
                        }
                        catch (...)
                        {
                            continuationPromise.SetException(std::current_exception());
                        }
                    }
                }
                else
                {
                    _state_->_continuation_.emplace();
                    _state_->_continuation_argument_holder_ = _state_->_continuation_->Init<FnT, ValueT>(
                        std::move(fn), std::move(continuationPromise));
                    _state_->_continuation_thread_pool_ = threadPool;
                }
            }

            _state_ = ReferenceCounted<PromiseFutureState<ValueT>>();

            return continuationFuture;
        }

        template<typename FnT>
        auto Then(FnT fn)
        {
            return Then(nullptr, std::move(fn));
        }
    };

    template <typename ValueT>
    class Future : public _InternalFutureBase<ValueT>
    {
    private:

        Future(ReferenceCounted<PromiseFutureState<ValueT>> state)
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
            other._state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
        }

        Future& operator=(Future&& other) noexcept
        {
            _InternalFutureBase<ValueT>::_state_ = other._state_;
            other._state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
            return *this;
        }

        Future(ValueT value)
        {
            _InternalFutureBase<ValueT>::_state_ = MakeReferenceCounted<PromiseFutureState<ValueT>>();
            _InternalFutureBase<ValueT>::_state_->_value_ = std::move(value);
        }
    };

    template <>
    class Future<void> : public _InternalFutureBase<void>
    {
    private:

        Future(ReferenceCounted<PromiseFutureState<void>> state)
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
            other._state_ = ReferenceCounted<PromiseFutureState<void>>();
        }

        Future& operator=(Future&& other) noexcept;

        template<typename FnT>
        void OnException(FnT fn);
    };

    template <typename ValueT>
    class _InternalPromiseBase
    {
    protected:

        ReferenceCounted<PromiseFutureState<ValueT>> _state_;
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

                _state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
            }
        }

        _InternalPromiseBase()
            : _state_(MakeReferenceCounted<PromiseFutureState<ValueT>>())
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

            other._state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
            other._future_retrieved_ = false;
            other._value_set_ = false;
        }

        Promise& operator=(Promise&& other) noexcept
        {
            _InternalPromiseBase<ValueT>::_clear();

            _InternalPromiseBase<ValueT>::_state_ = other._state_;
            _InternalPromiseBase<ValueT>::_future_retrieved_ = other._future_retrieved_;
            _InternalPromiseBase<ValueT>::_value_set_ = other._value_set_;

            other._state_ = ReferenceCounted<PromiseFutureState<ValueT>>();
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
                _InternalPromiseBase<ValueT>::_state_->_continuation_argument_holder_->SetValue(std::move(value));

                if (_InternalPromiseBase<ValueT>::_state_->_continuation_thread_pool_)
                {
                    _InternalPromiseBase<ValueT>::_state_->_continuation_thread_pool_->PushWork(
                        std::move(*_InternalPromiseBase<ValueT>::_state_->_continuation_));
                }
                else
                {
                    _InternalPromiseBase<ValueT>::_state_->_continuation_->Call();
                }
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

            other._state_ = ReferenceCounted<PromiseFutureState<void>>();
            other._future_retrieved_ = false;
            other._value_set_ = false;
        }

        Promise& operator=(Promise&& other) noexcept
        {
            _clear();

            _state_ = other._state_;
            _future_retrieved_ = other._future_retrieved_;
            _value_set_ = other._value_set_;

            other._state_ = ReferenceCounted<PromiseFutureState<void>>();
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

        std::mutex                                                                               _mtx_value_;
        std::condition_variable                                                                  _cv_value_;
        std::optional<std::conditional_t<std::is_same_v<ValueT, void>, VoidPlaceHolder, ValueT>> _value_;
        std::exception_ptr                                                                       _exception_;
        std::optional<_InternalCallableHolder>                                                   _continuation_;
        _InternalCallableHolder::_ArgumentHolder<ValueT>*                                        _continuation_argument_holder_;
        std::shared_ptr<ThreadPool>                                                              _continuation_thread_pool_;
        std::optional<Promise<ValueT>>                                                           _chained_promise_;
        std::optional<std::function<void(std::exception_ptr)>>                                   _on_exception_;

        friend class _InternalFutureBase<ValueT>;
        friend class _InternalPromiseBase<ValueT>;
        friend class Future<ValueT>;
        friend class Promise<ValueT>;
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

        _state_ = ReferenceCounted<PromiseFutureState<void>>();
    }

    template <typename ValueT>
    Future<std::vector<ValueT>> WhenAll(std::span<Future<ValueT>> futures)
    {
        struct WhenAllContext
        {
            std::vector<ValueT> values;
            std::atomic_size_t countdown;
            Promise<std::vector<ValueT>> promise_all;
            std::vector<std::exception_ptr> exceptions;
            std::atomic_size_t exception_count;
        };

        auto whenAllContext = MakeReferenceCounted<WhenAllContext>();
        whenAllContext->values.resize(futures.size());
        whenAllContext->exceptions.resize(futures.size());
        whenAllContext->countdown = futures.size();
        whenAllContext->exception_count = 0;

        for (size_t i = 0; i < futures.size(); ++i)
        {
            futures[i].Then([whenAllContext = whenAllContext, idx = i] (ValueT val) mutable
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
                }).OnException([whenAllContext = whenAllContext, idx = i](std::exception_ptr e) mutable
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
            std::atomic_size_t countdown;
            Promise<std::tuple<ValuesT...>> promise_all;
            std::array<std::exception_ptr, sizeof...(ValuesT)> exceptions;
            std::atomic_size_t exception_count;
        };

        auto whenAllContext = MakeReferenceCounted<WhenAllContext>();
        whenAllContext->countdown = sizeof...(ValuesT);
        whenAllContext->tuple_futures = std::tuple<Future<ValuesT>...>{ std::move(futures)... };
        whenAllContext->exception_count = 0;

        foreach_number<0, sizeof...(ValuesT)>([whenAllContext = whenAllContext](auto idx) mutable
            {
                auto& current_future = std::get<idx>(whenAllContext->tuple_futures);
                auto& current_value = std::get<idx>(whenAllContext->values);

                current_future.Then([whenAllContext = whenAllContext, v = &current_value](auto val) mutable
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
                    }).OnException([whenAllContext = whenAllContext, idx = idx](std::exception_ptr e) mutable
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
            std::mutex                     _mtx_value_;
            std::condition_variable        _cv_value_;
            ReferenceCounted<ValueT const> _value_;
            std::exception_ptr             _exception_;

            std::vector<
                std::tuple<
                    _InternalCallableHolder,
                    _InternalCallableHolder::_ArgumentHolder<ReferenceCounted<ValueT const>>*,
                    std::shared_ptr<ThreadPool>>> _continuations_;
        };

        ReferenceCounted<_persistentState> _persistent_state_;

        template <typename FnT>
        void _addContinuation(
            std::shared_ptr<ThreadPool> threadPool,
            FnT fn,
            Promise<std::invoke_result_t<FnT, ReferenceCounted<ValueT const>>> prom)
        {
            auto& [continuationCallableHolder, continuationArgumentHolder, continuationThreadPool] =
                _persistent_state_->_continuations_.emplace_back();
            
            continuationArgumentHolder = continuationCallableHolder.template Init<FnT, ReferenceCounted<ValueT const>>(
                std::move(fn), std::move(prom));

            continuationThreadPool = threadPool;
        }

        template <typename FnT>
        void _addChainedContinuation(
            std::shared_ptr<ThreadPool> threadPool,
            FnT fn,
            Promise<typename std::invoke_result_t<FnT, ReferenceCounted<ValueT const>>::value_type> prom)
        {
            auto& [continuationCallableHolder, continuationArgumentHolder, continuationThreadPool] =
                _persistent_state_->_continuations_.emplace_back();

            continuationArgumentHolder = continuationCallableHolder.template InitChained<FnT, ReferenceCounted<ValueT const>>(
                std::move(fn), std::move(prom));

            continuationThreadPool = threadPool;
        }

    public:

        PersistentFuture()
            : _persistent_state_(nullptr)
        { }

        PersistentFuture(Future<ValueT> fut)
            : _persistent_state_()
        {
            // Set a "proxy" continuation function on the base future that will set
            // the value in the persistent state and call all continuation functions.
            fut.Then([persistent_state = _persistent_state_](ValueT value) mutable
                {
                    std::unique_lock lock(persistent_state->_mtx_value_);
                    persistent_state->_value_ = ReferenceCounted<ValueT const>(std::move(value));

                    for (auto& [fn, argHolder, threadPool] : persistent_state->_continuations_)
                    {
                        argHolder->SetValue(persistent_state->_value_);
                        if (threadPool)
                            threadPool->PushWork(std::move(fn));
                        else
                            fn.Call();
                    }

                    persistent_state->_continuations_.clear();

                    persistent_state->_cv_value_.notify_all();
                }).OnException([persistent_state = _persistent_state_](std::exception_ptr e) mutable
                    {
                        std::unique_lock lock(persistent_state->_mtx_value_);
                        persistent_state->_exception_ = e;
                        
                        for (auto& [fn, argHolder, threadPool] : persistent_state->_continuations_)
                            fn.SetException(e);

                        persistent_state->_continuations_.clear();

                        persistent_state->_cv_value_.notify_all();
                    });;
        }

        ValueT const& Get()
        {
            std::unique_lock lck(_persistent_state_->_mtx_value_);

            while (!_persistent_state_->_value_ && !_persistent_state_->_exception_)
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
            _is_future_v<_internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>>,
            _internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>> Then(std::shared_ptr<ThreadPool> threadPool, FnT fn)
        {
            using resultType = typename _internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>::value_type;

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
                else if (_persistent_state_->_value_)
                {
                    // If the promise has already been fulfilled,
                    // call the continuation function immediately

                    if (threadPool)
                    {
                        Promise<resultType> continuationPromise;
                        continuationFuture = continuationPromise.GetFuture();

                        _InternalCallableHolder continuationCallable;
                        auto continuationArgumentHolder = continuationCallable.InitChained<FnT, ReferenceCounted<ValueT const>>(
                            std::move(fn), std::move(continuationPromise));

                        if constexpr (!std::is_same_v<resultType, void>)
                        {
                            continuationArgumentHolder->SetValue(_persistent_state_->_value_);
                        }

                        threadPool->PushWork(std::move(continuationCallable));
                    }
                    else
                    {
                        try
                        {
                            continuationFuture = fn(_persistent_state_->_value_);
                        }
                        catch (...)
                        {
                            Promise<resultType> continuationPromise;
                            continuationFuture = continuationPromise.GetFuture();
                            continuationPromise.SetException(std::current_exception());
                        }
                    }
                }
                else
                {
                    Promise<resultType> continuationPromise;
                    continuationFuture = continuationPromise.GetFuture();
                    _addChainedContinuation(threadPool, std::move(fn), std::move(continuationPromise));
                }
            }

            return continuationFuture;
        }

        template<typename FnT>
        std::enable_if_t<
            _is_not_future_v<_internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>>,
            Future<_internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>>> Then(std::shared_ptr<ThreadPool> threadPool, FnT fn)
        {
            using resultType = _internal_invoke_result_t<FnT, ReferenceCounted<ValueT const>>;

            if (!_persistent_state_)
            {
                throw FutureError(FutureErrorCode::NoState, "Future has no state!");
            }

            std::unique_lock lck(_persistent_state_->_mtx_value_);

            Promise<resultType> continuationPromise;
            auto continuationFuture = continuationPromise.GetFuture();

            if (_persistent_state_->_exception_)
            {
                continuationPromise.SetException(_persistent_state_->_exception_);
            }
            else if (_persistent_state_->_value_)
            {
                // If the promise has already been fulfilled,
                // call the continuation function immediately

                if (threadPool)
                {
                    _InternalCallableHolder continuation;
                    auto continuationArgumentHolder = continuation.Init<FnT, ReferenceCounted<ValueT const>>(
                        std::move(fn), std::move(continuationPromise));

                    if constexpr (!std::is_same_v<resultType, void>)
                    {
                        continuationArgumentHolder->SetValue(_persistent_state_->_value_);
                    }

                    threadPool->PushWork(std::move(continuation));
                }
                else
                {
                    try
                    {
                        if constexpr (std::is_same_v<resultType, void>)
                        {
                            fn(_persistent_state_->_value_);
                            continuationPromise.SetDone();
                        }
                        else
                        {
                            auto result = fn(_persistent_state_->_value_);
                            continuationPromise.SetValue(std::move(result));
                        }
                    }
                    catch (...)
                    {
                        continuationPromise.SetException(std::current_exception());
                    }
                }
            }
            else
            {
                _addContinuation(threadPool, std::move(fn), std::move(continuationPromise));
            }

            return continuationFuture;
        }

        template<typename FnT>
        auto Then(FnT fn)
        {
            return Then(nullptr, std::move(fn));
        }
    };

    enum class ChannelErrorCode : int32_t
    {
        None = 0,

        ReaderAlreadyRetrieved = 1,
        NoState = 2
    };

    class ChannelError : public std::runtime_error
    {
    private:

        ChannelErrorCode _error_code_;

    public:

        ChannelError(ChannelErrorCode errorCode, const char* msg)
            : std::runtime_error(msg)
            , _error_code_(errorCode)
        {
        }

        ChannelErrorCode ErrorCode() const
        {
            return _error_code_;
        }
    };

    template <typename ValueT>
    class AsyncChannelReader;

    template <typename ValueT>
    class AsyncChannelWriter;

    template <typename ValueT>
    class AsyncChannelState
    {
    private:

        std::mutex                                    _mtx_;
        std::deque<ValueT>                            _queue_;
        std::optional<Promise<std::optional<ValueT>>> _prom_;
        bool                                          _done_ = false;

        friend class AsyncChannelReader<ValueT>;
        friend class AsyncChannelWriter<ValueT>;
    };

    template <typename ValueT>
    class AsyncChannelReader
    {
    private:

        ReferenceCounted<AsyncChannelState<ValueT>> _state_;

        AsyncChannelReader(ReferenceCounted<AsyncChannelState<ValueT>> state)
            : _state_(state)
        {
        }

        friend class AsyncChannelWriter<ValueT>;

    public:

        AsyncChannelReader()
        { }

        ~AsyncChannelReader()
        {
            _state_ = ReferenceCounted<AsyncChannelState<ValueT>>();
        }

        AsyncChannelReader(AsyncChannelReader const&) = delete;
        AsyncChannelReader& operator=(AsyncChannelReader const&) = delete;

        AsyncChannelReader(AsyncChannelReader&& other) noexcept
        {
            _state_ = other._state_;
            other._state_ = ReferenceCounted<AsyncChannelState<ValueT>>();
        }

        AsyncChannelReader& operator=(AsyncChannelReader&& other) noexcept
        {
            _state_ = other._state_;
            other._state_ = ReferenceCounted<AsyncChannelState<ValueT>>();
            return *this;
        }

        Future<std::optional<ValueT>> Read()
        {
            if (!_state_)
            {
                throw ChannelError(ChannelErrorCode::NoState, "Channel has no state!");
            }

            std::unique_lock lock(_state_->_mtx_);

            // If there are values ready in the queue, directly return the first value
            if (!_state_->_queue_.empty())
            {
                auto val = std::move(_state_->_queue_.front());
                _state_->_queue_.pop_front();

                return Future<std::optional<ValueT>>(std::move(val));
            }

            // If there are no more values
            if (_state_->_done_)
            {
                return Future<std::optional<ValueT>>(std::nullopt);
            }

            // Otherwise set a promise in the state to be fulfilled later
            return _state_->_prom_.emplace().GetFuture();
        }
    };

    template <typename ValueT>
    class AsyncChannelWriter
    {
    private:

        ReferenceCounted<AsyncChannelState<ValueT>> _state_;
        bool _reader_retrieved_;

        AsyncChannelWriter(AsyncChannelWriter const&) = delete;
        AsyncChannelWriter& operator=(AsyncChannelWriter const&) = delete;

        void _clear()
        {
            if (_state_)
            {
                // Scope for lock
                {
                    std::unique_lock lock(_state_->_mtx_);
                    if (_state_->_prom_) // If there is a waiting promise it needs to be set to signal the end of the stream
                    {
                        _state_->_prom_->SetValue(std::nullopt);
                        _state_->_prom_.reset();
                    }

                    _state_->_done_ = true;
                }

                _state_ = ReferenceCounted<AsyncChannelState<ValueT>>();
            }
        }

    public:

        AsyncChannelWriter()
            : _state_(AsyncChannelState<ValueT>())
            , _reader_retrieved_(false)
        {
        }

        AsyncChannelWriter(AsyncChannelWriter&& other)
            : _state_(other._state_)
            , _reader_retrieved_(other._reader_retrieved_)
        {
            other._state_ = ReferenceCounted<AsyncChannelState<ValueT>>();
        }

        AsyncChannelWriter& operator=(AsyncChannelWriter&& other)
        {
            _clear();

            _state_ = other._state_;
            other._state_ = ReferenceCounted<AsyncChannelState<ValueT>>();

            return *this;
        }

        ~AsyncChannelWriter()
        {
            _clear();
        }

        void Write(ValueT val)
        {
            if (!_state_)
            {
                throw ChannelError(ChannelErrorCode::NoState, "Channel has no state!");
            }

            std::unique_lock lock(_state_->_mtx_);

            // If there is a waiting promise, set its value
            if (_state_->_prom_)
            {
                _state_->_prom_->SetValue(std::move(val));
                _state_->_prom_.reset();
            }
            else // Otherwise push the value to the queue
            {
                _state_->_queue_.push_back(std::move(val));
            }
        }

        AsyncChannelReader<ValueT> GetReader()
        {
            if (_reader_retrieved_)
            {
                throw ChannelError(ChannelErrorCode::ReaderAlreadyRetrieved, "Channel reader already retrieved!");
            }

            if (!_state_)
            {
                throw ChannelError(ChannelErrorCode::NoState, "Channel has no state!");
            }

            _reader_retrieved_ = true;

            return AsyncChannelReader<ValueT>(_state_);
        }
    };
}
