// Phase 3a: play Minecraft with the keyboard and mouse of the Cyberpunk window.
//
// While V follows Minecraft's player ("routing"), the game window's message handler is replaced by one that:
//   * turns keys, mouse buttons, the wheel and typed characters into events for Minecraft (the input ring in
//     shared memory), and keeps them away from the game, so V doesn't also run, shoot or open menus;
//   * turns mouse movement into a look direction, or, while a Minecraft screen (inventory, chat) is open, into
//     a cursor that is drawn over the game.
// A few things stay the game's: Esc (the pause menu, unless a Minecraft screen is open to close), and Alt+F4.
// F9 switches routing off and on by hand. Routing also pauses by itself while the game is paused (its menus).
//
// Both the old-style window messages and Windows "raw input" are understood, because the game may use either.
// Each key or button is only sent when its state changes, so having both costs nothing.

#include "Input.hpp"
#include "Log.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace cybercraft::input
{
	namespace
	{
		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		constexpr int kToggleKey = VK_F9;
		constexpr float kDegreesPerCount = 0.15f;  // Minecraft: 0.15 degrees per mouse count, times the sensitivity curve

		// DirectInput scan code (the hardware scan code, plus 0x80 for the extended keys) -> SDL scancode, which is
		// what Minecraft uses. The same table SkyCraft uses (MIT, by chasmlol).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		constexpr unsigned kDikEscape = 0x01;
		constexpr unsigned kDikF11 = 0x57;

		struct State
		{
			HWND window = nullptr;
			WNDPROC oldProc = nullptr;
			bool installed = false;

			// Decided on the main thread each frame, read by the window procedure.
			std::atomic<bool> routing{ false };
			std::atomic<bool> screenOpen{ false };
			bool userEnabled = true;  // F9
			bool wasRouting = false;
			bool escPaused = false;  // fallback if the game can't tell us it is paused

			// Mouse movement collected by the window procedure, taken by Update().
			std::atomic<int> pendingDx{ 0 };
			std::atomic<int> pendingDy{ 0 };

			// Key and button states as last sent to Minecraft (or, when not routing, as the game last saw them).
			std::array<bool, 512> sent{};      // by DIK code (0..255) and, +256, extended; see Index()
			std::array<bool, 512> gameHeld{};  // keys the game has seen go down and not yet up
			std::array<bool, 6> button{};      // by SDL button number
			bool sawRawWheel = false;
			std::uint32_t highSurrogate = 0;

			// What the main thread works out.
			float yaw = 0.0f;
			float pitch = 0.0f;
			float cursorX = 0.0f;
			float cursorY = 0.0f;
			bool cursorShown = false;

			// Counters for the log.
			std::uint64_t counts[8] = {};  // 0 keys, 1 buttons, 2 wheel, 3 raw mouse, 4 raw keyboard, 5 text, 6 swallowed, 7 passed
			std::chrono::steady_clock::time_point lastReport{};
		} g;

		// The index of a DIK code in the state arrays.
		std::size_t Index(unsigned a_dik)
		{
			return a_dik & 0xFF;
		}

		unsigned DikFromMessage(LPARAM a_lParam)
		{
			const unsigned scan = (a_lParam >> 16) & 0xFF;
			const unsigned extended = (a_lParam >> 24) & 1;
			return scan | (extended ? 0x80u : 0u);
		}

		// ---- the game's own idea of "paused" -------------------------------------------------------------
		RED4ext::CClassFunction* g_isPaused = nullptr;
		bool g_lookedUpPaused = false;
		bool g_pausedKnown = false;

		bool GamePaused()
		{
			if (!g_lookedUpPaused) {
				g_lookedUpPaused = true;
				auto rtti = RED4ext::CRTTISystem::Get();
				if (auto cls = rtti->GetClass("gameTimeSystem")) {
					g_isPaused = cls->GetFunction("IsPausedState");
				}
				g_pausedKnown = g_isPaused != nullptr;
				g_sdk->logger->Info(g_handle, g_pausedKnown ? "input: the game can tell us when it is paused (its menus)"
				                                              : "input: no IsPausedState; Esc will switch routing off and on instead");
			}
			if (!g_pausedKnown) {
				return false;
			}
			auto engine = RED4ext::CGameEngine::Get();
			if (!engine || !engine->framework || !engine->framework->gameInstance) {
				return false;
			}
			auto cls = RED4ext::CRTTISystem::Get()->GetClass("gameTimeSystem");
			auto* system = cls ? engine->framework->gameInstance->GetSystem(cls) : nullptr;
			if (!system) {
				return false;
			}
			bool paused = false;
			RED4ext::ExecuteFunction(system, g_isPaused, &paused);
			return paused;
		}

		// ---- sending to Minecraft --------------------------------------------------------------------------
		void SendKey(unsigned a_dik, bool a_down)
		{
			const auto idx = Index(a_dik);
			if (g.sent[idx] == a_down) {
				return;  // no change
			}
			g.sent[idx] = a_down;
			const std::uint16_t sdl = kDikToSdl[a_dik & 0xFF];
			if (sdl == 0) {
				return;
			}
			Link::Get().PushInput(proto::kInKey, sdl, a_down ? 1 : 0);
			++g.counts[0];
		}

		void SendButton(int a_sdlButton, bool a_down)
		{
			if (a_sdlButton < 1 || a_sdlButton > 5 || g.button[a_sdlButton] == a_down) {
				return;
			}
			g.button[a_sdlButton] = a_down;
			Link::Get().PushInput(proto::kInMouseButton, static_cast<std::uint16_t>(a_sdlButton), a_down ? 1 : 0);
			++g.counts[1];
		}

		void SendWheel(int a_delta)
		{
			Link::Get().PushInput(proto::kInScroll, 0, a_delta);
			++g.counts[2];
		}

		void ReleaseEverythingInMinecraft()
		{
			g.sent.fill(false);
			g.button.fill(false);
			Link::Get().PushInput(proto::kInReleaseAll, 0);
		}

		// The game goes on seeing keys while we aren't routing: remember which are held, so that when routing
		// starts they can be let go of in the game (it would never see the key-up otherwise).
		void TrackGameKey(unsigned a_dik, bool a_down)
		{
			g.gameHeld[Index(a_dik)] = a_down;
		}

		void LetGoOfGameKeys()
		{
			for (unsigned dik = 0; dik < 256; ++dik) {
				if (!g.gameHeld[dik]) {
					continue;
				}
				g.gameHeld[dik] = false;
				const unsigned scan = dik & 0x7F;
				const bool extended = (dik & 0x80) != 0;
				const UINT vk = ::MapVirtualKeyW(scan | (extended ? 0xE000u : 0u), MAPVK_VSC_TO_VK_EX);
				const LPARAM lParam = 1 | (LPARAM(scan) << 16) | (LPARAM(extended ? 1 : 0) << 24) | (1 << 30) | (1u << 31);
				if (vk) {
					g.oldProc(g.window, WM_KEYUP, vk, lParam);
				}
			}
		}

		// ---- the window procedure ---------------------------------------------------------------------------
		enum class Verdict
		{
			Pass,     // the game gets it
			Swallow,  // Minecraft gets it (or nobody does); the game doesn't
		};

		Verdict OnKey(unsigned a_dik, UINT a_vk, bool a_down, bool a_isSysKey)
		{
			if (a_vk == VK_ESCAPE || a_dik == kDikEscape) {
				if (g.screenOpen.load()) {
					SendKey(a_dik, a_down);  // closes the Minecraft screen
					return Verdict::Swallow;
				}
				// The game's pause menu. If the game can't tell us it is paused, Esc is how we find out.
				if (a_down && !g_pausedKnown) {
					g.escPaused = !g.escPaused;
				}
				TrackGameKey(a_dik, a_down);
				return Verdict::Pass;
			}
			if (a_isSysKey && a_vk == VK_F4) {
				return Verdict::Pass;  // Alt+F4 still closes the game
			}
			if (a_dik == kDikF11) {
				return Verdict::Swallow;  // Minecraft's fullscreen toggle: not wanted on the hidden window
			}
			SendKey(a_dik, a_down);
			return Verdict::Swallow;
		}

		void OnMouseButtonMessage(UINT a_msg, WPARAM a_wParam)
		{
			switch (a_msg) {
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
				SendButton(1, true);
				break;
			case WM_LBUTTONUP:
				SendButton(1, false);
				break;
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
				SendButton(2, true);
				break;
			case WM_MBUTTONUP:
				SendButton(2, false);
				break;
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
				SendButton(3, true);
				break;
			case WM_RBUTTONUP:
				SendButton(3, false);
				break;
			case WM_XBUTTONDOWN:
			case WM_XBUTTONDBLCLK:
				SendButton(HIWORD(a_wParam) == XBUTTON1 ? 4 : 5, true);
				break;
			case WM_XBUTTONUP:
				SendButton(HIWORD(a_wParam) == XBUTTON1 ? 4 : 5, false);
				break;
			default:
				break;
			}
		}

		// Reads one WM_INPUT. Returns nothing; feeds the mouse movement, buttons, wheel and keys.
		void OnRawInput(LPARAM a_lParam, bool a_routing)
		{
			alignas(8) std::uint8_t buffer[256];
			UINT size = sizeof(buffer);
			const UINT got = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
			if (got == static_cast<UINT>(-1) || got == 0) {
				return;
			}
			const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
			if (raw->header.dwType == RIM_TYPEMOUSE) {
				const RAWMOUSE& m = raw->data.mouse;
				if (!a_routing) {
					return;
				}
				++g.counts[3];
				if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
					g.pendingDx += m.lLastX;
					g.pendingDy += m.lLastY;
				}
				const USHORT f = m.usButtonFlags;
				if (f & RI_MOUSE_LEFT_BUTTON_DOWN) SendButton(1, true);
				if (f & RI_MOUSE_LEFT_BUTTON_UP) SendButton(1, false);
				if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) SendButton(2, true);
				if (f & RI_MOUSE_MIDDLE_BUTTON_UP) SendButton(2, false);
				if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) SendButton(3, true);
				if (f & RI_MOUSE_RIGHT_BUTTON_UP) SendButton(3, false);
				if (f & RI_MOUSE_BUTTON_4_DOWN) SendButton(4, true);
				if (f & RI_MOUSE_BUTTON_4_UP) SendButton(4, false);
				if (f & RI_MOUSE_BUTTON_5_DOWN) SendButton(5, true);
				if (f & RI_MOUSE_BUTTON_5_UP) SendButton(5, false);
				if (f & RI_MOUSE_WHEEL) {
					g.sawRawWheel = true;
					SendWheel(static_cast<SHORT>(m.usButtonData));
				}
			} else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
				const RAWKEYBOARD& k = raw->data.keyboard;
				const bool down = (k.Flags & RI_KEY_BREAK) == 0;
				const unsigned dik = (k.MakeCode & 0x7F) | ((k.Flags & RI_KEY_E0) ? 0x80u : 0u);
				if (k.MakeCode == 0 || k.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) {
					return;  // fake keys Windows sends along with Pause, Print Screen and the like
				}
				if (a_routing) {
					++g.counts[4];
					if (k.VKey == kToggleKey) {
						return;
					}
					OnKey(dik, k.VKey, down, false);
				} else {
					TrackGameKey(dik, down);
				}
			}
		}

		LRESULT CALLBACK HookedProc(HWND a_window, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			// F9: switch routing off and on, whatever state we are in.
			if ((a_msg == WM_KEYDOWN || a_msg == WM_KEYUP) && a_wParam == kToggleKey) {
				if (a_msg == WM_KEYDOWN && (a_lParam & (1 << 30)) == 0) {
					g.userEnabled = !g.userEnabled;
					g_sdk->logger->Info(g_handle, g.userEnabled ? "input: F9 pressed, Minecraft controls are on (when V follows)"
					                                            : "input: F9 pressed, Minecraft controls are off");
				}
				return 0;
			}

			const bool routing = g.routing.load();

			if (a_msg == WM_INPUT) {
				OnRawInput(a_lParam, routing);
				if (routing) {
					++g.counts[6];
					return ::DefWindowProcW(a_window, a_msg, a_wParam, a_lParam);  // Windows needs this to free the raw input
				}
				return g.oldProc(a_window, a_msg, a_wParam, a_lParam);
			}

			if (!routing) {
				if (a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN) {
					TrackGameKey(DikFromMessage(a_lParam), true);
				} else if (a_msg == WM_KEYUP || a_msg == WM_SYSKEYUP) {
					TrackGameKey(DikFromMessage(a_lParam), false);
				}
				return g.oldProc(a_window, a_msg, a_wParam, a_lParam);
			}

			switch (a_msg) {
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				{
					const bool down = a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN;
					const bool repeat = down && (a_lParam & (1 << 30)) != 0;
					const bool sys = a_msg == WM_SYSKEYDOWN || a_msg == WM_SYSKEYUP;
					if (repeat && static_cast<UINT>(a_wParam) != VK_ESCAPE) {
						++g.counts[6];
						return 0;
					}
					if (OnKey(DikFromMessage(a_lParam), static_cast<UINT>(a_wParam), down, sys) == Verdict::Pass) {
						++g.counts[7];
						return g.oldProc(a_window, a_msg, a_wParam, a_lParam);
					}
					++g.counts[6];
					return 0;
				}
			case WM_CHAR:
				{
					if (g.screenOpen.load()) {
						std::uint32_t code = static_cast<std::uint32_t>(a_wParam);
						if (code >= 0xD800 && code <= 0xDBFF) {
							g.highSurrogate = code;
						} else {
							if (code >= 0xDC00 && code <= 0xDFFF && g.highSurrogate) {
								code = 0x10000 + ((g.highSurrogate - 0xD800) << 10) + (code - 0xDC00);
							}
							g.highSurrogate = 0;
							if (code >= 32 && code != 127) {
								Link::Get().PushInput(proto::kInText, 0, static_cast<std::int32_t>(code));
								++g.counts[5];
							}
						}
					}
					++g.counts[6];
					return 0;
				}
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_RBUTTONDBLCLK:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_XBUTTONDBLCLK:
				OnMouseButtonMessage(a_msg, a_wParam);
				++g.counts[6];
				return a_msg == WM_XBUTTONDOWN || a_msg == WM_XBUTTONUP || a_msg == WM_XBUTTONDBLCLK ? TRUE : 0;
			case WM_MOUSEWHEEL:
				if (!g.sawRawWheel) {
					SendWheel(GET_WHEEL_DELTA_WPARAM(a_wParam));
				}
				++g.counts[6];
				return 0;
			case WM_MOUSEMOVE:
				++g.counts[6];
				return 0;
			default:
				break;
			}
			return g.oldProc(a_window, a_msg, a_wParam, a_lParam);
		}

		void LogStatus()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - g.lastReport < std::chrono::seconds(5)) {
				return;
			}
			g.lastReport = now;
			if (!log::Verbose()) {
				std::fill(std::begin(g.counts), std::end(g.counts), 0ull);
				return;
			}
			if (!g.routing.load() && g.counts[0] + g.counts[1] + g.counts[3] + g.counts[4] == 0) {
				return;
			}
			g_sdk->logger->InfoF(g_handle,
				"input: routing=%d screen open=%d; in the last 5 s: %llu keys, %llu buttons, %llu wheel, %llu raw mouse, %llu raw keyboard, %llu typed characters sent to Minecraft; %llu messages kept from the game, %llu let through",
				g.routing.load() ? 1 : 0, g.screenOpen.load() ? 1 : 0, static_cast<unsigned long long>(g.counts[0]), static_cast<unsigned long long>(g.counts[1]),
				static_cast<unsigned long long>(g.counts[2]), static_cast<unsigned long long>(g.counts[3]), static_cast<unsigned long long>(g.counts[4]),
				static_cast<unsigned long long>(g.counts[5]), static_cast<unsigned long long>(g.counts[6]), static_cast<unsigned long long>(g.counts[7]));
			std::fill(std::begin(g.counts), std::end(g.counts), 0ull);
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void Install(HWND a_window)
	{
		static bool tried = false;
		if (g.installed || tried || !a_window) {
			return;
		}
		tried = true;
		::SetLastError(0);
		const auto old = ::SetWindowLongPtrW(a_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedProc));
		if (!old) {
			g_sdk->logger->ErrorF(g_handle, "input: could not take over the game window's messages (Windows error %lu); keyboard and mouse stay with the game", ::GetLastError());
			return;
		}
		g.window = a_window;
		g.oldProc = reinterpret_cast<WNDPROC>(old);
		g.installed = true;
		g_sdk->logger->InfoF(g_handle, "input: hooked the game window %p", static_cast<void*>(a_window));
	}

	void Uninstall()
	{
		if (!g.installed) {
			return;
		}
		g.routing = false;
		// Only put the old handler back if nobody has replaced ours since.
		if (reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g.window, GWLP_WNDPROC)) == &HookedProc) {
			::SetWindowLongPtrW(g.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.oldProc));
			g.installed = false;
		}
	}

	void Update(bool a_followOn, bool a_playerHere, const Link::McSnapshot& a_mc)
	{
		if (!g.installed) {
			return;
		}

		const bool screen = (a_mc.flags & proto::kMcScreenOpen) != 0;
		const bool paused = GamePaused();
		const bool want = a_followOn && a_playerHere && g.userEnabled && !paused && !g.escPaused;

		// The Esc fallback: once the game is no longer showing a menu the next Esc isn't ours to guess about; F9 resets it.
		if (!a_followOn) {
			g.escPaused = false;
		}

		if (want && !g.wasRouting) {
			// Starting: take the look direction from Minecraft's player, and let go of keys the game thinks are held.
			g.yaw = a_mc.yaw;
			g.pitch = a_mc.pitch;
			g.pendingDx = 0;
			g.pendingDy = 0;
			g.sent.fill(false);
			g.button.fill(false);
			LetGoOfGameKeys();
			g_sdk->logger->Info(g_handle, "input: routing on, the keyboard and mouse now go to Minecraft (Esc: game menu, F9: off)");
		} else if (!want && g.wasRouting) {
			ReleaseEverythingInMinecraft();
			g.cursorShown = false;
			g_sdk->logger->InfoF(g_handle, "input: routing off (%s)", !a_followOn ? "V isn't following" : paused ? "the game is paused" : !g.userEnabled ? "F9" : "Esc");
		}
		g.wasRouting = want;
		g.routing = want;
		g.screenOpen = want && screen;
		Link::Get().SetRouting(want);

		if (!want) {
			LogStatus();
			return;
		}

		// Mouse movement since last frame.
		const int dx = g.pendingDx.exchange(0);
		const int dy = g.pendingDy.exchange(0);

		if (screen) {
			// A screen is open: a cursor, in the HUD's pixels.
			std::uint32_t hudW = 0;
			std::uint32_t hudH = 0;
			Link::Get().OverlaySize(hudW, hudH);
			if (hudW == 0 || hudH == 0) {
				hudW = 1280;
				hudH = 720;
			}
			if (!g.cursorShown) {
				g.cursorShown = true;
				g.cursorX = hudW * 0.5f;
				g.cursorY = hudH * 0.5f;
				Link::Get().PushInput(proto::kInCursor, 0, static_cast<std::int32_t>(g.cursorX), static_cast<std::int32_t>(g.cursorY));
			}
			if (dx != 0 || dy != 0) {
				// A mouse count moves the cursor about as far, on the screen, as the HUD is bigger than it.
				const float scale = 1.0f;
				g.cursorX = std::clamp(g.cursorX + dx * scale, 0.0f, float(hudW - 1));
				g.cursorY = std::clamp(g.cursorY + dy * scale, 0.0f, float(hudH - 1));
				Link::Get().PushInput(proto::kInCursor, 0, static_cast<std::int32_t>(g.cursorX), static_cast<std::int32_t>(g.cursorY));
			}
		} else {
			g.cursorShown = false;
			// Looking: Minecraft's own sensitivity curve, so it feels like Minecraft.
			const float s = a_mc.sensitivity * 0.6f + 0.2f;
			const float degrees = s * s * s * 8.0f * kDegreesPerCount;
			g.yaw += dx * degrees;
			g.pitch = std::clamp(g.pitch + dy * degrees, -90.0f, 90.0f);
			while (g.yaw > 180.0f) g.yaw -= 360.0f;
			while (g.yaw < -180.0f) g.yaw += 360.0f;
		}
		Link::Get().SetLook(g.yaw, g.pitch);
		LogStatus();
	}

	bool Routing()
	{
		return g.routing.load();
	}

	float LookYaw()
	{
		return g.yaw;
	}

	float LookPitch()
	{
		return g.pitch;
	}

	bool CursorVisible()
	{
		return g.routing.load() && g.screenOpen.load() && g.cursorShown;
	}

	void CursorPosition(float& a_x, float& a_y)
	{
		a_x = g.cursorX;
		a_y = g.cursorY;
	}
}
