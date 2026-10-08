#pragma once

// Starting a Qt logging worker can itself emit a Qt warning. Never submit
// that nested warning to the pool whose lock the caller already holds.
// Warnings raised while writing a log must also avoid the writer's mutex.
namespace LogDispatch {

enum class Operation { Idle, Submitting, Writing };

inline thread_local Operation currentOperation = Operation::Idle;

class Scope {
public:
    explicit Scope(Operation operation) : m_Previous(currentOperation)
    {
        currentOperation = operation;
    }
    ~Scope() { currentOperation = m_Previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const Operation m_Previous;
};

template<typename Write>
void write(Write&& operation)
{
    const Scope scope(Operation::Writing);
    operation();
}

template<typename Submit, typename Write, typename EmergencyWrite>
void dispatch(bool asynchronous, Submit&& submit, Write&& write, EmergencyWrite&& emergencyWrite)
{
    if (currentOperation == Operation::Writing) {
        // The normal writer is already on this thread's stack. Use a sink
        // which does not call Qt or take the normal logger's locks.
        emergencyWrite();
    }
    else if (asynchronous && currentOperation == Operation::Idle) {
        const Scope scope(Operation::Submitting);
        submit();
    }
    else {
        // Nested worker-start warnings can be written immediately: this
        // thread holds the pool lock, but not the normal writer's mutex.
        LogDispatch::write(write);
    }
}

}
