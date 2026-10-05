#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace cybercraft::proto
{
	struct OverlaySlotHdr;
}

namespace cybercraft
{
	// The Cyberpunk end of the shared-memory link. We create the mapping; the Minecraft mod opens it.
	class Link
	{
	public:
		// A published camera, as Minecraft coordinates and degrees.
		struct CameraPose
		{
			double x = 0, y = 0, z = 0;
			float yaw = 0, pitch = 0, roll = 0, vfov = 60.0f, aspect = 1.778f;
		};

		static Link& Get();

		bool Create();  // idempotent
		void Close();
		bool IsOpen() const { return base_ != nullptr; }
		bool InGame() const { return inGame_; }  // a save is loaded and V exists

		// Start of the shared memory (for the ground grid, which Ground.cpp writes directly).
		std::uint8_t* Base() const { return base_; }

		// Call every frame: proves to Minecraft that this game is alive.
		void Beat();

		// Publish V's position (Minecraft coordinates) with a seqlock so Minecraft never reads a torn value.
		void PublishPlayer(bool inGame, double mcX, double mcY, double mcZ);

		// What goes out with the next PublishPlayer: where the player is looking (while routing), and whether input is routed.
		void SetLook(float a_yaw, float a_pitch) { lookYaw_ = a_yaw; lookPitch_ = a_pitch; }
		void SetRouting(bool a_routing) { routing_ = a_routing; }

		// Publishes the game's camera for Minecraft to draw through (Minecraft coordinates and degrees).
		void PublishCamera(bool a_valid, double a_x, double a_y, double a_z, float a_yaw, float a_pitch, float a_vfov, float a_aspect,
			float a_velX = 0.0f, float a_velY = 0.0f, float a_velZ = 0.0f, float a_yawRate = 0.0f, float a_pitchRate = 0.0f, float a_roll = 0.0f);

		// The camera with this frame counter (the one a Minecraft picture was drawn through), if it is still in the history.
		bool CameraPoseForFrame(std::uint64_t a_cameraFrame, CameraPose& a_out) const;
		// The camera as it was at a moment (interpolated between the two published around it; the newest if the moment is later).
		bool CameraPoseAt(std::chrono::steady_clock::time_point a_time, CameraPose& a_out) const;

		// How long ago the camera with this frame counter was published (milliseconds), or a negative number if too long ago.
		double CameraAgeMs(std::uint64_t a_cameraFrame) const;

		// Queues one keyboard/mouse event for Minecraft (see InputEvent in the protocol).
		void PushInput(std::uint16_t a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);

		// What Minecraft currently wants (see McState in the protocol).
		struct McSnapshot
		{
			std::uint32_t flags;
			double        x, y, z;  // where V should be, protocol coordinates (Minecraft axes)
			float         yaw, pitch;  // Minecraft degrees
			std::uint64_t frame;
			float         sensitivity;
			float         warpDelayMs;
		};

		// True when a consistent copy was read.
		bool ReadMcState(McSnapshot& a_out) const;

		// A one-off command from Minecraft (see McCommand in the protocol).
		struct Command
		{
			std::uint32_t seq;
			std::uint32_t kind;
			double        x, y, z;  // Minecraft coordinates
		};

		// True (and fills a_out) when Minecraft has sent a command we haven't looked at yet.
		bool PollCommand(Command& a_out);

		// Tell Minecraft what happened to that command; sent with the next PublishPlayer.
		void AckCommand(std::uint32_t a_seq, bool a_ok);

		// The newest overlay frame (Minecraft's HUD). AcquireOverlayFrame() makes it the front frame, if there is a
		// newer one than the front frame already; the accessors then read the front frame. Render thread only.
		bool AcquireOverlayFrame();
		const proto::OverlaySlotHdr* OverlayFrontHeader() const;
		const std::uint8_t* OverlayFrontPixels() const;
		std::uint64_t OverlayFramesPublished() const;
		// The size, in pixels, of the HUD frame currently in front (0 if there has been none).
		void OverlaySize(std::uint32_t& a_w, std::uint32_t& a_h) const;

		std::uint32_t McPid() const;
		std::uint64_t McHeartbeatMs() const;

	private:
		Link() = default;

		void*         mapping_{ nullptr };  // HANDLE
		std::uint8_t* base_{ nullptr };
		std::uint64_t frame_{ 0 };
		std::uint32_t lastSeenCmdSeq_{ 0 };
		std::uint32_t cmdAck_{ 0 };
		std::uint32_t cmdResult_{ 0 };
		std::uint32_t overlayFront_{ 2 };
		bool inGame_{ false };
		std::atomic<std::uint64_t> cameraFrame_{ 0 };
		// The last 1024 cameras published, with when: to find the one a Minecraft picture was drawn through, and the one the
		// game's own picture was drawn through. Written by the game's thread, read by the render thread: each entry has its own seqlock.
		struct HistoryEntry
		{
			std::atomic<std::uint32_t> seq{ 0 };
			std::uint64_t frame = 0;
			std::chrono::steady_clock::time_point time{};
			CameraPose pose{};
		};
		std::array<HistoryEntry, 1024> cameraHistory_{};
		bool routing_{ false };
		float lookYaw_{ 0.0f };
		float lookPitch_{ 0.0f };
	};
}
