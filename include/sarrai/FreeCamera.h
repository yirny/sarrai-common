// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace RE
{
	class Actor;
}

// A camera the plugin's window drives (see FreeCamera.cpp): the engine's free camera,
// as the tfc console command gives it, with the engine's own input off. It
// starts where the camera is, looking the same way, and moves only as the
// window reports mouse and key input over the game (ViewInput).
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
	};

	// Enters the free camera when the camera is in ordinary first or third
	// person; otherwise changes nothing and returns false. Previewing the player
	// from first person switches to third person first: first person does not
	// draw their body. With `startFromFirstPerson` the camera instead switches to
	// first person and starts at the player's eyes, looking where they look,
	// whoever `actor` is. A left drag turns around `actor` either way.
	bool Begin(RE::Actor& actor, bool startFromFirstPerson = false);

	// Applies one frame of input; `speedScale` multiplies the move, pan and zoom rates
	// (1 is normal; a left drag turns at the game's own rate). Does nothing without Begin.
	void Move(const ViewInput& input, float speedScale);

	// Puts back everything Begin changed, the first or third person view and the
	// third person camera's turn around the player included; does nothing when
	// Begin did not take the camera.
	void End();
}
