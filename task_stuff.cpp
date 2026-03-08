#include "task_stuff.h"

namespace TaskStuff
{
    void Promise<void>::SetDone()
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

    Future<void>& Future<void>::operator=(Future<void>&& other) noexcept
    {
        if (_state_)
            _state_->_release();

        _state_ = other._state_;
        other._state_ = nullptr;
        return *this;
    }
}
