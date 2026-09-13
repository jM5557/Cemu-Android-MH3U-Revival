#include "Cafe/SaveState/Quiesce.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cemu/Logging/CemuLogging.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace SaveStates
{
	static std::atomic<bool> s_quiesceRequested{false};
	static std::atomic<bool> s_scopeActive{false};
	static std::mutex s_barrierMutex;
	static std::condition_variable s_barrierCv;
	static uint32 s_parkedCoreCount = 0;

	bool IsQuiesceRequested()
	{
		return s_quiesceRequested.load(std::memory_order_relaxed);
	}

	void QuiesceParkCurrentCore()
	{
		std::unique_lock lock(s_barrierMutex);
		// The request can be withdrawn between the caller's fast-path check and here
		// (e.g. a quiesce that timed out on another core).
		if (!s_quiesceRequested.load(std::memory_order_relaxed))
			return;
		s_parkedCoreCount++;
		s_barrierCv.notify_all();
		while (s_quiesceRequested.load(std::memory_order_relaxed))
			s_barrierCv.wait(lock);
		cemu_assert_debug(s_parkedCoreCount > 0);
		s_parkedCoreCount--;
		s_barrierCv.notify_all();
	}

	const char* GetQuiesceStatusMessage(QuiesceStatus status)
	{
		switch (status)
		{
		case QuiesceStatus::Success:
			return "Success";
		case QuiesceStatus::Timeout:
			return "Timed out waiting for all CPU cores to reach a safe point";
		case QuiesceStatus::SchedulerNotRunning:
			return "No title is running";
		case QuiesceStatus::CalledFromSchedulerThread:
			return "Internal error: state operation requested from a scheduler thread";
		case QuiesceStatus::Reentrant:
			return "Another save state operation is already in progress";
		}
		return "Unknown error";
	}

	// Drain the GPU command ring.
	//
	// Parking the PPC cores stops new submissions, but the Latte thread keeps running and
	// keeps consuming. Waiting for the ring to empty means a state is only ever captured
	// at a point where the GPU has caught up with the CPU, so the host-side read/write
	// cursors agree and can be restored as "empty" rather than mid-stream.
	//
	// This is best effort: a title whose GPU is blocked on something the parked cores were
	// meant to provide will not drain. We log and continue rather than making saving
	// impossible, since the failure mode is a torn GPU state rather than a broken snapshot
	// of guest memory.
	static void DrainGpuRingBuffer(uint32 timeoutMs)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!TCL::TCLGPUIsRingBufferEmpty())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				cemuLog_log(LogType::Force, "Save state: GPU command ring did not drain within {}ms", timeoutMs);
				return;
			}
			std::this_thread::yield();
		}
	}

	QuiesceScope::QuiesceScope(uint32 timeoutMs)
	{
		if (!coreinit::OSIsSchedulerActive())
		{
			m_status = QuiesceStatus::SchedulerNotRunning;
			return;
		}
		// Parking is done BY the scheduler cores, so a scheduler thread waiting for all
		// cores to park would be waiting for itself.
		if (coreinit::__OSIsCurrentThreadScheduler())
		{
			m_status = QuiesceStatus::CalledFromSchedulerThread;
			return;
		}
		if (s_scopeActive.exchange(true))
		{
			m_status = QuiesceStatus::Reentrant;
			return;
		}

		const uint32 coreCount = (uint32)coreinit::OSGetSchedulerThreadCount();
		if (coreCount == 0)
		{
			s_scopeActive.store(false);
			m_status = QuiesceStatus::SchedulerNotRunning;
			return;
		}

		{
			std::unique_lock lock(s_barrierMutex);
			cemu_assert_debug(s_parkedCoreCount == 0);
			s_quiesceRequested.store(true, std::memory_order_relaxed);
		}

		// A core sitting in g_coreRunQueueThreadCount[].waitUntilNonZero() has no
		// runnable thread and would never reach the barrier on its own. This is the
		// same wake-up trick OSSchedulerEnd() uses to shut the scheduler down.
		coreinit::__OSQuiesceWakeCores();

		std::unique_lock lock(s_barrierMutex);
		const bool allParked = s_barrierCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
													[coreCount] { return s_parkedCoreCount >= coreCount; });
		lock.unlock();

		if (!allParked)
		{
			// Abort rather than deadlock. A title spinning with interrupts disabled, or
			// a stuck IPC request, can keep a core out of the scheduler indefinitely.
			ReleaseAndWaitForCores();
			s_scopeActive.store(false);
			m_status = QuiesceStatus::Timeout;
			return;
		}

		// Cores are parked, so nothing new enters the ring from here on.
		DrainGpuRingBuffer(timeoutMs);

		m_status = QuiesceStatus::Success;
		m_held = true;
	}

	QuiesceScope::~QuiesceScope()
	{
		if (!m_held)
			return;
		ReleaseAndWaitForCores();
		s_scopeActive.store(false);
	}

	void QuiesceScope::ReleaseAndWaitForCores()
	{
		{
			std::unique_lock lock(s_barrierMutex);
			s_quiesceRequested.store(false, std::memory_order_relaxed);
		}
		s_barrierCv.notify_all();
		// Wait for every core to actually leave the barrier before undoing the run-queue
		// nudge, so the next quiesce starts from a known-zero park count.
		{
			std::unique_lock lock(s_barrierMutex);
			s_barrierCv.wait(lock, [] { return s_parkedCoreCount == 0; });
		}
		coreinit::__OSQuiesceReleaseCores();
	}
} // namespace SaveStates
