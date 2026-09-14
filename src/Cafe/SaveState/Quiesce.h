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

	// --- GPU (Latte) thread participation ---
	//
	// Parking the PPC cores stops the guest, but the Latte thread is a separate host
	// thread that keeps running, and it owns a large amount of state *derived* from guest
	// memory: the texture cache and buffer cache are keyed by guest address, the render
	// target database points into them, and the command processor holds a parse position.
	// Replacing all of guest memory underneath that leaves it describing a world that no
	// longer exists.
	//
	// So the GPU thread is a participant in the world-stop rather than a bystander: it
	// parks at a command-packet boundary, and it is the thread that performs the post-load
	// cache drop. Renderer resources belong to the thread that created them, so dropping
	// them from the GUI thread would race the backend's deferred-destruction queues.

	// Cheap check for the command processor's idle path. False in the overwhelmingly
	// common case, so the hook costs one relaxed load per idle iteration.
	bool IsGpuStateWorkPending();

	// Called by the Latte thread from its command-processor idle path. Parks if a quiesce
	// is in progress, then performs any pending cache drop before returning. Safe to call
	// when there is nothing to do.
	void GpuHandleStateWork();

	// Ask the GPU thread to drop its guest-address-keyed caches. Call this while a
	// QuiesceScope is held, after guest memory has been restored. The drop is carried out
	// by the parked GPU thread and completes before the scope releases the PPC cores, so
	// no guest drawcall can be submitted against a stale cache.
	void RequestGpuCacheDropOnRelease();

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

		// Both are best effort. A GPU thread that cannot reach its safe point must not
		// make saving impossible, so failure downgrades to the previous behaviour (drained
		// ring, running GPU thread) and is logged rather than aborting the operation.
		bool AcquireGpuThread(uint32 timeoutMs);
		void ReleaseGpuThread();

		QuiesceStatus m_status = QuiesceStatus::SchedulerNotRunning;
		bool m_held = false;
		bool m_gpuParked = false;
	};
} // namespace SaveStates
