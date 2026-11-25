//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Rtt_WinTimer.h"
#include "Interop\UI\MessageOnlyWindow.h"
#include <system_error>
#include <windows.h>
#include <mmsystem.h>


namespace Rtt
{

std::unordered_map<UINT_PTR, Rtt::WinTimer *> WinTimer::sTimerMap;
UINT_PTR WinTimer::sMostRecentTimerID;

enum
{
	kTickIdle,
	kTickPosted,
	kTickRunning
};

#pragma region Constructors/Destructors
WinTimer::WinTimer(MCallback& callback, HWND windowHandle)
:	PlatformTimer(callback),
	fReceivedMessageEventHandler(this, &WinTimer::OnReceivedMessage)
{
	fWindowHandle = windowHandle;
	fTimerPointer = NULL;
	fTimerID = 0;
	fIntervalInMilliseconds = 10;
	fNextIntervalTimeInTicks = 0;
	fStopEventHandle = nullptr;
	fWaitableTimerHandle = nullptr;
	fMessageOnlyWindowPointer = nullptr;
	fTickMessageId = 0;
	fTickState = kTickIdle;
}

WinTimer::~WinTimer()
{
	Stop();
}

#pragma endregion


#pragma region Public Methods
void WinTimer::Start()
{
	// Do not continue if the timer is already running.
	if (IsRunning())
	{
		return;
	}

	// Start the timer, but with an interval faster than the configured interval.
	// We do this because Windows timers can invoke later than expected.
	// To compensate, we'll schedule when to invoke the timer's callback using "fIntervalEndTimeInTicks".
	fNextIntervalTimeInTicks = (S32)::GetTickCount() + (S32)fIntervalInMilliseconds;
	fTimerID = ++sMostRecentTimerID; // ID should be non-0, so pre-increment for first time

	// 8 ms means fps 120: tick at the monitor's rate up to 120 from a thread, since system timers tick at 64 Hz at most.
	if ((8 == fIntervalInMilliseconds) && StartTickThread())
	{
		sTimerMap[fTimerID] = this;
		return;
	}

	fTimerPointer = ::SetTimer(fWindowHandle, fTimerID, 10, WinTimer::OnTimerElapsed);

	if (IsRunning())
	{
		sTimerMap[fTimerID] = this;
	}
}

void WinTimer::Stop()
{
	// Do not continue if the timer has already been stopped.
	if (IsRunning() == false)
	{
		return;
	}

	// Stop the timer.
	if (fTickThread.joinable())
	{
		StopTickThread();
	}
	else
	{
		::KillTimer(fWindowHandle, fTimerID);
	}

	sTimerMap.erase(fTimerID);

	fTimerPointer = NULL;
	fTimerID = 0;
}

void WinTimer::SetInterval(U32 milliseconds)
{
	fIntervalInMilliseconds = milliseconds;
}

bool WinTimer::IsRunning() const
{
	return (fTimerPointer != NULL) || fTickThread.joinable();
}

bool WinTimer::SupportsFrameRate(U8 fps) const
{
	return (120 == fps) || PlatformTimer::SupportsFrameRate(fps);
}

void WinTimer::Evaluate()
{
	// Do not continue if the timer is not running.
	if (IsRunning() == false)
	{
		return;
	}

	if (fTickThread.joinable())
	{
		if (kTickPosted == fTickState)
		{
			fMessageOnlyWindowPointer->RemoveMessagesById(fTickMessageId);
			InvokePostedTick();
		}
		return;
	}

	// Do not continue if we haven't reached the scheduled time yet.
	if (CompareTicks((S32)::GetTickCount(), fNextIntervalTimeInTicks) < 0)
	{
		return;
	}

	// Schedule the next interval time.
	for (; CompareTicks((S32)::GetTickCount(), fNextIntervalTimeInTicks) > 0; fNextIntervalTimeInTicks += fIntervalInMilliseconds);

	// Invoke this timer's callback.
	this->operator()();
}

#pragma endregion


#pragma region Private Methods/Functions
bool WinTimer::StartTickThread()
{
	fMessageOnlyWindowPointer = Interop::UI::MessageOnlyWindow::GetSharedInstanceForCurrentThread();
	HWND messageWindowHandle = fMessageOnlyWindowPointer ? fMessageOnlyWindowPointer->GetWindowHandle() : nullptr;
	if (messageWindowHandle)
	{
		fMessageOnlyWindowPointer->GetReceivedMessageEventHandlers().Add(&fReceivedMessageEventHandler);
		fTickMessageId = fMessageOnlyWindowPointer->ReserveMessageId();
	}

	fWaitableTimerHandle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!fWaitableTimerHandle)
	{
		fWaitableTimerHandle = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	}
	fStopEventHandle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	fTickState = kTickIdle;

	if (fTickMessageId && fWaitableTimerHandle && fStopEventHandle)
	{
		try
		{
			fTickThread = std::thread(&WinTimer::RunTickThread, this, messageWindowHandle);
		}
		catch (const std::system_error&)
		{
		}
	}
	if (fTickThread.joinable())
	{
		return true;
	}

