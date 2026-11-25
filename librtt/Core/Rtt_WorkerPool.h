//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#ifndef Rtt_WorkerPool_H
#define Rtt_WorkerPool_H

#include "Core/Rtt_Assert.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// ----------------------------------------------------------------------------

namespace Rtt
{

// ----------------------------------------------------------------------------

// Wait() runs a still-queued job on the calling thread, so a pool with zero
// workers (or busy workers) never blocks progress.
class WorkerPool
{
	public:
		typedef std::function< void() > Job;
		typedef std::function< void() > ThreadHook;

		enum State
		{
			kQueued,
			kRunning,
			kDone,
			kCancelled
		};

	private:
		struct Task
		{
			Task( const Job& job, size_t cost ) : fJob( job ), fCost( cost ), fState( kQueued ) {}

			Job fJob;
			size_t fCost;
			State fState;
		};

		typedef std::shared_ptr< Task > TaskPtr;
		typedef std::chrono::steady_clock Clock;

		// Largest cost first; equal costs keep submission order.
		typedef std::multimap< size_t, TaskPtr, std::greater< size_t > > Queue;

	public:
		typedef TaskPtr Ticket;

		static const unsigned kMaxWorkers = 4;
		static const unsigned kIdleDelayMs = 1000;

		static unsigned DefaultWorkerCount()
		{
			const unsigned cores = std::thread::hardware_concurrency();
			const unsigned spare = cores < 2 ? 0 : cores - 1;
			return spare < kMaxWorkers ? spare : kMaxWorkers;
		}

		// True while the calling thread runs a job of any pool, on a worker or
		// through Wait().
		static bool IsRunningJob() { return RunningJobMark(); }

	public:
		// The hooks run on each worker thread right after it starts and right
		// before it exits (e.g. to attach the thread to a VM). The idle hook runs
		// on one worker, once kIdleDelayMs have passed since the end of the last
		// job with none queued or running, and not again until a worker has run
		// another job.
		explicit WorkerPool(
			unsigned workerCount = DefaultWorkerCount(),
			const ThreadHook& onThreadStart = ThreadHook(),
			const ThreadHook& onThreadExit = ThreadHook(),
			const ThreadHook& onIdle = ThreadHook() )
		:	fWorkerCount( kThreadsAvailable ? workerCount : 0 ),
			fOnThreadStart( onThreadStart ),
			fOnThreadExit( onThreadExit ),
			fOnIdle( onIdle ),
			fRunningCount( 0 ),
			fIdleHookDue( false ),
			fStopping( false )
		{
		}

		// Every ticket must be waited on or cancelled before the pool is destroyed.
		~WorkerPool()
		{
			{
				std::lock_guard< std::mutex > lock( fMutex );
				Rtt_ASSERT( fQueue.empty() );
				fStopping = true;
			}
			fWorkAvailable.notify_all();
			for ( size_t i = 0; i < fWorkers.size(); i++ )
			{
				fWorkers[i].join();
			}
		}

	private:
		WorkerPool( const WorkerPool& );
		WorkerPool& operator=( const WorkerPool& );

	public:
		unsigned WorkerCount() const { return fWorkerCount; }

		Ticket Submit( const Job& job, size_t cost )
		{
			TaskPtr task = std::make_shared< Task >( job, cost );
			{
				std::lock_guard< std::mutex > lock( fMutex );
				StartWorkersOnce();
				fQueue.insert( Queue::value_type( cost, task ) );
			}
			fWorkAvailable.notify_one();
			return task;
		}

		void Wait( const Ticket& ticket )
		{
			std::unique_lock< std::mutex > lock( fMutex );
			if ( kQueued == ticket->fState )
			{
				Dequeue( ticket );
				Execute( ticket, lock );
				return;
			}
			fJobDone.wait( lock, [&ticket]{ return kRunning != ticket->fState; } );
		}

		bool Cancel( const Ticket& ticket )
		{
			std::lock_guard< std::mutex > lock( fMutex );
			if ( kQueued != ticket->fState )
			{
				return false;
			}
			Dequeue( ticket );
			ticket->fState = kCancelled;
			ticket->fJob = Job();
			return true;
		}

		State GetState( const Ticket& ticket ) const
		{
			std::lock_guard< std::mutex > lock( fMutex );
			return ticket->fState;
		}

	private:
#if defined( Rtt_EMSCRIPTEN_ENV )
		static const bool kThreadsAvailable = false;
#else
		static const bool kThreadsAvailable = true;
#endif

		static bool& RunningJobMark()
		{
			static thread_local bool sIsRunningJob = false;
			return sIsRunningJob;
		}

		// Caller holds fMutex.
		void StartWorkersOnce()
		{
			if ( ! fWorkers.empty() )
			{
				return;
			}
#if defined( __cpp_exceptions ) || defined( _CPPUNWIND )
			try { StartWorkers(); }
			catch ( const std::system_error& ) {} // Go on with the workers that did start
#else
			StartWorkers();
#endif
		}

		// Caller holds fMutex.
		void StartWorkers()
		{
			for ( unsigned i = 0; i < fWorkerCount; i++ )
			{
				fWorkers.push_back( std::thread( &WorkerPool::RunWorker, this ) );
			}
		}

