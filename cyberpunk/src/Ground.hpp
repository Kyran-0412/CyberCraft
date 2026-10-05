#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::ground
{
	// Called once from Main with the RED4ext handles, so this file can write to the log.
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Call every frame while V exists. Shoots a few rays down around V and updates the shared ground grid.
	// (vx, vy, vz) is V's position in Cyberpunk coordinates (X east, Y north, Z up, metres).
	void Update(double a_vx, double a_vy, double a_vz);

	// How far the game's static world is along a ray (origin and direction in game coordinates, metres): the nearest hit over the
	// collision groups the scan uses, or NaN if nothing is hit within a_maxDistance (or the scan isn't ready yet).
	double RayDistance(double a_ox, double a_oy, double a_oz, double a_dx, double a_dy, double a_dz, double a_maxDistance);

	// The ground height (Cyberpunk's Z) the scan has found for the cell at this game position, or NaN if it hasn't looked there.
	double HeightAt(double a_x, double a_y);

	// The vertical offset has changed: move every stored height so the grid agrees with the new mapping at once.
	void OffsetChanged(double a_oldOffset, double a_newOffset);

	// Call when the world goes away (loading screen, main menu): forget everything cached from the old world.
	void Reset();
}
