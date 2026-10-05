#include "Link.hpp"

#include <cybercraft_protocol.h>

#include <Windows.h>

#include <atomic>
#include <cstring>

namespace cybercraft
{
	namespace
	{
		template <class T>
		std::atomic_ref<T> Atomic(T& a_value)
		{
			return std::atomic_ref<T>(a_value);
		}
	}

	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	bool Link::Create()
	{
		if (base_) {
			return true;
		}

		const std::uint64_t size = proto::kMappingBytes;
		mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
			static_cast<DWORD>(size & 0xFFFFFFFF), proto::kMappingName);
		if (!mapping_) {
			return false;
		}

		void* view = ::MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (!view) {
			::CloseHandle(static_cast<HANDLE>(mapping_));
			mapping_ = nullptr;
			return false;
		}

		base_ = static_cast<std::uint8_t*>(view);
		std::memset(base_, 0, static_cast<std::size_t>(size));

		// Every ground slot starts out as "no data": a cell that can't exist, and no ground.
		{
			float noGround = proto::kNoGround;
			std::uint32_t bits;
			std::memcpy(&bits, &noGround, sizeof(bits));
			const std::uint64_t empty = std::uint64_t(bits) | (std::uint64_t(0x8000) << 32) | (std::uint64_t(0x8000) << 48);
			auto* slots = reinterpret_cast<std::uint64_t*>(base_ + proto::kOffGround);
			for (std::uint32_t i = 0; i < proto::kGroundN * proto::kGroundN * 2; ++i) {
				slots[i] = empty;
			}
		}

		auto* header = reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		header->version = proto::kVersion;
		header->gamePid = ::GetCurrentProcessId();
		header->gameHeartbeatMs = ::GetTickCount64();
		// The magic goes in last: Minecraft treats the mapping as ready only once it sees it.
		Atomic(header->magic).store(proto::kMagic, std::memory_order_release);
		return true;
	}

	void Link::Close()
	{
		if (base_) {
			::UnmapViewOfFile(base_);
			base_ = nullptr;
		}
		if (mapping_) {
			::CloseHandle(static_cast<HANDLE>(mapping_));
			mapping_ = nullptr;
		}
	}

	void Link::Beat()
	{
		if (!base_) {
			return;
		}
		auto* header = reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		Atomic(header->gameHeartbeatMs).store(::GetTickCount64(), std::memory_order_release);
	}

	void Link::PublishPlayer(bool a_inGame, double a_x, double a_y, double a_z)
	{
		if (!base_) {
			return;
		}
		auto* state = reinterpret_cast<proto::GameState*>(base_ + proto::kOffGameState);

		auto       seq = Atomic(state->seq);
		const auto start = seq.load(std::memory_order_relaxed);
		seq.store(start + 1, std::memory_order_relaxed);  // odd: write in progress
		std::atomic_thread_fence(std::memory_order_release);

		state->flags = a_inGame ? proto::kGameInGame : 0;
		state->posX = a_x;
		state->posY = a_y;
		state->posZ = a_z;
		state->frameCounter = ++frame_;
		state->cmdAck = cmdAck_;
		state->cmdResult = cmdResult_;

		seq.store(start + 2, std::memory_order_release);  // even: done
	}

	bool Link::ReadMcState(McSnapshot& a_out) const
	{
		if (!base_) {
			return false;
		}
		auto* state = reinterpret_cast<proto::McState*>(base_ + proto::kOffMcState);

		for (int attempt = 0; attempt < 8; ++attempt) {
			const auto seq1 = Atomic(state->seq).load(std::memory_order_acquire);
			if ((seq1 & 1) != 0) {
				continue;  // Minecraft is writing it right now
			}

			const McSnapshot copy{ state->flags, state->targetX, state->targetY, state->targetZ, state->yaw, state->pitch,
				state->frameCounter };
			std::atomic_thread_fence(std::memory_order_acquire);
			if (Atomic(state->seq).load(std::memory_order_relaxed) == seq1) {
				a_out = copy;
				return true;
			}
		}
		return false;
	}

	bool Link::PollCommand(Command& a_out)
	{
		if (!base_) {
			return false;
		}
		auto* cmd = reinterpret_cast<proto::McCommand*>(base_ + proto::kOffMcCommand);

		for (int attempt = 0; attempt < 8; ++attempt) {
			const auto seq1 = Atomic(cmd->seq).load(std::memory_order_acquire);
			if ((seq1 & 1) != 0) {
				continue;  // Minecraft is writing it right now
			}
			if (seq1 == 0 || seq1 == lastSeenCmdSeq_) {
				return false;  // nothing new
			}

			const Command copy{ seq1, cmd->kind, cmd->x, cmd->y, cmd->z };
			std::atomic_thread_fence(std::memory_order_acquire);
			if (Atomic(cmd->seq).load(std::memory_order_relaxed) == seq1) {
				a_out = copy;
				lastSeenCmdSeq_ = seq1;
				return true;
			}
		}
		return false;
	}

	void Link::AckCommand(std::uint32_t a_seq, bool a_ok)
	{
		cmdAck_ = a_seq;
		cmdResult_ = a_ok ? proto::kResultOk : proto::kResultFailed;
	}

	std::uint32_t Link::McPid() const
	{
		if (!base_) {
			return 0;
		}
		auto* header = reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		return Atomic(header->mcPid).load(std::memory_order_acquire);
	}

	std::uint64_t Link::McHeartbeatMs() const
	{
		if (!base_) {
			return 0;
		}
		auto* header = reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		return Atomic(header->mcHeartbeatMs).load(std::memory_order_acquire);
	}
}
