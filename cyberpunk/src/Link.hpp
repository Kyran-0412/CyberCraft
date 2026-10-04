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

		// Call every frame: proves to Minecraft that this game is alive.
		void Beat();

		// Publish V's position (Minecraft coordinates) with a seqlock so Minecraft never reads a torn value.
		void PublishPlayer(bool inGame, double mcX, double mcY, double mcZ);

		std::uint32_t McPid() const;
		std::uint64_t McHeartbeatMs() const;

	private:
		Link() = default;

		void*         mapping_{ nullptr };  // HANDLE
		std::uint8_t* base_{ nullptr };
		std::uint64_t frame_{ 0 };
	};
}
