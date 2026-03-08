#include "task_stuff.h"

namespace TaskStuff
{
    void Promise<void>::_clear()
    {
        if (_state_)
        {
            if (!_is_done_)
            {
                SetException(FutureError(FutureErrorCode::BrokenPromise, "Promise was broken!"));
            }

            _state_->_release();
            _state_ = nullptr;
        }
    }

    Promise<void>::Promise()
        : _state_(new PromiseFutureState<void>())
        , _future_retrieved_(false)
        , _is_done_(false)
    {
    }

    Future<void> Promise<void>::GetFuture()
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

        return Future<void>(_state_);
    }

    void Promise<void>::SetDone()
    {
        if (_is_done_)
        {
            throw FutureError(FutureErrorCode::PromiseAlreadySatisfied, "Promise value already set!");
        }

        if (!_state_)
        {
            throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
        }

        std::unique_lock lck(_state_->_mtx_value_);
        _is_done_ = true;

        // If a continuation function is set, call it with the value
        if (_state_->_continuation_)
        {
            _state_->_continuation_->Call();
        }
        else if (_state_->_chained_promise_)
        {
            _state_->_chained_promise_->SetDone();
        }
        else // Otherwise set the value in the state normally
        {
            _state_->_value_.emplace();
            _state_->_cv_value_.notify_all();
        }
    }

    void Promise<void>::SetException(std::exception_ptr exceptionPtr)
    {
        if (_is_done_)
        {
            throw FutureError(FutureErrorCode::PromiseAlreadySatisfied, "Promise value already set!");
        }

        if (!_state_)
        {
            throw FutureError(FutureErrorCode::NoState, "Promise has no state!");
        }

        std::unique_lock lck(_state_->_mtx_value_);
        _is_done_ = true;

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


    void Future<void>::_setChainedPromise(Promise<void> chainedPromise)
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
                chainedPromise.SetDone();
            }
            else
            {
                _state_->_chained_promise_ = std::move(chainedPromise);
            }
        }

        _state_->_release();
        _state_ = nullptr;
    }
}
