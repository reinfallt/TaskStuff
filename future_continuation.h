#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace Futures
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

    struct ValueT {};

    class PromiseFutureState
    {
    private:

        std::atomic_int _ref_count_ = 1;

        std::mutex                      _mtx_value_;
        std::condition_variable         _cv_value_;
        std::optional<ValueT>           _value_;
        std::unique_ptr<std::exception> _exception_;

        void _addRef() { ++_ref_count_; }

        void _release()
        {
            if (0 == --_ref_count_)
            {
                delete this;
            }
        }

        friend class Future;
        friend class Promise;
    };

    class Future
    {
    private:

        PromiseFutureState* _state_;

        Future(PromiseFutureState* state)
            : _state_(state)
        { }

        friend class Promise;

    public:

        Future(Future const&) = delete;
        Future& operator=(Future const&) = delete;

        Future(Future&& other) :
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
            _state_(new PromiseFutureState())
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

            std::unique_lock lck(_state_->_mtx_value_);

            while (!_state_->_value_.has_value() && !_state_->_exception_)
            {
                _state_->_cv_value_.wait(lck);
            }

            if (_state_->_exception_)
            {
                throw *_state_->_exception_;
            }

            ValueT val = std::move(*_state_->_value_);

            _state_->_release();
            _state_ = nullptr;

            return val;
        }
    };

    class Promise
    {
    private:

        PromiseFutureState* _state_;
        bool _future_retrieved_;
        bool _value_set_;

        Promise(Promise const&) = delete;
        Promise& operator=(Promise const&) = delete;

    public:

        Promise()
            : _state_(new PromiseFutureState())
            , _future_retrieved_(false)
            , _value_set_(false)
        { }

        ~Promise()
        {
            if (!_value_set_ && _state_)
            {
                std::unique_lock lck(_state_->_mtx_value_);
                _state_->_exception_ = std::make_unique<FutureError>(FutureErrorCode::BrokenPromise, "Promise was broken!");
                _state_->_cv_value_.notify_all();
            }
        }

        Future GetFuture()
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

            return Future(_state_);
        }

        void SetValue()
        {
            if (_value_set_)
            {

            }
        }
    };
}