//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

package com.ansca.corona;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Choreographer;

import java.util.Arrays;

class VsyncFrameTimer implements Choreographer.FrameCallback {
	static final int HIGH_FRAME_RATE = 120;

	private static final long HIGH_RATE_PERIOD_NANOS = 1000000000L / HIGH_FRAME_RATE;
	private static final long MAX_HIGH_RATE_VSYNC_DELTA_NANOS = 40000000;
	private static final int VSYNC_SAMPLE_COUNT = 15;
	private static final double MAX_LOCK_ERROR = 0.05;
	private static final double MAX_LOCKED_ERROR = 0.08;
	private static final long REQUEST_LEAD_BEFORE_NEXT_VSYNC_NANOS = 2500000;

	private final Handler myMainHandler;
	private final Runnable myFrameRequest;

	private volatile int myGeneration;
	private volatile boolean myIsRunning;
	private volatile long myContentPeriodNanos;

	private int myArmedGeneration = -1;
	private long myLastFrameNanos;
	private long myLastFireNanos;
	private long myDueNanos;
	private boolean myIsLockedToVsync;
	private final long[] myVsyncDeltas = new long[VSYNC_SAMPLE_COUNT];
	private final long[] mySortedVsyncDeltas = new long[VSYNC_SAMPLE_COUNT];
	private int myVsyncDeltaCount;
	private int myNextVsyncDeltaIndex;
	private boolean myHasLoggedArm;

	VsyncFrameTimer(Handler mainHandler, Runnable frameRequest) {
		myMainHandler = mainHandler;
		myFrameRequest = frameRequest;
	}

	static boolean isHighRateInterval(int milliseconds) {
		return milliseconds <= 12;
	}

	boolean isRunning() {
		return myIsRunning;
	}

	synchronized void start(int milliseconds) {
		myContentPeriodNanos = contentPeriodNanos(milliseconds);
		if (myIsRunning) {
			return;
		}
		myIsRunning = true;
		final int generation = myGeneration;
		myMainHandler.post(new Runnable() {
			@Override
			public void run() {
				arm(generation);
			}
		});
	}

	synchronized void stop() {
		if (!myIsRunning) {
			return;
		}
		myIsRunning = false;
		myGeneration++;
		myMainHandler.removeCallbacks(myFrameRequest);
		if (Looper.myLooper() == Looper.getMainLooper()) {
			Choreographer.getInstance().removeFrameCallback(this);
		}
	}

	@Override
	public void doFrame(long frameTimeNanos) {
		if (myArmedGeneration != myGeneration) {
			return;
		}
		Choreographer.getInstance().postFrameCallback(this);
		updateVsyncPeriod(frameTimeNanos);
		long vsyncPeriod = medianVsyncPeriodNanos();
		if (isFrameDue(frameTimeNanos, vsyncPeriod)) {
			long requestNanos = frameTimeNanos + vsyncPeriod - REQUEST_LEAD_BEFORE_NEXT_VSYNC_NANOS;
			myMainHandler.postAtTime(myFrameRequest, (requestNanos + 999999) / 1000000);
		}
	}

	private void arm(int generation) {
		if (generation != myGeneration) {
			return;
		}
		myArmedGeneration = generation;
		myLastFrameNanos = 0;
		myLastFireNanos = 0;
		myDueNanos = 0;
		myIsLockedToVsync = false;
		myVsyncDeltaCount = 0;
		myNextVsyncDeltaIndex = 0;
		myMainHandler.removeCallbacks(myFrameRequest);
		Choreographer choreographer = Choreographer.getInstance();
		choreographer.removeFrameCallback(this);
		choreographer.postFrameCallback(this);
		if (!myHasLoggedArm) {
			myHasLoggedArm = true;
			Log.i("Corona", "Frames paced by display vsync");
		}
	}

	private void updateVsyncPeriod(long frameTimeNanos) {
		long delta = frameTimeNanos - myLastFrameNanos;
		if (myLastFrameNanos != 0 && delta > 0 && delta <= maxVsyncDeltaNanos()) {
			myVsyncDeltas[myNextVsyncDeltaIndex] = delta;
			myNextVsyncDeltaIndex = (myNextVsyncDeltaIndex + 1) % VSYNC_SAMPLE_COUNT;
			myVsyncDeltaCount = Math.min(myVsyncDeltaCount + 1, VSYNC_SAMPLE_COUNT);
		}
		myLastFrameNanos = frameTimeNanos;
	}

	private long maxVsyncDeltaNanos() {
		long contentPeriod = myContentPeriodNanos;
		// Most panels have vsyncs longer than this content period, and they must still be measured.
		return (contentPeriod == HIGH_RATE_PERIOD_NANOS) ? MAX_HIGH_RATE_VSYNC_DELTA_NANOS : contentPeriod;
	}

	private long medianVsyncPeriodNanos() {
		if (myVsyncDeltaCount == 0) {
			return 0;
		}
		System.arraycopy(myVsyncDeltas, 0, mySortedVsyncDeltas, 0, myVsyncDeltaCount);
		Arrays.sort(mySortedVsyncDeltas, 0, myVsyncDeltaCount);
		return mySortedVsyncDeltas[myVsyncDeltaCount / 2];
	}

	private boolean isFrameDue(long frameTimeNanos, long vsyncPeriod) {
		long contentPeriod = myContentPeriodNanos;
		if (myLastFireNanos == 0 || vsyncPeriod == 0) {
			markFired(frameTimeNanos, frameTimeNanos + contentPeriod);
			return true;
		}

		long vsyncsPerFrame = Math.max(1, Math.round((double)contentPeriod / vsyncPeriod + 0.05));
		double lockError = Math.abs(vsyncsPerFrame * vsyncPeriod - contentPeriod) / (double)contentPeriod;
		myIsLockedToVsync = lockError <= (myIsLockedToVsync ? MAX_LOCKED_ERROR : MAX_LOCK_ERROR);
		if (myIsLockedToVsync) {
			long elapsedVsyncs = Math.round((double)(frameTimeNanos - myLastFireNanos) / vsyncPeriod);
			if (elapsedVsyncs < vsyncsPerFrame) {
				return false;
			}
			markFired(frameTimeNanos, frameTimeNanos + contentPeriod);
			return true;
		}

		if (frameTimeNanos - myDueNanos > contentPeriod) {
			myDueNanos = frameTimeNanos;
		}
		// Half a vsync of tolerance, so jitter in the vsync timestamps neither skips nor doubles a frame.
		if (frameTimeNanos < myDueNanos - vsyncPeriod / 2) {
			return false;
		}
		markFired(frameTimeNanos, myDueNanos + contentPeriod);
		return true;
	}

	private void markFired(long frameTimeNanos, long nextDueNanos) {
		myLastFireNanos = frameTimeNanos;
		myDueNanos = nextDueNanos;
	}

	private static long contentPeriodNanos(int milliseconds) {
		int fps;
		if (isHighRateInterval(milliseconds)) {
			fps = HIGH_FRAME_RATE;
		} else if (milliseconds <= 20) {
			fps = 60;
		} else if (milliseconds <= 40) {
			fps = 30;
		} else {
			fps = Math.max(1, Math.round(1000f / milliseconds));
		}
		return 1000000000L / fps;
	}
}
