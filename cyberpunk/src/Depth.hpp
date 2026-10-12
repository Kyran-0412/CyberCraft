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

	// The texture the capture copies from (the game's main depth), or null if there isn't one yet.
	ID3D12Resource* CandidateResource();

	// Records into a_list a copy of the game's depth into the same texture the capture fills, for a_list to run at a moment when the depth texture is in the state a_state (all
	// subresources). The end-of-frame capture comes too late for blocks drawn into the scene (the main depth is still written to after that point), so the in-scene list makes its own
	// copy first. False if there is no capture to copy into.
	bool RecordSceneCapture(ID3D12GraphicsCommandList* a_list, std::uint32_t a_state);

	// Records into a_list copies of single texels of the game's main depth texture (both planes: the depth, and the stencil) into a_readback (a buffer in a readback heap), at the pixels
	// a_pixels[i] = { x, y }: the depth of pixel i at a_depthOffset + i * a_stride (a float), its stencil at a_stencilOffset + i * a_stride (a byte). a_state is the state the depth texture is
	// in when a_list runs. For the G-buffer reader: nothing is changed in the texture.
	// Records a transition of the game's main depth texture (all subresources) into a_list WITHOUT going through the hooked barrier entry: the capture treats the texture leaving depth write as the
	// cue to copy it, and a pass of ours that writes depth (the engine-lit blocks) must not trigger that. False if there is no such texture.
	bool RecordCandidateBarrier(ID3D12GraphicsCommandList* a_list, std::uint32_t a_before, std::uint32_t a_after);

	bool RecordTexelReadback(ID3D12GraphicsCommandList* a_list, std::uint32_t a_state, ID3D12Resource* a_readback, UINT64 a_depthOffset, UINT64 a_stencilOffset, UINT64 a_stride, const UINT (*a_pixels)[2], int a_count);

	// The game's interface layer (HUD), copied each frame right after the game has drawn it (needs the depth tools on, which install the hook), for the
	// overlay to draw back on top of Minecraft's blocks. Kept in the copy-destination state like the depth copy: move it to a readable state to use it
	// and put it back. Its format is R8G8B8A8_TYPELESS: view it as R8G8B8A8_UNORM (the layer's own sRGB encoding is kept as it is).
	struct GameUi
	{
		ID3D12Resource* resource = nullptr;
		UINT width = 0;
		UINT height = 0;
		bool ready = false;
	};
	void SetUiWanted(bool a_wanted);
	void SetScreenSize(UINT a_width, UINT a_height);  // the back buffer's size: the interface layer is the screen-sized texture
	bool GetGameUi(GameUi& a_out);                    // false if there is no fresh copy

	// Call once per presented frame: sets up the capture when it can, and logs every few seconds while a tool is on.
	void Report();

	// Puts everything back (game exit).
	void Shutdown();
}
