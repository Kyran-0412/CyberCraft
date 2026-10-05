#pragma once

#include <cstdint>

namespace cybercraft
{
	// The Cyberpunk end of the shared-memory link. We create the mapping; the Minecraft mod opens it.
	class Link
	{
	public:
		static Link& Get();

		bool Create();  // idempotent
		void Close();
		bool IsOpen() const { return base_ != nullptr; }

		// Start of the shared memory (for the ground grid, which Ground.cpp writes directly).
		std::uint8_t* Base() const { return base_; }

		// Call every frame: proves to Minecraft that this game is alive.
		void Beat();

		// Publish V's position (Minecraft coordinates) with a seqlock so Minecraft never reads a torn value.
		void PublishPlayer(bool inGame, double mcX, double mcY, double mcZ);

		// What Minecraft currently wants (see McState in the protocol).
		struct McSnapshot
		{
			std::uint32_t flags;
			double        x, y, z;  // where V should be, protocol coordinates (Minecraft axes)
			float         yaw, pitch;  // Minecraft degrees
			std::uint64_t frame;
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
	};
}
