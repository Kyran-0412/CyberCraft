#include "Link.hpp"
#include "Mapping.hpp"

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
		// The pixel slots at the end are big and the OS hands them out zeroed already: only clear the control part.
		std::memset(base_, 0, static_cast<std::size_t>(proto::kOffOverlayPixels));

		// Every ground slot starts out as "no data": a cell that can't exist, and no ground.
		{
			float noGround = proto::kNoGround;
			std::uint32_t bits;
			std::memcpy(&bits, &noGround, sizeof(bits));
			const std::uint64_t empty = std::uint64_t(bits) | (std::uint64_t(0x8000) << 32) | (std::uint64_t(0x8000) << 48);
			auto* slots = reinterpret_cast<std::uint64_t*>(base_ + proto::kOffGround);
			for (std::uint32_t i = 0; i < proto::kGroundN * proto::kGroundN * 3; ++i) {
				slots[i] = empty;
			}
		}

		reinterpret_cast<proto::OverlayCtl*>(base_ + proto::kOffOverlayCtl)->front = overlayFront_;

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

		inGame_ = a_inGame;
		state->flags = (a_inGame ? proto::kGameInGame : 0) | (routing_ ? proto::kGameRouting : 0);
		state->lookYaw = lookYaw_;
		state->lookPitch = lookPitch_;
		state->verticalOffset = static_cast<float>(mapping::Offset());
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
				state->frameCounter, state->sensitivity };
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

	void Link::PublishCamera(bool a_valid, double a_x, double a_y, double a_z, float a_yaw, float a_pitch, float a_vfov, float a_aspect,
		float a_velX, float a_velY, float a_velZ, float a_yawRate, float a_pitchRate, float a_roll)
	{
		if (!base_) {
			return;
		}
		auto* state = reinterpret_cast<proto::CameraState*>(base_ + proto::kOffCamera);
		auto  seq = Atomic(state->seq);
		const auto start = seq.load(std::memory_order_relaxed);
		seq.store(start + 1, std::memory_order_relaxed);  // odd: write in progress
		std::atomic_thread_fence(std::memory_order_release);

		state->flags = a_valid ? proto::kCameraValid : 0;
		state->posX = a_x;
		state->posY = a_y;
		state->posZ = a_z;
		state->yaw = a_yaw;
		state->pitch = a_pitch;
		state->vfov = a_vfov;
		state->aspect = a_aspect;
		state->velX = a_velX;
		state->velY = a_velY;
		state->velZ = a_velZ;
		state->yawRate = a_yawRate;
		state->pitchRate = a_pitchRate;
		state->roll = a_roll;
		++cameraFrame_;
		state->frameCounter = cameraFrame_;
		cameraTimes_[cameraFrame_ & 1023] = std::chrono::steady_clock::now();

		seq.store(start + 2, std::memory_order_release);  // even: done
	}

	double Link::CameraAgeMs(std::uint64_t a_cameraFrame) const
	{
		if (a_cameraFrame == 0 || a_cameraFrame > cameraFrame_ || cameraFrame_ - a_cameraFrame >= 1024) {
			return -1.0;
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cameraTimes_[a_cameraFrame & 1023]).count();
	}

	void Link::PushInput(std::uint16_t a_type, std::uint16_t a_code, std::int32_t a_a, std::int32_t a_b, std::int32_t a_c)
	{
		if (!base_) {
			return;
		}
		auto* ring = base_ + proto::kOffInputRing;
		auto  head = Atomic(*reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff));
		const std::uint64_t h = head.load(std::memory_order_relaxed);
		auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (h & (proto::kInputRingEntries - 1));
		entry->type = a_type;
		entry->code = a_code;
		entry->a = a_a;
		entry->b = a_b;
		entry->c = a_c;
		head.store(h + 1, std::memory_order_release);
	}

	bool Link::AcquireOverlayFrame()
	{
		if (!base_) {
			return false;
		}
		auto* ctl = reinterpret_cast<proto::OverlayCtl*>(base_ + proto::kOffOverlayCtl);
		auto  state = Atomic(ctl->state);
		if ((state.load(std::memory_order_acquire) & proto::kOverlayDirty) == 0) {
			return false;  // nothing newer than the frame we already have
		}
		const std::uint32_t old = state.exchange(overlayFront_, std::memory_order_acq_rel);
		if ((old & proto::kOverlayDirty) == 0) {
			return false;
		}
		overlayFront_ = old & 3;
		Atomic(ctl->front).store(overlayFront_, std::memory_order_release);  // so a Minecraft that starts later can work out its own slot
		return true;
	}

	const proto::OverlaySlotHdr* Link::OverlayFrontHeader() const
	{
		return reinterpret_cast<const proto::OverlaySlotHdr*>(base_ + proto::kOffOverlaySlotHdr) + overlayFront_;
	}

	const std::uint8_t* Link::OverlayFrontPixels() const
	{
		return base_ + proto::kOffOverlayPixels + std::uint64_t(overlayFront_) * proto::kOverlaySlotBytes;
	}

	void Link::OverlaySize(std::uint32_t& a_w, std::uint32_t& a_h) const
	{
		a_w = a_h = 0;
		if (!base_) {
			return;
		}
		const auto* hdr = reinterpret_cast<const proto::OverlaySlotHdr*>(base_ + proto::kOffOverlaySlotHdr) + overlayFront_;
		a_w = hdr->width;
		a_h = hdr->height;
	}

	std::uint64_t Link::OverlayFramesPublished() const
	{
		if (!base_) {
			return 0;
		}
		auto* ctl = reinterpret_cast<proto::OverlayCtl*>(base_ + proto::kOffOverlayCtl);
		return Atomic(ctl->framesPublished).load(std::memory_order_acquire);
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
