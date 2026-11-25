//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#include "Core/Rtt_Build.h"

#include "Rtt_AppleTimer.h"

#import "Rtt_AppleCallback.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#ifdef Rtt_MAC_ENV
	#import <AppKit/AppKit.h>
#endif

#ifdef Rtt_IPHONE_ENV
	#include "Rtt_IPhoneConstants.h"
	#import <UIKit/UIDevice.h>
#endif

// For background processes, we don't want to suck up the CPU resources.
// Set the timer interval to something that is low impact.
// 1 second usually is sufficient to not be noticed on the CPU monitors.
#define RTT_NICE_BACKGROUND_TIMER_INTERVAL 3.0
// ----------------------------------------------------------------------------

namespace Rtt
{

// ----------------------------------------------------------------------------

#ifdef Rtt_MAC_ENV

static double
CoreGraphicsRate( double modeRate )
{
	return modeRate > 0.0 ? modeRate : 60.0;
}

static double
ScreenRate( NSView *view )
{
	NSScreen *screen = [[view window] screen];
	if ( ! screen )
	{
		return 0.0;
	}

	if ( @available( macOS 12.0, * ) )
	{
		return [screen maximumFramesPerSecond];
	}

	CGDirectDisplayID display = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode( display );
	if ( ! mode )
	{
		return 0.0;
	}

	double rate = CoreGraphicsRate( CGDisplayModeGetRefreshRate( mode ) );
	CGDisplayModeRelease( mode );
	return rate;
}

static double
TickRate( double screenRate, double lastRate )
{
	return screenRate > 0.0 ? ( screenRate < 120.0 ? screenRate : 120.0 ) : lastRate;
}

static void
RemoveObserver( id& observer )
{
	if ( observer )
	{
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
		[observer release];
		observer = nil;
	}
}

#endif

// Timer is optimized for rendering. To avoid screen tearing and stuttering,
// timing should synchronized with the refresh rate of the display.
// 
// * (TODO:) On Mac OS 10.4 or later, use CVDisplayLink: http://developer.apple.com/mac/library/qa/qa2004/qa1385.html
// * On iPhone OS 3.1 or later, use CADisplayLink.
// 
AppleTimer::AppleTimer( MCallback& callback, id view )
:	Super( callback ),
	fDisplayLink( nil ),
	fTimer( nil ),
	fTarget( [[AppleCallback alloc] init] ),
	fInterval( 0x8000000 ),
	fDisplayLinkSupported( false ),
	fSavedInterval( fInterval ),
	fView( [view retain] ),
	fScreenObserver( nil ),
	fWindowObserver( nil ),
	fTickRate( 120.0 )
{
#if (MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_5)
	fTarget.callback = & callback;
#else
	[fTarget setCallback: & callback];
#endif

#ifdef Rtt_IPHONE_ENV
	fDisplayLinkSupported = ( NSClassFromString(@"CADisplayLink") != nil );
#endif
}

AppleTimer::~AppleTimer()
{
	Stop();
	[fTarget release];
	[fDisplayLink release];
	[fView release];
}

void
AppleTimer::SwitchToForegroundTimer()
{
#ifdef Rtt_IPHONE_ENV
	if ( nil != fDisplayLink )
	{
		// already using displayLink timer
		return;
	}
	bool wasRunning = IsRunning();
	Stop();
	fInterval = fSavedInterval;
	fDisplayLinkSupported = ( NSClassFromString(@"CADisplayLink") != nil );
	if ( wasRunning )
	{
		Start();
	}
#endif
}

void
AppleTimer::SwitchToBackgroundTimer()
{
#ifdef Rtt_IPHONE_ENV
	/* skip check because interval may need to change */
	 /*
	if ( nil != fTimer )
	{
		// already using NSTimer timer
		return;
	}
	 */
	bool wasRunning = IsRunning();
	Stop();
	fDisplayLinkSupported = NO;
	fSavedInterval = fInterval;
	fInterval = RTT_NICE_BACKGROUND_TIMER_INTERVAL;
	if ( wasRunning )
	{
		Start();
	}
#endif
}
	
void
AppleTimer::Start()
{
	if ( IsRunning() )
	{
		return;
	}
	NSTimeInterval interval = ((NSTimeInterval)fInterval) / 1000.0;

#ifdef Rtt_IPHONE_ENV
	if ( fDisplayLinkSupported )
	{
		// interval is number of screen refreshes. on iPhone the screen refresh rate is 60Hz
		// fInterval is measured in milliseconds, so 33.3 is 30fps and 16.7 is 60 fps
		NSInteger interval = ( fInterval < 33 ? 1 : 2 );

		fDisplayLink = [CADisplayLink displayLinkWithTarget:fTarget selector:@selector(invoke:)];
		[fDisplayLink setFrameInterval:interval];
		[fDisplayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
	}
	else
#endif
	{
#ifdef Rtt_MAC_ENV
		// 8 ms means fps 120: tick at the screen's rate up to 120, so a 60 Hz screen gets 60.
		if ( 8 == fInterval )
		{
			fTickRate = TickRate( ScreenRate( fView ), fTickRate );
			interval = 1.0 / fTickRate;

			if ( ! fScreenObserver )
			{
				void (^screenChanged)( NSNotification * ) = ^( NSNotification * )
				{
					double lastRate = fTickRate;
					fTickRate = TickRate( ScreenRate( fView ), fTickRate );
					if ( fTickRate != lastRate && [fTimer isValid] )
					{
						[fTimer invalidate];
						[fTimer release];
						fTimer = nil;
						Start();
					}
				};

				NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
				fScreenObserver = [[center addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil usingBlock:screenChanged] retain];
				NSWindow *window = [fView window];
				if ( window )
				{
					fWindowObserver = [[center addObserverForName:NSWindowDidChangeScreenNotification object:window queue:nil usingBlock:screenChanged] retain];
				}
			}
		}
#endif

		fTimer = [NSTimer
					scheduledTimerWithTimeInterval:interval
					target:fTarget
					selector:@selector(invoke:)
					userInfo:nil
					repeats:YES];
		[fTimer retain];

#ifdef Rtt_MAC_ENV
		// For single threaded apps like this one,
		// Cocoa seems to block timers or events sometimes. This can be seen
		// when I'm animating (via a timer) and you open an popup box or move a slider.
		// Apparently, sheets and dialogs can also block (try printing).
		// To work around this, Cocoa provides different run-loop modes. I need to
		// specify the modes to avoid the blockage.
		// NSDefaultRunLoopMode seems to be the default. I don't think I need to explicitly
		// set this one, but just in case, I will set it anyway.
		[[NSRunLoop currentRunLoop] addTimer:fTimer forMode:NSDefaultRunLoopMode];
		// This seems to be the one for preventing blocking on other events (popup box, slider, etc)
		[[NSRunLoop currentRunLoop] addTimer:fTimer forMode:NSEventTrackingRunLoopMode];
		// This seems to be the one for dialogs.
		[[NSRunLoop currentRunLoop] addTimer:fTimer forMode:NSModalPanelRunLoopMode];
#endif
	}
}

void
AppleTimer::Stop()
{
#ifdef Rtt_IPHONE_ENV
	if ( fDisplayLink )
	{
		[fDisplayLink invalidate]; // implicitly releases itself
		fDisplayLink = nil;
	}
#endif
	[fTimer invalidate];
	[fTimer release];
	fTimer = nil;

#ifdef Rtt_MAC_ENV
	RemoveObserver( fScreenObserver );
	RemoveObserver( fWindowObserver );
#endif
}

void
AppleTimer::SetInterval( U32 milliseconds )
{
	if ( fInterval != milliseconds )
	{
		bool wasRunning = IsRunning();

		if ( wasRunning ) { Stop(); }

		fInterval = milliseconds;

		if ( wasRunning ) { Start(); }
	}
}

bool
AppleTimer::IsRunning() const
{
	return ( ( nil != fDisplayLink ) || ( true == [fTimer isValid] ) );
}

bool
AppleTimer::SupportsFrameRate( U8 fps ) const
{
#ifdef Rtt_MAC_ENV
	return 120 == fps || Super::SupportsFrameRate( fps );
#else
	return Super::SupportsFrameRate( fps );
#endif
}

// ----------------------------------------------------------------------------

} // namespace Rtt

// ----------------------------------------------------------------------------

