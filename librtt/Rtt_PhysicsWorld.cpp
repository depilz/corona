//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#include "Core/Rtt_Build.h"

#include "Rtt_PhysicsWorld.h"

#include "Box2D/Box2D.h"
#include "b2GLESDebugDraw.h"

#include "Display/Rtt_Display.h"
#include "Display/Rtt_DisplayObject.h"
#include "Rtt_LuaAux.h"
#include "Rtt_LuaLibPhysics.h"
#include "Rtt_Runtime.h"
#include "Rtt_PhysicsContactListener.h"

// ----------------------------------------------------------------------------

namespace Rtt
{

// ----------------------------------------------------------------------------

// These iterations are reasonable default values. See http://www.box2d.org/forum/viewtopic.php?f=8&t=4396 for discussion.
const S32 kVelocityIterations = 8;
const S32 kPositionIterations = 3;

const S32 kMaxRealTimeSteps = 4;
const double kRealTimeLeak = 0.02;

// ----------------------------------------------------------------------------

class PhysicsDestructionListener : public b2DestructionListener
{
	public:
		virtual void SayGoodbye(b2Joint* joint);
		virtual void SayGoodbye(b2Fixture* fixture);
};

void
PhysicsDestructionListener::SayGoodbye(b2Joint* joint)
{
	UserdataWrapper *wrapper = (UserdataWrapper *)joint->GetUserData();

	// Check that wrapper is valid. If Lua GC'd the wrapper, then we already set the joint's ud to NULL
	if ( wrapper && UserdataWrapper::GetFinalizedValue() != wrapper )
	{
		wrapper->Invalidate();
	}
}

void
PhysicsDestructionListener::SayGoodbye(b2Fixture* fixture)
{
}

// ----------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld( Rtt_Allocator& allocator )
:	fAllocator( allocator ),
	fWorldDebugDraw( NULL ),
	fWorldDestructionListener( NULL ),
	fWorldContactListener( NULL ),
	fReportCollisionsInContentCoordinates( false ),
	fLuaAssertEnabled( false ),
	fAverageCollisionPositions( false ),
	fProperties( 0 ),
	fWorld( NULL ),
	fPixelsPerMeter( 30.0f ), // default on iPhone
	fGroundBody( NULL ),
	fVelocityIterations( kVelocityIterations ),
	fPositionIterations( kPositionIterations ),
	fFrameInterval( -1.0f ),
	fTimeStep( -1.0f ), // Set time step equal to frame interval
	fTimeScale( 1.0f ),
	fTimePrevious( -1.f ),
	fTimeRemainder( 0.0f ),
	fFollowsRealTime( false ),
	fIsTimeStepDefault( true ),
	fRealTimePrevious( -1. ),
	fRealTimeRemainder( 0. )
{
}

PhysicsWorld::~PhysicsWorld()
{
	if ( fWorld )
	{
		fWorld->SetContactListener( NULL );
	}

	StopWorld();
}

void
PhysicsWorld::Initialize( float frameInterval, bool followsRealTime )
{
	Rtt_ASSERT( fFrameInterval < 0.f );

	fFrameInterval = frameInterval;
	fFollowsRealTime = followsRealTime;
}

void
PhysicsWorld::WillDestroyDisplay()
{
	if ( fWorld )
	{
		fWorld->SetContactListener( NULL );
	}
}

void
PhysicsWorld::StartWorld( Runtime& runtime, bool noSleep )
{
	if ( ! fWorld )
	{
		Rtt_ASSERT( ! IsProperty( kIsWorldRunning ) );

		// Note that gravity is oriented along positive y-axis in Corona coordinates
		// (we are flipping the "handedness" of the Box2d world to make the coordinate system the same as Corona)
		// Hence, in the shape API we declare polygon coordinates in clockwise (rather than counterclockwise) order, due to this world inversion
		
		// Default to Earthlike gravity
		b2Vec2 gravity( 0.0f, 9.8f );

		SetVelocityIterations( kVelocityIterations );
		SetPositionIterations( kPositionIterations );
		SetTimeStep( -1.f ); // Set time step equal to frame interval
		fTimePrevious = -1.f;
		fTimeRemainder = 0.f;

		fWorld = Rtt_NEW( Allocator(), b2World( gravity ) );
		fWorldDestructionListener = Rtt_NEW( Allocator(), PhysicsDestructionListener );
		fWorld->SetDestructionListener( fWorldDestructionListener );
		
		// The noSleep flag sets whether to simulate inactive bodies, or allow them to "sleep" after a few seconds
		// of no interaction. The recommended default is to allow sleep. Our exposed boolean should be the opposite,
		// so that it can default to false, as expected for all Corona booleans.
		fWorld->SetAllowSleeping( !noSleep );
		
		// More world setup
		fWorldContactListener = Rtt_NEW( Allocator(), PhysicsContactListener( runtime ) );
		fWorld->SetContactListener( fWorldContactListener );

		fWorldDebugDraw = Rtt_NEW( Allocator(), b2GLESDebugDraw( runtime.GetDisplay() ) );

		uint32 debugFlags =
			b2Draw::e_shapeBit |
			b2Draw::e_jointBit |
//			b2Draw::e_aabbBit |
			b2Draw::e_pairBit |
			b2Draw::e_centerOfMassBit |
			b2Draw::e_particleBit;
		fWorldDebugDraw->AppendFlags( debugFlags );

		fWorld->SetDebugDraw( fWorldDebugDraw );

		// Initialize a ground body, so that joints can be attached to "the world"
		b2BodyDef bd;
		bd.userData = const_cast< void* >( LuaLibPhysics::GetGroundBodyUserdata() );
		fGroundBody = fWorld->CreateBody(&bd);
	}

	if ( ! IsProperty( kIsWorldRunning ) )
	{
		ResetRealTime();
	}

	SetProperty( kIsWorldRunning, true );
}

void
PhysicsWorld::PauseWorld()
{
	if ( fWorld )
	{
		SetProperty( kIsWorldRunning, false );
	}
}

void
PhysicsWorld::StopWorld()
{
	if ( fWorld )
	{
		SetProperty( kIsWorldRunning, false );

		fWorld->SetContactListener( NULL );

		// The b2World is about to destroy the block allocator that owns
		// the memory for b2Body objects, so we have to pre-emptively
		// iterate over bodies and detach from display object
		const void *groundBodyUserdata = LuaLibPhysics::GetGroundBodyUserdata();

		for ( b2Body *body = fWorld->GetBodyList(), *nextBody = NULL;
			  NULL != body;
			  body = nextBody )
		{
			// Prefetch next body before we delete body
			nextBody = body->GetNext();

			if ( body->GetUserData() )
			{
				if ( body->GetUserData() != groundBodyUserdata )
				{
					DisplayObject *o = (DisplayObject*)body->GetUserData();
					o->RemoveExtensions();
				}
			}

			fWorld->DestroyBody( body );
		}

		Rtt_DELETE( fWorld );
		fWorld = NULL;

		// These need to outlive fWorld
		Rtt_DELETE( fWorldDestructionListener );
		fWorldDestructionListener = NULL;

		Rtt_DELETE( fWorldContactListener );
		fWorldContactListener = NULL;

		Rtt_DELETE( fWorldDebugDraw );
		fWorldDebugDraw = NULL;
	}
}

void
PhysicsWorld::SetProperty( Properties mask, bool value )
{
	const Properties p = fProperties;
	fProperties = ( value ? p | mask : p & ~mask );
}

void
PhysicsWorld::SetTimeStep( float newValue )
{
	fIsTimeStepDefault = ( newValue < 0.f );
	ResetRealTime();

	if ( newValue > 0.f )
	{
		fTimeStep = newValue;
	}
	else if ( newValue < 0.f )
	{
		fTimeStep = fFrameInterval;
	}
	else
	{
		fTimeStep = Rtt_REAL_0;
		fTimePrevious = -1.f;
	}
}

void
PhysicsWorld::SetReportCollisionsInContentCoordinates( bool enabled )
{
	fReportCollisionsInContentCoordinates = enabled;
}

bool
PhysicsWorld::GetReportCollisionsInContentCoordinates() const
{
	return fReportCollisionsInContentCoordinates;
}

void
PhysicsWorld::SetLuaAssertEnabled( bool enabled )
{
	fLuaAssertEnabled = enabled;
}

bool
PhysicsWorld::GetLuaAssertEnabled() const
{
	return fLuaAssertEnabled;
}

void
PhysicsWorld::SetAverageCollisionPositions( bool enabled )
{
	fAverageCollisionPositions = enabled;
}

bool
PhysicsWorld::GetAverageCollisionPositions() const
{
	return fAverageCollisionPositions;
}

void
PhysicsWorld::DebugDraw( Renderer &renderer ) const
{
	if( ! fWorld )
	{
		// Nothing to do.
		return;
	}

	fWorldDebugDraw->DrawDebugData( * this, renderer );
//	fWorldDebugDraw->Begin( *this,
//							renderer );
//	{
//		LuaLibPhysics::DebugDraw( fWorld,
//									fWorldDebugDraw,
//									GetMetersPerPixel() );
//	}
//	fWorldDebugDraw->End();
//
}

void
PhysicsWorld::ResetRealTime()
{
	fRealTimePrevious = -1.;
	fRealTimeRemainder = 0.;
}

S32
PhysicsWorld::CountRealTimeSteps( double elapsedMS )
{
	const double dt = fFrameInterval;
	const double tPrevious = fRealTimePrevious;
	fRealTimePrevious = elapsedMS * 0.001;

	if ( tPrevious < 0. )
	{
		return 1;
	}

	const double elapsed = fRealTimePrevious - tPrevious;
	const double accumulated = fRealTimeRemainder + elapsed;
	const double roundedSteps = floor( accumulated / dt + 0.5 );
	if ( roundedSteps > kMaxRealTimeSteps )
	{
		fRealTimeRemainder = 0.;
		return kMaxRealTimeSteps;
	}

	const S32 numSteps = Max( (S32)roundedSteps, (S32)1 );
	const double remainder = Max( accumulated - numSteps * dt, -dt );

	// Without this leak, a display just off 120 Hz takes a visible double step each time the remainder fills up
	const double leak = kRealTimeLeak * elapsed;
	fRealTimeRemainder = ( remainder > 0. ? Max( remainder - leak, 0. ) : Min( remainder + leak, 0. ) );

	return numSteps;
}

void
PhysicsWorld::StepWorld( double elapsedMS )
{
	if ( fWorld && IsProperty( kIsWorldRunning ) )
	{
		// These values may be changed on the fly. TODO: make sure this isn't occurring real overhead, or we should drop back to default values only!
		S32 velocityIterations = GetVelocityIterations();
		S32 positionIterations = GetPositionIterations();

		b2World& world = * fWorld;

		float dt = GetTimeStep();
		if ( fFollowsRealTime && fIsTimeStepDefault )
		{
			const S32 numSteps = CountRealTimeSteps( elapsedMS );

			// Box2D would clear forces after the first step, so a force applied once per frame would act on only one of them
			const bool autoClearForces = world.GetAutoClearForces();
			world.SetAutoClearForces( false );
			for ( S32 i = 0; i < numSteps && IsProperty( kIsWorldRunning ); i++ )
			{
				world.Step( fFrameInterval * fTimeScale, velocityIterations, positionIterations );
			}
			if ( autoClearForces )
			{
				world.ClearForces();
			}
			world.SetAutoClearForces( autoClearForces );
		}
		else if ( dt > Rtt_REAL_0 )
		{
			// Simulation timesteps are driven by the render frame rate
			world.Step( dt * fTimeScale, velocityIterations, positionIterations );
		}
		else
		{
			dt = fFrameInterval;

			// Simulation timesteps match actual time with an error <= dt
			// For more info: http://gafferongames.com/game-physics/fix-your-timestep/
			// NOTE: times are in seconds, not milliseconds
			float tCurrent = elapsedMS * 0.001f;
			float tPrevious = ( fTimePrevious > 0.f
				? fTimePrevious
				: ( tCurrent - dt ) );
			
			 // time elapsed between current and previous frame plus the remainder from the previous step
			float tStep = ( tCurrent - tPrevious ) + fTimeRemainder;

			while ( tStep >= dt )
			{
				world.Step( dt * fTimeScale, velocityIterations, positionIterations );
				tStep -= dt;
			}

			fTimePrevious = tCurrent;
			fTimeRemainder = tStep;
		}

		Real scale = GetPixelsPerMeter();

		const void *groundBodyUserdata = LuaLibPhysics::GetGroundBodyUserdata();

		// Iterate over bodies, and update sprites (display objects)
		for ( b2Body *body = world.GetBodyList(), *nextBody = NULL;
			  NULL != body;
			  body = nextBody )
		{
			// Prefetch next body in case we delete body
			nextBody = body->GetNext();

			if ( body->GetUserData() )
			{
				if ( body->GetUserData() != groundBodyUserdata )
				{
					DisplayObject *o = (DisplayObject*)body->GetUserData();
					if ( ! o->IsOrphan() )
					{
						// While updating DisplayObject transform based on Box2d body,
						// inhibit updates to corresponding Box2d body.
						o->SetExtensionsLocked( true );

						b2Vec2 position = body->GetPosition(); 
						Rtt_ASSERT(position.IsValid());
						position *= scale;
						
						Real angle = Rtt_RealRadiansToDegrees( Rtt_FloatToReal( body->GetAngle() ) );
						o->SetGeometricProperty( kOriginX, position.x );
						o->SetGeometricProperty( kOriginY, position.y );
						o->SetGeometricProperty( kRotation, angle );

						o->SetExtensionsLocked( false );
					}
				}
			}
			else 
			{
				// We assume that any body with no UserData should be destroyed here, since the UserData initially stores the corresponding 
				// Corona display object on body construction, and is then set to NULL when the corresponding display object has been deleted.
				world.DestroyBody( body );
			}
		}
		
		void *finalizedUserdata = UserdataWrapper::GetFinalizedValue();
		// Iterate over joints, and remove any that the user has deleted
		for ( b2Joint *joint = world.GetJointList(), *nextJoint = NULL;
			  NULL != joint;
			  joint = nextJoint )
		{
			// Prefetch next joint in case we delete joint
			nextJoint = joint->GetNext();

			if ( finalizedUserdata == joint->GetUserData() )
			{
				// We assume that any joint with no UserData should be destroyed here, since the UserData initially stores the corresponding 
				// UserdataWrapper on joint construction, and is then set to NULL when the user calls joint:removeSelf().
				world.DestroyJoint( joint );
			}
		}
	}
}

// ----------------------------------------------------------------------------

} // namespace Rtt

// ----------------------------------------------------------------------------

