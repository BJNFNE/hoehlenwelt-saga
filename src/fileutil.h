// Portable file helpers. All path strings in the program are UTF-8; on
// Windows they are converted to wide strings before calling the C runtime.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fsu {

inline std::filesystem::path path(const std::string &utf8) {
	return std::filesystem::u8path(utf8);
}

inline std::string str(const std::filesystem::path &p) {
	return p.u8string();
}

inline FILE *open(const std::string &utf8, const char *mode) {
#ifdef _WIN32
	std::wstring wmode(mode, mode + strlen(mode));
	return _wfopen(path(utf8).wstring().c_str(), wmode.c_str());
#else
	return fopen(utf8.c_str(), mode);
#endif
}

inline int seek(FILE *f, int64_t off, int whence) {
#ifdef _WIN32
	return _fseeki64(f, off, whence);
#else
	return fseeko(f, (off_t)off, whence);
#endif
}

inline int64_t tell(FILE *f) {
#ifdef _WIN32
	return _ftelli64(f);
#else
	return (int64_t)ftello(f);
#endif
}

} // namespace fsu
