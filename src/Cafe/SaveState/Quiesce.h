#pragma once

namespace SaveStates
{
	// World-stop protocol.
	//
	// A state may only be taken when no guest thread is mid-timeslice. Cemu's
	// cooperative fiber scheduler provides a natural rendezvous: every guest thread
	// funnels through __OSThreadSwitchToNext() at least once per quantum
	// (ppcThreadQuantum instructions), and that function calls __OSStoreThread() to
	// write the thread's full register context into its guest OSThread_t before
	// switching away. Parking each core *after* that store means every guest CPU
	// context is already inside the RAM snapshot for free -- which is the property the
	// whole save state design rests on.

	// Hot-path check. Relaxed load of an atomic bool: a core may miss a request by one
	// scheduler iteration, which only delays the rendezvous, never breaks it.
	bool IsQuiesceRequested();

	// Park the calling scheduler core at the rendezvous barrier until the state
	// operation completes.
	//
	// Preconditions: the caller is a scheduler host thread, the scheduler lock is NOT
	// held, and no PPC instance is current (i.e. the guest context has already been
	// stored to guest memory).
	void QuiesceParkCurrentCore();

	enum class QuiesceStatus
	{
		Success,
		Timeout,                   // a core failed to reach the barrier in time
		SchedulerNotRunning,       // no title running
		CalledFromSchedulerThread, // would self-deadlock
		Reentrant,                 // another quiesce is already in progress
	};

	const char* GetQuiesceStatusMessage(QuiesceStatus status);

	// RAII world-stop. Must be constructed from a non-scheduler thread. For the
	// lifetime of the object every scheduler core is parked at the barrier and guest
	// memory is not being mutated by guest code.
	//
	// Always check IsHeld() before touching guest state -- construction can legitimately
	// fail (timeout on a title spinning with interrupts disabled, for example), and in
	// that case the world is still running.
	class QuiesceScope
	{
	  public:
		static constexpr uint32 kDefaultTimeoutMs = 2000;

		explicit QuiesceScope(uint32 timeoutMs = kDefaultTimeoutMs);
		~QuiesceScope();

		QuiesceScope(const QuiesceScope&) = delete;
		QuiesceScope& operator=(const QuiesceScope&) = delete;

		QuiesceStatus GetStatus() const { return m_status; }
		bool IsHeld() const { return m_held; }

	  private:
		void ReleaseAndWaitForCores();

		QuiesceStatus m_status = QuiesceStatus::SchedulerNotRunning;
		bool m_held = false;
	};
} // namespace SaveStates