		// Caller holds fMutex.
		void Dequeue( const TaskPtr& task )
		{
			std::pair< Queue::iterator, Queue::iterator > range = fQueue.equal_range( task->fCost );
			for ( Queue::iterator it = range.first; it != range.second; ++it )
			{
				if ( it->second == task )
				{
					fQueue.erase( it );
					return;
				}
			}
			Rtt_ASSERT_NOT_REACHED();
		}

		// Caller holds fMutex through lock; the job itself runs unlocked.
		void Execute( const TaskPtr& task, std::unique_lock< std::mutex >& lock )
		{
			task->fState = kRunning;
			++fRunningCount;
			lock.unlock();
			const bool wasRunningJob = RunningJobMark();
			RunningJobMark() = true;
			task->fJob();
			RunningJobMark() = wasRunningJob;
			task->fJob = Job();
			lock.lock();
			--fRunningCount;
			fLastJobEnd = Clock::now();
			task->fState = kDone;
			fJobDone.notify_all();
		}

		// Caller holds fMutex.
		bool HasWork() const { return fStopping || ! fQueue.empty(); }

		// Caller holds fMutex. The earliest time the idle hook may run.
		Clock::time_point IdleTime() const
		{
			const Clock::time_point start = fRunningCount > 0 ? Clock::now() : fLastJobEnd;
			return start + std::chrono::milliseconds( +kIdleDelayMs ); // + avoids an ODR use
		}

		// Caller holds fMutex through lock. False if IdleTime() passes first.
		bool WaitForWork( std::unique_lock< std::mutex >& lock )
		{
			return fWorkAvailable.wait_until( lock, IdleTime(), [this]{ return HasWork(); } );
		}

		// Caller holds fMutex through lock; the hook itself runs unlocked.
		void RunIdleHook( std::unique_lock< std::mutex >& lock )
		{
			fIdleHookDue = false;
			lock.unlock();
			fOnIdle();
			lock.lock();
		}

		void RunWorker()
		{
			if ( fOnThreadStart ) { fOnThreadStart(); }

			std::unique_lock< std::mutex > lock( fMutex );
			for ( ;; )
			{
				if ( fIdleHookDue && ! WaitForWork( lock ) )
				{
					// Another worker may have run the hook, or a job may have started or ended meanwhile
					if ( fIdleHookDue && Clock::now() >= IdleTime() ) { RunIdleHook( lock ); }
					continue;
				}
				fWorkAvailable.wait( lock, [this]{ return HasWork(); } );
				if ( fStopping )
				{
					break;
				}
				TaskPtr task = fQueue.begin()->second;
				fQueue.erase( fQueue.begin() );
				Execute( task, lock );
				fIdleHookDue = static_cast< bool >( fOnIdle );
			}
			lock.unlock();

			if ( fOnThreadExit ) { fOnThreadExit(); }
		}

	private:
		const unsigned fWorkerCount;
		const ThreadHook fOnThreadStart;
		const ThreadHook fOnThreadExit;
		const ThreadHook fOnIdle;

		mutable std::mutex fMutex;
		std::condition_variable fWorkAvailable;
		std::condition_variable fJobDone;
		Queue fQueue;
		std::vector< std::thread > fWorkers;
		unsigned fRunningCount; // Includes the jobs run by Wait() callers
		Clock::time_point fLastJobEnd;
		bool fIdleHookDue; // A worker has run a job since the last idle hook
		bool fStopping;
};

// ----------------------------------------------------------------------------

// Byte budget for decoded pixels in flight. Items are admitted strictly in
// the order they were pushed; an item larger than the cap is admitted only
// when nothing else is in flight. Admission does not care whether the item
// then goes to a worker or is processed inline. Not thread-safe: one thread
// (the render thread) owns it.
template < typename T >
class AdmissionQueue
{
	public:
		static const size_t kDecodedBytesCap = 64 * 1024 * 1024;

	public:
		explicit AdmissionQueue( size_t capacity = kDecodedBytesCap )
		:	fCapacity( capacity ),
			fInFlight( 0 )
		{
		}

	public:
		void Push( const T& item, size_t bytes )
		{
			fPending.push_back( std::make_pair( item, bytes ) );
		}

		// Pops and reserves the front item if the budget allows it.
		bool AdmitNext( T& item, size_t& bytes )
		{
			if ( fPending.empty() || ! Fits( fPending.front().second ) )
			{
				return false;
			}
			item = fPending.front().first;
			bytes = fPending.front().second;
			fPending.pop_front();
			fInFlight += bytes;
			return true;
		}

		void Release( size_t bytes )
		{
			Rtt_ASSERT( bytes <= fInFlight );
			fInFlight -= bytes;
		}

		size_t InFlight() const { return fInFlight; }
		bool HasPending() const { return ! fPending.empty(); }

	private:
		bool Fits( size_t bytes ) const
		{
			return 0 == fInFlight || fInFlight + bytes <= fCapacity;
		}

	private:
		const size_t fCapacity;
		size_t fInFlight;
		std::deque< std::pair< T, size_t > > fPending;
};

// ----------------------------------------------------------------------------

} // namespace Rtt

// ----------------------------------------------------------------------------

#endif // Rtt_WorkerPool_H
