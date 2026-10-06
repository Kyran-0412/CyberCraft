#include "Mapping.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace cybercraft::mapping
{
	namespace
	{
		double g_offset = 0.0;

		// The file lives next to the plugin: <game>\red4ext\plugins\CyberCraft\vertical-offset.txt
		std::wstring FilePath()
		{
			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&FilePath), &module)) {
				return {};
			}
			wchar_t buffer[MAX_PATH * 2] = {};
			const DWORD length = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
			if (length == 0) {
				return {};
			}
			std::wstring path(buffer, length);
			const auto slash = path.find_last_of(L"\\/");
			if (slash == std::wstring::npos) {
				return {};
			}
			return path.substr(0, slash + 1) + L"vertical-offset.txt";
		}
	}

	std::filesystem::path PluginFolder()
	{
		const std::wstring file = FilePath();
		return file.empty() ? std::filesystem::path{} : std::filesystem::path(file).parent_path();
	}

	double Offset()
	{
		return g_offset;
	}

	void SetOffset(double a_offset)
	{
		g_offset = a_offset;
		const std::wstring path = FilePath();
		if (path.empty()) {
			return;
		}
		std::ofstream file{ std::filesystem::path(path) };
		if (file) {
			file << a_offset << "\n";
		}
	}

	void Load()
	{
		const std::wstring path = FilePath();
		if (path.empty()) {
			return;
		}
		std::ifstream file{ std::filesystem::path(path) };
		double value = 0.0;
		if (file && (file >> value) && value > -1.0 && value < 1.0) {
			g_offset = value;
		}
	}
}
