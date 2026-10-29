// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace RE
{
	class Actor;
}

// A camera the plugin's window drives (see FreeCamera.cpp): the engine's free camera,
// as the tfc console command gives it, with the engine's own input off. It
// starts where the camera is, looking the same way, and moves only as the
// window reports mouse and key input over the game (ViewInput) or as
// CentreSideways puts it.
//
// Contract: every function runs on the game thread.
namespace FreeCamera
{
	// One frame of the window's mouse and key input over the game.
	struct ViewInput
	{
		float lookX = 0.0f;        // pixels of a left drag, right
		float lookY = 0.0f;        // pixels of a left drag, down
		float panX = 0.0f;         // pixels of a right drag, right
		float panY = 0.0f;         // pixels of a right drag, down
		float moveForward = 0.0f;  // -1, 0 or 1 from S / W
		float moveRight = 0.0f;    // -1, 0 or 1 from A / D
		float zoomSteps = 0.0f;    // wheel steps, positive towards the view
		bool  slow = false;        // Shift held
		float seconds = 0.0f;      // the frame's length
		float moveUp = 0.0f;       // -1, 0 or 1, world up. Left at 0, a caller is unchanged.
	};

	// Bounds for one Move, measured from where Begin left the camera. A zero length is no
	// limit. The defaults keep a caller that omits them: a left drag turns around the actor,
	// and sideways movement stays.
	struct MoveLimits
	{
		float maxDistance = 0.0f;   // horizontal distance from Begin
		float maxUp = 0.0f;         // above Begin
		float maxDown = 0.0f;       // below Begin
		bool  turnInPlace = false;  // a left drag turns the view and leaves the camera where it is
		bool  allowLateral = true;  // moveRight and a right drag
		bool  levelZoom = false;    // the wheel moves on the horizontal facing, not along the look
	};

	// Enters the free camera when the camera is in ordinary first or third
	// person; otherwise changes nothing and returns false. Previewing the player
	// from first person switches to third person first: first person does not
	// draw their body. With `startFromFirstPerson` the camera instead switches to
	// first person and starts at the player's eyes, looking where they look,
	// whoever `actor` is. A left drag turns around `actor` unless Move is asked to turn in place.
	bool Begin(RE::Actor& actor, bool startFromFirstPerson = false);

	// Applies one frame of input; `speedScale` multiplies the move, pan and zoom rates
	// (1 is normal; a left drag turns at the game's own rate). Does nothing without Begin.
	void Move(const ViewInput& input, float speedScale, MoveLimits limits = {});

	// Moves the camera sideways, along its right, until `actor` stands in the middle of the
	// view from left to right; the heading, the tilt, the height and the distance stay, so an
	// over-the-shoulder offset is all that goes. Call right after Begin; does nothing without it.
	void CentreSideways(RE::Actor& actor);

	// Puts back everything Begin changed, the first or third person view and the
	// third person camera's turn around the player included; does nothing when
	// Begin did not take the camera.
	void End();
}
