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

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stddef.h>
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

		// Largest cost first; equal costs keep submission order.
		typedef std::multimap< size_t, TaskPtr, std::greater< size_t > > Queue;

	public:
		typedef TaskPtr Ticket;

		static const unsigned kMaxWorkers = 4;

		static unsigned DefaultWorkerCount()
		{
			const unsigned cores = std::thread::hardware_concurrency();
			const unsigned spare = cores < 2 ? 0 : cores - 1;
			return spare < kMaxWorkers ? spare : kMaxWorkers;
		}

		// True on the worker threads of any pool. A thread that runs a job
		// through Wait() does not become a worker.
		static bool IsWorkerThread() { return WorkerThreadMark(); }

	public:
		// The hooks run on each worker thread right after it starts and right
		// before it exits (e.g. to attach the thread to a VM).
		explicit WorkerPool(
			unsigned workerCount = DefaultWorkerCount(),
			const ThreadHook& onThreadStart = ThreadHook(),
			const ThreadHook& onThreadExit = ThreadHook() )
		:	fWorkerCount( kThreadsAvailable ? workerCount : 0 ),
			fOnThreadStart( onThreadStart ),
			fOnThreadExit( onThreadExit ),
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

		static bool& WorkerThreadMark()
		{
			static thread_local bool sIsWorkerThread = false;
			return sIsWorkerThread;
		}

		// Caller holds fMutex.
		void StartWorkersOnce()
		{
			if ( ! fWorkers.empty() )
			{
				return;
			}
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
			lock.unlock();
			task->fJob();
			task->fJob = Job();
			lock.lock();
			task->fState = kDone;
			fJobDone.notify_all();
		}

		void RunWorker()
		{
			WorkerThreadMark() = true;
			if ( fOnThreadStart ) { fOnThreadStart(); }

			std::unique_lock< std::mutex > lock( fMutex );
			for ( ;; )
			{
				fWorkAvailable.wait( lock, [this]{ return fStopping || ! fQueue.empty(); } );
				if ( fStopping )
				{
					break;
				}
				TaskPtr task = fQueue.begin()->second;
				fQueue.erase( fQueue.begin() );
				Execute( task, lock );
			}
			lock.unlock();

			if ( fOnThreadExit ) { fOnThreadExit(); }
		}

	private:
		const unsigned fWorkerCount;
		const ThreadHook fOnThreadStart;
		const ThreadHook fOnThreadExit;

		mutable std::mutex fMutex;
		std::condition_variable fWorkAvailable;
		std::condition_variable fJobDone;
		Queue fQueue;
		std::vector< std::thread > fWorkers;
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
