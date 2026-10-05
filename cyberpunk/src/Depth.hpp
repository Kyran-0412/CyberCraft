#pragma once

#include <RED4ext/RED4ext.hpp>

#include <d3d12.h>

namespace cybercraft::depth
{
	// A place on the screen where the game's own rays say how far the world is. u, v: across and down the screen, 0 to 1.
	// z: how far along the view direction the world is there, metres (NaN if the ray hit nothing).
	struct Reference
	{
		float u = 0.5f;
		float v = 0.5f;
		float z = 0.0f;
		bool valid = false;
	};
	constexpr int kReferenceCount = 5;

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Turns the depth tools on or off (/ccdepthprobe, /ccdepthcapture).
	//  probe:   watch the game's command lists for resource state changes into "depth write" and log which textures are used as
	//           depth buffers, how big they are and what happens to them. Changes nothing in the picture.
	//  capture: as well, copy the game's main depth texture (see Depth.cpp) and compare a few of its texels with the references.
	void SetModes(bool a_probe, bool a_capture);

	// Is the capture on, so that the camera code should measure references now and then?
	bool CaptureWanted();

	// The distances the game's own rays found at the places on the screen the capture reads (see Reference).
	void SetReferences(const Reference* a_references, int a_count);

	// The capture's copy of the game's depth texture, for the overlay to read when it draws. The texture is kept in the copy-destination
	// state: whoever uses it must move it to a shader-readable state first and put it back after (the hook records its next copy
	// assuming that). ready: the capture is running and has recorded at least one copy. srvFormat: how to view the depth plane.
	struct GameDepth
	{
		ID3D12Resource* resource = nullptr;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
		bool ready = false;
	};
	bool GetGameDepth(GameDepth& a_out);

	// Call once per presented frame: sets up the capture when it can, and logs every few seconds while a tool is on.
	void Report();

	// Puts everything back (game exit).
	void Shutdown();
}