	StopTickThread();
	static bool sWasFailureLogged = false;
	if (!sWasFailureLogged)
	{
		sWasFailureLogged = true;
		Rtt_LogException("WARNING: Failed to start the 120 fps timer thread. Using a slower system timer instead.\n");
	}
	return false;
}

void WinTimer::StopTickThread()
{
	if (fTickThread.joinable())
	{
		::SetEvent(fStopEventHandle);
		fTickThread.join();
	}
	if (fWaitableTimerHandle)
	{
		::CloseHandle(fWaitableTimerHandle);
		fWaitableTimerHandle = nullptr;
	}
	if (fStopEventHandle)
	{
		::CloseHandle(fStopEventHandle);
		fStopEventHandle = nullptr;
	}
	if (fMessageOnlyWindowPointer)
	{
		if (fTickMessageId)
		{
			fMessageOnlyWindowPointer->RemoveMessagesById(fTickMessageId);
			fMessageOnlyWindowPointer->UnreserveMessageId(fTickMessageId);
			fTickMessageId = 0;
		}
		fMessageOnlyWindowPointer->GetReceivedMessageEventHandlers().Remove(&fReceivedMessageEventHandler);
		fMessageOnlyWindowPointer = nullptr;
	}
}

void WinTimer::RunTickThread(HWND messageWindowHandle)
{
	bool usesTimePeriod = (TIMERR_NOERROR == ::timeBeginPeriod(1));

	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;
	::QueryPerformanceFrequency(&frequency);
	::QueryPerformanceCounter(&counter);
	LONGLONG dueTime = counter.QuadPart;
	LONGLONG nextRateReadTime = counter.QuadPart;
	LONGLONG period = 0;
	HANDLE handles[] = { fStopEventHandle, fWaitableTimerHandle };
	while (true)
	{
		::QueryPerformanceCounter(&counter);
		if (counter.QuadPart >= nextRateReadTime)
		{
			U32 rate = GetMonitorRefreshRate(fWindowHandle);
			period = frequency.QuadPart / (((rate > 1) && (rate < 120)) ? rate : 120);
			nextRateReadTime = counter.QuadPart + frequency.QuadPart;
		}

		dueTime += period;
		if (counter.QuadPart - dueTime > period)
		{
			dueTime = counter.QuadPart;
		}
		LARGE_INTEGER relativeTime;
		relativeTime.QuadPart = -((dueTime > counter.QuadPart) ? ((dueTime - counter.QuadPart) * 10000000 / frequency.QuadPart) : 0);
		::SetWaitableTimer(fWaitableTimerHandle, &relativeTime, 0, nullptr, nullptr, FALSE);
		if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE) != (WAIT_OBJECT_0 + 1))
		{
			break;
		}

		// At most one tick message is pending, and a deadline that finds the last tick still running is skipped, so WM_PAINT is served.
		int expectedState = kTickIdle;
		if (fTickState.compare_exchange_strong(expectedState, kTickPosted) && !::PostMessageW(messageWindowHandle, fTickMessageId, 0, 0))
		{
			fTickState = kTickIdle;
		}
	}

	if (usesTimePeriod)
	{
		::timeEndPeriod(1);
	}
}

void WinTimer::InvokePostedTick()
{
	int expectedState = kTickPosted;
	if (!fTickState.compare_exchange_strong(expectedState, kTickRunning))
	{
		return;
	}

	auto timerID = fTimerID;
	this->operator()();

	auto timer = sTimerMap.find(timerID);
	if ((sTimerMap.end() != timer) && (this == timer->second))
	{
		fTickState = kTickIdle;
	}
}

void WinTimer::OnReceivedMessage(Interop::UI::UIComponent& sender, Interop::UI::HandleMessageEventArgs& arguments)
{
	if (arguments.WasHandled() || (arguments.GetMessageId() != fTickMessageId))
	{
		return;
	}

	arguments.SetHandled();
	arguments.SetReturnResult(0);
	InvokePostedTick();
}

U32 WinTimer::GetMonitorRefreshRate(HWND windowHandle)
{
	HMONITOR monitorHandle = ::MonitorFromWindow(windowHandle, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFOEXW monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!monitorHandle || !::GetMonitorInfoW(monitorHandle, &monitorInfo))
	{
		return 0;
	}

	DEVMODEW displayMode{};
	displayMode.dmSize = sizeof(displayMode);
	if (!::EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode))
	{
		return 0;
	}
	return displayMode.dmDisplayFrequency;
}

VOID CALLBACK WinTimer::OnTimerElapsed(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	auto timer = sTimerMap.find(idEvent);
	if (sTimerMap.end() != timer)
	{
		timer->second->Evaluate();
	}
}

S32 WinTimer::CompareTicks(S32 x, S32 y)
{
	// Overflow will occur when flipping sign bit of largest negative number.
	// Give it a one millisecond boost before flipping the sign.
	if (0x80000000 == y)
	{
		y++;
	}

	// Compare the given tick values via subtraction. Overlow for this subtraction operation is okay.
	S32 deltaTime = x - y;
	if (deltaTime < 0)
	{
		return -1;
	}
	else if (0 == deltaTime)
	{
		return 0;
	}
	return 1;
}

#pragma endregion

}	// namespace Rtt
