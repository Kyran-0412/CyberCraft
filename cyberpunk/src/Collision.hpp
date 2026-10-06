#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::collision
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// The collision experiment: can the plugin put a physical object into Night City, so cars and people are stopped by Minecraft blocks?
	//  action 0: write to the log what the game and Codeware offer for it (classes, properties, functions)
	//  action 1: spawn one test object, a few metres in front of the camera, from the entity template named in collision-template.txt
	//            (next to the plugin), and, a moment later, write to the log what it turned out to be (class, components)
	//  action 2: remove everything this has spawned
	// a_variant (action 3 only) picks one of a few ways to fill in the collider, to find out which one the game accepts.
	void Command(int a_action, int a_variant);

	// Call every frame while V exists: finishes the inspection of a spawned test object once the game has had time to create it.
	void Update();
}
