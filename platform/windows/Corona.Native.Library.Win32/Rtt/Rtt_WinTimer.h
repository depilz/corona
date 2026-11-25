//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Core\Rtt_Build.h"
#include "Rtt_PlatformTimer.h"
#include "Interop\UI\UIComponent.h"
#include <atomic>
#include <thread>
#include <Windows.h>
#include <unordered_map>


namespace Interop { namespace UI {
	class MessageOnlyWindow;
} }


namespace Rtt
{

class WinTimer : public PlatformTimer
{
	Rtt_CLASS_NO_COPIES(WinTimer)

	public:
		/// <summary>Creates a new Win32 timer.</summary>
		/// <param name="callback">The callback to be invoked every time the timer elapses.</param>
		/// <param name="windowHandle">Handle to a window or control to attach the windows timer to. Can be null.</param>
		WinTimer(MCallback& callback, HWND windowHandle);

		/// <summary>Destroys the timer and its resources. Stops the timer if currently running.</summary>
		virtual ~WinTimer();

		/// <summary>Starts the timer.</summary>
		virtual void Start() override;

		/// <summary>Stops the timer, if running.</summary>
		virtual void Stop() override;

		/// <summary>Sets the timer's interval in milliseconds. This can be applied while the timer is running.</summary>
		/// <param name="milliseconds">
		///  <para>How often the timer will "elapse" and invoke its given callback when running.</para>
		///  <para>Cannot be set less than 10 milliseconds.</para>
		/// </param>
		virtual void SetInterval(U32 milliseconds) override;

		/// <summary>Determines if the timer is currently running.</summary>
		/// <returns>Returns true if the timer is currently running. Returns false if not started or stopped.</returns>
		virtual bool IsRunning() const override;

		/// <summary>Determines if this timer supports the given frame rate.</summary>
		/// <param name="fps">The frame rate to check.</param>
		/// <returns>Returns true for 120 and for the frame rates supported by the base class.</returns>
		virtual bool SupportsFrameRate(U8 fps) const override;

		/// <summary>
		///  <para>Checks if the running timer's interval has elapsed, and if it has, invokes its callback.</para>
		///  <para>This method is provided because a Windows system timer can trigger late.</para>
		///  <para>This method will not do anything if the timer is not running.</para>
		/// </summary>
		void Evaluate();

	private:
		/// <summary>
		///  <para>Starts the thread that ticks this timer at the monitor's rate, up to 120 times a second.</para>
		///  <para>The thread posts a private message to the calling thread's shared message-only window.</para>
		/// </summary>
		/// <returns>Returns true if the thread was started. Returns false if it could not be set up.</returns>
		bool StartTickThread();

		/// <summary>Stops and joins the tick thread and removes its message and handler, if started.</summary>
		void StopTickThread();

		/// <summary>The tick thread's loop, waiting on the waitable timer until the stop event is set.</summary>
		void RunTickThread(HWND messageWindowHandle);

		/// <summary>
		///  <para>Invokes the callback for the tick the tick thread posted, if it has not been invoked yet.</para>
		///  <para>Does not touch this timer after the callback if the callback stopped or deleted it.</para>
		/// </summary>
		void InvokePostedTick();

		/// <summary>Called when the shared message-only window receives a message.</summary>
		void OnReceivedMessage(Interop::UI::UIComponent& sender, Interop::UI::HandleMessageEventArgs& arguments);

		/// <summary>
		///  <para>Gets the refresh rate of the monitor showing the given window, or of the primary monitor.</para>
		///  <para>Returns 0 if unknown.</para>
		/// </summary>
		static U32 GetMonitorRefreshRate(HWND windowHandle);

		/// <summary>
		///  <para>Called by Windows when the system timer has elapsed.</para>
		///  <para>Calls WinTimer's Evaluate() function to see if it is time to invoke its callback.</para>
		/// </summary>
		static VOID CALLBACK OnTimerElapsed(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);

		/// <summary>
		///  <para>Compares the given tick values returned by ::GetTickCount().</para>
		///  <para>
		///   Correctly handles tick overflow where large negative numbers are considered greater than
		///   large positive numbers.
		///  </para>
		/// </summary>
		/// <param name="x">Ticks value to be compared against argument "y".</para>
		/// <param name="y">Ticks value to be compared against argument "x".</para>
		/// <returns>
		///  <para>Returns a positive value if "x" is greater than "y".</para>
		///  <para>Returns zero if "x" is equal to "y".</para>
		///  <para>Returns a negative value if "x" is less than "y".</para>
		/// </returns>
		static S32 CompareTicks(S32 x, S32 y);

		HWND fWindowHandle;
		UINT_PTR fTimerPointer;
		UINT_PTR fTimerID;
		U32 fIntervalInMilliseconds;
		S32 fNextIntervalTimeInTicks;

		std::thread fTickThread;
		HANDLE fStopEventHandle;
		HANDLE fWaitableTimerHandle;
		Interop::UI::MessageOnlyWindow* fMessageOnlyWindowPointer;
		UINT fTickMessageId;
		Interop::UI::UIComponent::ReceivedMessageEvent::MethodHandler<WinTimer> fReceivedMessageEventHandler;
		std::atomic<int> fTickState;

		static std::unordered_map<UINT_PTR, WinTimer*> sTimerMap; // timer callback might be called after Stop(), so use this as a guard
		static UINT_PTR sMostRecentTimerID; // use an incrementing index as key, to be robust against the rare case that a new timer is reallocated into the same memory
};

}	// namespace Rtt
