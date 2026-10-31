// SPDX-License-Identifier: GPL-3.0-or-later
//
// FILE MAP
//   Constants  - speeds and sensitivities
//   State      - what Begin changed, kept for End
//   Helpers    - free camera fields, the third person state, the toggle that
//                keeps the control switches, the first / third person switch
//                and its way back, the start at the player's eyes, the limit clamps,
//                a frame of the sideways glide and of the wheel's ease
//   API        - Begin / Move / CentreSideways / Gliding / End
//
// Entering and leaving the free camera follows Tailor 3.0's own outfit preview
// (src/preview/TailorPreviewSession.cpp and PreviewSessionPolicy.h in its
// repository); the movement is the plugin's own, driven by the window.
#include "PCH.h"

#include "sarrai/FreeCamera.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace
{
	// --- Constants ------------------------------------------------------------

	// The move, pan and zoom rates are at 100% camera speed; Move's speedScale scales them.
	// A left drag turns at the game's own mouse look rate instead (ReadLookRadiansPerPixel).
	constexpr float kMoveUnitsPerSecond = 225.0f;   // with W / A / S / D
	constexpr float kSlowFactor = 0.5f;             // on the move speed while Shift is held, as in tfc
	constexpr float kLookRadiansPerPixel = 0.004f;  // of a left drag, only when the game's settings cannot be read
	constexpr float kPanUnitsPerPixel = 0.5f;       // of a right drag
	constexpr float kZoomUnitsPerStep = 40.0f;      // forward per wheel step
	constexpr float kZoomEaseSeconds = 0.06f;       // of an eased wheel step, the time for the rest of it to fall to 37%
	constexpr float kZoomDoneUnits = 0.05f;         // what is left of an eased wheel step when it counts as finished
	constexpr float kMaxPitch = 89.0f * std::numbers::pi_v<float> / 180.0f;

	// --- State ----------------------------------------------------------------

	struct FreeCameraFields
	{
		RE::NiPoint3         translation;
		RE::BSTPoint2<float> rotation;
		RE::BSTPoint2<float> zUpDown;
		std::int16_t         verticalDirection = 0;
		bool                 lockToZPlane = false;
		bool                 inputEnabled = false;
	};

	// Everything Begin changed, so End can put it back.
	struct Session
	{
		bool                                    active = false;
		RE::BSTSmartPointer<RE::TESCameraState> previousState;
		FreeCameraFields                        freeFields;
		bool                                    forcedThirdPerson = false;                   // the player was in first person
		bool                                    forcedFirstPerson = false;                   // the camera was in third person, and Begin started from the eyes
		bool                                    restoreOrbit = false;                        // the game was in third person, so End puts `orbit` back
		RE::NiPoint2                            orbit;                                       // ThirdPersonState::freeRotation as Begin found it
		RE::ActorHandle                         actor;                                       // the previewed actor, which a left drag turns around
		float                                   pitch = 0.0f;                                // radians in -kMaxPitch..kMaxPitch, positive looks down
		bool                                    invertY = false;                             // the game's Invert Y Axis option, for a left drag up and down
		float                                   lookRadiansPerPixel = kLookRadiansPerPixel;  // of a left drag, from the game's look sensitivity
		RE::NiPoint3                            anchor;                                      // translation after Begin; Move's limits are measured from here
		float                                   glideTotal = 0.0f;                           // units CentreSideways moves the camera along its right
		float                                   glideDone = 0.0f;                            // of glideTotal, moved so far
		float                                   glideSeconds = 0.0f;                         // the glide's length; 0 for none under way
		float                                   glideElapsed = 0.0f;                         // seconds of it gone
		bool                                    glideTimed = false;                          // its first frame has passed, so frame lengths count
		RE::NiPoint3                            actorStart;                                  // where the actor stood at Begin; Move's actor limits are measured from here
		float                                   actorHeight = 0.0f;                          // units, at Begin
		float                                   startDistance = 0.0f;                        // units from the camera to actorStart on the camera's facing, at Begin
		float                                   startSide = 0.0f;                            // units from actorStart sideways, at Begin, always positive
		float                                   startRise = 0.0f;                            // units above actorStart, at Begin
		float                                   zoomLeft = 0.0f;                             // units of eased wheel steps still to move, positive towards the view
	};

	// Turns the camera's place around the actor by the angle the view just
	// turned, so the actor keeps its place on screen and seems to turn on the
	// spot (as in Tailor's preview). The actor's current position is the pivot,
	// so it follows an NPC who walks; without the actor the view turns in place.
	void OrbitActor(RE::FreeCameraState& free, const RE::ActorHandle& actor, float radians)
	{
		const auto target = actor.get();
		if (!target || radians == 0.0f) {
			return;
		}
		const auto  pivot = target->GetPosition();
		const float dx = free.translation.x - pivot.x;
		const float dy = free.translation.y - pivot.y;
		const float sin = std::sin(radians);
		const float cos = std::cos(radians);
		// The same rotation the heading applies to its forward vector (sin yaw, cos yaw).
		free.translation.x = pivot.x + dx * cos + dy * sin;
		free.translation.y = pivot.y - dx * sin + dy * cos;
	}

	Session g_session;

	// Contract: written on the game thread only; Gliding reads it from any thread.
	std::atomic<bool> g_gliding{ false };

	// --- Helpers --------------------------------------------------------------

	RE::FreeCameraState* FreeState(RE::PlayerCamera& camera)
	{
		return static_cast<RE::FreeCameraState*>(camera.GetRuntimeData().cameraStates[RE::CameraState::kFree].get());
	}

	RE::ThirdPersonState* ThirdState(RE::PlayerCamera& camera)
	{
		return static_cast<RE::ThirdPersonState*>(camera.GetRuntimeData().cameraStates[RE::CameraState::kThirdPerson].get());
	}

	float Degrees(float radians)
	{
		return radians * 180.0f / std::numbers::pi_v<float>;
	}

	FreeCameraFields ReadFields(RE::FreeCameraState& state)
	{
		return { state.translation, state.rotation, state.zUpDown, state.verticalDirection, state.lockToZPlane,
			state.IsInputEventHandlingEnabled() };
	}

	void WriteFields(RE::FreeCameraState& state, const FreeCameraFields& fields)
	{
		state.translation = fields.translation;
		state.rotation = fields.rotation;
		state.zUpDown = fields.zUpDown;
		state.verticalDirection = fields.verticalDirection;
		state.lockToZPlane = fields.lockToZPlane;
		state.SetInputEventHandlingEnabled(fields.inputEnabled);
	}

	// The game's Invert Y Axis option (SkyrimPrefs.ini). Read once per preview:
	// GetSetting scans the whole collection, and the window holds the game's
	// menus, so the option cannot change while the preview is on.
	bool ReadInvertY()
	{
		auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
		auto* setting = prefs ? prefs->GetSetting("bInvertYValues:Controls") : nullptr;
		if (!setting) {
			logger::warn("Free camera: the game's Invert Y setting was not found; treated as off");
			return false;
		}
		return setting->GetBool();
	}

	// The game's mouse look rate: pi * fMouseHeadingSensitivity (SkyrimPrefs.ini,
	// the Look Sensitivity slider) * fMouseHeadingXScale (Skyrim.ini) radians per
	// pixel, as measured in the game's own heading turn; the same rate is used up
	// and down, since the game's pitch rate depends on the frame rate. Read once
	// per preview, like ReadInvertY.
	float ReadLookRadiansPerPixel()
	{
		auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
		auto* ini = RE::INISettingCollection::GetSingleton();
		auto* sensitivity = prefs ? prefs->GetSetting("fMouseHeadingSensitivity:Controls") : nullptr;
		auto* xScale = ini ? ini->GetSetting("fMouseHeadingXScale:Controls") : nullptr;
		if (!sensitivity || !xScale) {
			logger::warn("Free camera: the game's look sensitivity was not found; using the built-in rate");
			return kLookRadiansPerPixel;
		}
		return std::numbers::pi_v<float> * sensitivity->GetFloat() * xScale->GetFloat();
	}

	// The free camera saves the control switches as it starts, turns every one
	// on, and puts its copy back as it ends; keeping the switches as they are
	// now leaves one another mod holds off still off afterwards (as Tailor does).
	void ToggleFreeCamera(RE::PlayerCamera& camera)
	{
		auto*         controls = RE::ControlMap::GetSingleton();
		std::uint32_t enabled = 0;
		std::uint32_t stored = 0;
		if (controls) {
			controls->GetControlsState(enabled, stored);
		}
		camera.ToggleFreeCameraMode(false);  // false: time keeps running
		if (controls) {
			controls->SetControlsState(enabled, stored);
		}
	}

	// Switches to first person (`first`) or third person unless the camera is
	// there already; `switched` then tells End to switch back. False, with the
	// camera put back, when the switch does not take.
	bool SwitchPerson(RE::PlayerCamera& camera, bool first, bool& switched)
	{
		if (first ? camera.IsInFirstPerson() : camera.IsInThirdPerson()) {
			return true;
		}
		const bool done = first ? camera.ForceFirstPerson() && camera.IsInFirstPerson() : camera.ForceThirdPerson() && camera.IsInThirdPerson();
		if (!done) {
			logger::warn("Free camera: could not switch to {} person", first ? "first" : "third");
			if (first && !camera.IsInThirdPerson()) {
				camera.ForceThirdPerson();
			} else if (!first && !camera.IsInFirstPerson()) {
				camera.ForceFirstPerson();
			}
			return false;
		}
		switched = true;
		return true;
	}

	// Undoes SwitchPerson and puts back the turn around the player.
	// Contract: after the free camera has ended. The way back to third person
	// leaves the turn at zero (seen in game), so it is written once third
	// person is current.
	void SwitchBack(RE::PlayerCamera& camera, const Session& session)
	{
		if (session.forcedFirstPerson) {
			camera.ForceThirdPerson();
		}
		auto* third = session.restoreOrbit && camera.IsInThirdPerson() ? ThirdState(camera) : nullptr;
		if (third) {
			logger::info("Free camera off: the turn around the player put back (the game had moved it by heading {:+.1f} deg, pitch {:+.1f} deg)",
				Degrees(third->freeRotation.x - session.orbit.x), Degrees(third->freeRotation.y - session.orbit.y));
			third->freeRotation = session.orbit;
		}
		if (session.forcedThirdPerson) {
			camera.ForceFirstPerson();
		}
	}

	// Puts the free camera at the player's eyes, looking where they look. Set
	// here rather than left to the free camera's own start, which may not have
	// caught up with a switch to first person made in the same frame.
	void StartAtEyes(RE::FreeCameraState& free)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		RE::NiPoint3 origin;
		RE::NiPoint3 direction;
		player->GetEyeVector(origin, direction, false);
		free.translation = origin;
		if (direction.Unitize() > 0.0f) {
			// The inverse of Move's forward vector: (sin yaw cos pitch, cos yaw cos pitch, -sin pitch).
			free.rotation.y = std::atan2(direction.x, direction.y);
			free.rotation.x = std::clamp(std::asin(std::clamp(-direction.z, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
		}
	}

	// Pulls a place that has left the limits back onto them. A zero limit is not applied.
	void ClampToAnchor(RE::NiPoint3& place, const RE::NiPoint3& anchor, FreeCamera::MoveLimits limits)
	{
		if (limits.maxDistance > 0.0f) {
			const float dx = place.x - anchor.x;
			const float dy = place.y - anchor.y;
			const float dist2 = dx * dx + dy * dy;
			const float max2 = limits.maxDistance * limits.maxDistance;
			if (dist2 > max2 && dist2 > 0.0f) {
				const float scale = limits.maxDistance / std::sqrt(dist2);
				place.x = anchor.x + dx * scale;
				place.y = anchor.y + dy * scale;
			}
		}
		if (limits.maxDown > 0.0f) {
			place.z = (std::max)(place.z, anchor.z - limits.maxDown);
		}
		if (limits.maxUp > 0.0f) {
			place.z = (std::min)(place.z, anchor.z + limits.maxUp);
		}
	}

	// Pulls a place that has left the bounds around the actor back onto them, each axis on its
	// own. Each bound stretches to Begin's own place, so a far start never jumps at the first
	// input; a place inside them is not touched. True when the distance bound stopped it.
	bool ClampToActor(RE::NiPoint3& place, const RE::NiPoint3& right, const RE::NiPoint3& level, const FreeCamera::MoveLimits& limits)
	{
		if (limits.maxActorDistance <= 0.0f) {
			return false;
		}
		const auto  offset = place - g_session.actorStart;
		const float distance = -offset.Dot(level);
		const float keptDistance = std::clamp(distance, (std::min)(limits.minActorDistance, g_session.startDistance),
			(std::max)(limits.maxActorDistance, g_session.startDistance));
		const float side = offset.Dot(right);
		const float maxSide = (std::max)(limits.sideSlope * keptDistance, g_session.startSide);
		const float maxRise = (std::max)(limits.upperHeight * g_session.actorHeight + limits.riseSlope * keptDistance, g_session.startRise);
		const float minRise = (std::min)(limits.lowerHeight * g_session.actorHeight - limits.riseSlope * keptDistance, g_session.startRise);
		place += level * (distance - keptDistance);
		place += right * (std::clamp(side, -maxSide, maxSide) - side);
		place.z += std::clamp(offset.z, minRise, maxRise) - offset.z;
		return keptDistance != distance;
	}

	// Adds this frame's wheel steps and returns the units to move along the view now. With a
	// zoomShare the steps ease out: each frame takes a part of what is left, so a long frame
	// finishes them and never overshoots, and steps in quick succession run together.
	float StepZoom(const FreeCamera::ViewInput& input, float scale, const RE::NiPoint3& place, const RE::NiPoint3& level, const FreeCamera::MoveLimits& limits)
	{
		if (limits.zoomShare <= 0.0f) {
			return input.zoomSteps * kZoomUnitsPerStep * scale;
		}
		if (input.zoomSteps != 0.0f) {
			const float distance = (std::max)(-(place - g_session.actorStart).Dot(level), limits.minActorDistance);
			g_session.zoomLeft += input.zoomSteps * limits.zoomShare * distance * scale;
		}
		float part = g_session.zoomLeft * (1.0f - std::exp(-input.seconds / kZoomEaseSeconds));
		g_session.zoomLeft -= part;
		if (std::abs(g_session.zoomLeft) < kZoomDoneUnits) {
			part += g_session.zoomLeft;
			g_session.zoomLeft = 0.0f;
		}
		return part;
	}

	// Moves the camera by the part of the glide that falls in this frame, fast at first and
	// slowing to a stop (an ease-out cubic). A long frame finishes it and never overshoots.
	// The first frame counts as no time: no ImGui frame runs while the window is closed, so
	// the first one after it opens is as long as the window was closed.
	void StepGlide(RE::FreeCameraState& free, const RE::NiPoint3& right, float seconds)
	{
		if (g_session.glideSeconds <= 0.0f) {
			return;
		}
		g_session.glideElapsed += g_session.glideTimed ? seconds : 0.0f;
		g_session.glideTimed = true;
		const float t = (std::min)(g_session.glideElapsed / g_session.glideSeconds, 1.0f);
		const float left = 1.0f - t;
		const float done = g_session.glideTotal * (1.0f - left * left * left);
		const auto  step = right * (done - g_session.glideDone);
		g_session.glideDone = done;
		free.translation += step;
		g_session.anchor += step;
		if (t >= 1.0f) {
			g_session.glideSeconds = 0.0f;
		}
	}
}

namespace FreeCamera
{
	// --- API ------------------------------------------------------------------

	bool Begin(RE::Actor& actor, bool startFromFirstPerson)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* free = camera ? FreeState(*camera) : nullptr;
		if (g_session.active || !free || !camera->currentState || (!camera->IsInFirstPerson() && !camera->IsInThirdPerson())) {
			return false;
		}
		Session session;
		// The way back to third person sets the camera's turn around the player to
		// zero, so End puts back the one it has now.
		if (auto* third = ThirdState(*camera); third && camera->IsInThirdPerson()) {
			session.orbit = third->freeRotation;
			session.restoreOrbit = true;
		}
		const bool switched = startFromFirstPerson ? SwitchPerson(*camera, true, session.forcedFirstPerson) :
		                                             !actor.IsPlayerRef() || SwitchPerson(*camera, false, session.forcedThirdPerson);
		if (!switched) {
			SwitchBack(*camera, session);  // only the turn around the player: SwitchPerson put the view back
			return false;
		}

		// The free camera starts where the camera is, looking the same way; its fields are kept only to put them back.
		session.previousState = camera->currentState;
		session.freeFields = ReadFields(*free);
		ToggleFreeCamera(*camera);
		if (camera->currentState.get() != free) {
			logger::warn("Free camera: the free camera did not start");
			WriteFields(*free, session.freeFields);
			if (camera->currentState.get() != session.previousState.get()) {
				camera->SetState(session.previousState.get());
			}
			SwitchBack(*camera, session);
			return false;
		}
		if (startFromFirstPerson) {
			StartAtEyes(*free);
		}
		free->SetInputEventHandlingEnabled(false);  // the window drives it (Move)
		free->zUpDown = {};
		free->verticalDirection = 0;
		free->lockToZPlane = false;

		session.actor = actor.GetHandle();
		// Brought into -pi..pi once: the engine may keep the angle as 0..2pi.
		session.pitch = std::clamp(std::remainder(free->rotation.x, 2.0f * std::numbers::pi_v<float>), -kMaxPitch, kMaxPitch);
		session.invertY = ReadInvertY();
		session.lookRadiansPerPixel = ReadLookRadiansPerPixel();
		session.anchor = free->translation;
		session.actorStart = actor.GetPosition();
		session.actorHeight = actor.GetHeight();
		const auto         startOffset = free->translation - session.actorStart;
		const RE::NiPoint3 startLevel{ std::sin(free->rotation.y), std::cos(free->rotation.y), 0.0f };
		const RE::NiPoint3 startRight{ std::cos(free->rotation.y), -std::sin(free->rotation.y), 0.0f };
		session.startDistance = -startOffset.Dot(startLevel);
		session.startSide = std::abs(startOffset.Dot(startRight));
		session.startRise = startOffset.z;
		session.active = true;
		g_session = std::move(session);
		logger::info("Free camera on {:08X}{} (invert Y: {}, look {:.6f} rad/px, {:.1f} units away, {:.1f} sideways, {:+.1f} up, actor {:.1f} tall)", actor.GetFormID(),
			startFromFirstPerson ? ", from the eyes" : "", g_session.invertY ? "on" : "off", g_session.lookRadiansPerPixel, g_session.startDistance, g_session.startSide,
			g_session.startRise, g_session.actorHeight);
		return true;
	}

	// rotation.x is the pitch (positive looks down), rotation.y the heading the
	// camera faces, in Skyrim's convention: heading 0 faces +Y, and turns clockwise
	// seen from above. A left drag sideways turns the view and, unless asked to turn
	// in place, the camera's place around the actor together; up and down tilts in
	// place, as the game's mouse look does (Invert Y Axis included). The engine reads
	// the fields every frame, so nothing else needs an update.
	void Move(const ViewInput& input, float speedScale, MoveLimits limits)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* free = camera ? FreeState(*camera) : nullptr;
		if (!g_session.active || !free || camera->currentState.get() != free) {
			g_gliding.store(false);  // nothing left to glide, so the window stops sending empty frames
			return;
		}

		const float scale = speedScale;
		const float look = g_session.lookRadiansPerPixel;  // the game's own rate: the camera speed setting does not apply
		const float turn = input.lookX * look;
		free->rotation.y += turn;
		if (!limits.turnInPlace) {
			OrbitActor(*free, g_session.actor, turn);
		}
		// The pitch is the session's own, never read back: the engine may store a
		// look above the horizon as an angle near 2pi, which a clamp would turn
		// into looking straight down.
		const float lookY = g_session.invertY ? -input.lookY : input.lookY;
		g_session.pitch = std::clamp(g_session.pitch + lookY * look, -kMaxPitch, kMaxPitch);
		free->rotation.x = g_session.pitch;
		const float        yaw = free->rotation.y;
		const float        pitch = g_session.pitch;
		const RE::NiPoint3 forward{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
		const RE::NiPoint3 level{ std::sin(yaw), std::cos(yaw), 0.0f };
		const RE::NiPoint3 right{ std::cos(yaw), -std::sin(yaw), 0.0f };
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };

		const float step = kMoveUnitsPerSecond * scale * (input.slow ? kSlowFactor : 1.0f) * input.seconds;
		auto        move = forward * (input.moveForward * step);
		move += (limits.levelZoom ? level : forward) * StepZoom(input, scale, free->translation, level, limits);
		move += up * (input.moveUp * step);
		if (limits.allowLateral) {
			move += right * (input.moveRight * step);
			// A right drag moves the camera the way the mouse goes; the scene slides the other way.
			const float pan = kPanUnitsPerPixel * scale;
			move += right * (input.panX * pan) + up * (-input.panY * pan);
		}
		free->translation += move;
		// After this frame's turn: the remaining glide is along the right the camera has now.
		StepGlide(*free, right, input.seconds);
		ClampToAnchor(free->translation, g_session.anchor, limits);
		if (ClampToActor(free->translation, right, level, limits)) {
			g_session.zoomLeft = 0.0f;  // at the end of the range, so the next step the other way starts at once
		}
		g_gliding.store(g_session.glideSeconds > 0.0f || g_session.zoomLeft != 0.0f);
	}

	// Moving along Move's right vector changes neither the actor's depth nor its height on
	// screen, and the place Move's limits are measured from goes with the camera.
	void CentreSideways(RE::Actor& actor, float seconds)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* free = camera ? FreeState(*camera) : nullptr;
		if (!g_session.active || !free || camera->currentState.get() != free) {
			return;
		}
		const float        yaw = free->rotation.y;
		const RE::NiPoint3 right{ std::cos(yaw), -std::sin(yaw), 0.0f };
		const float        side = (actor.GetPosition() - free->translation).Dot(right);
		if (seconds > 0.0f) {
			g_session.glideTotal = side;
			g_session.glideDone = 0.0f;
			g_session.glideElapsed = 0.0f;
			g_session.glideTimed = false;
			g_session.glideSeconds = seconds;
			g_gliding.store(true);
		} else {
			free->translation += right * side;
			g_session.anchor += right * side;
		}
		logger::info("Free camera moves {:+.1f} units sideways in {:.1f} s to centre {:08X}", side, seconds, actor.GetFormID());
	}

	bool Gliding()
	{
		return g_gliding.load();
	}

	void End()
	{
		if (!g_session.active) {
			return;
		}
		auto session = std::exchange(g_session, Session{});
		g_gliding.store(false);
		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* free = camera ? FreeState(*camera) : nullptr;
		if (!free || camera->currentState.get() != free) {
			logger::warn("Free camera: something else changed the camera; left as it is");
			return;
		}
		WriteFields(*free, session.freeFields);
		ToggleFreeCamera(*camera);
		if (session.previousState && camera->currentState.get() != session.previousState.get()) {
			camera->SetState(session.previousState.get());
		}
		SwitchBack(*camera, session);
	}
}
