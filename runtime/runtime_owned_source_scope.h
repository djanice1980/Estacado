#pragma once
#include "runtime_owned_camera_source.h"

namespace runtime_owned_camera_source {
// Lifetime belongs to an actual native submission, not its optional binder.
// Unknown nested work hides the outer token. Revalidate at each transfer.
class SubmissionScope {
public:
    SubmissionScope(const void* context, Token token) noexcept
        : previous_(active_), context_(context), token_(token) { active_ = this; }
    ~SubmissionScope() { active_ = previous_; }
    SubmissionScope(const SubmissionScope&) = delete;
    SubmissionScope& operator=(const SubmissionScope&) = delete;
    static Token Current(const void* context) noexcept {
        return active_ && context && active_->context_ == context ? active_->token_ : Token{};
    }
private:
    inline static thread_local const SubmissionScope* active_{};
    const SubmissionScope* previous_{};
    const void* context_{};
    Token token_{};
};
} // namespace runtime_owned_camera_source
