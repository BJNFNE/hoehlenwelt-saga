// Maps DOS file names used by the game onto host files.
//
// Read-only game files come from the data directory; files the game writes
// (HW.CFG, Save.NN) live in the save directory. Lookups are case-insensitive
// and ignore drive letters and directories, the game only uses flat names.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class Vfs {
public:
	void setDirs(const std::string &dataDir, const std::string &saveDir);

	FILE *openHost(const std::string &dosName, bool write, bool create, std::string &hostPath);
	bool readWhole(const std::string &dosName, std::vector<uint8_t> &out);
	bool exists(const std::string &dosName);
	bool remove(const std::string &dosName);
	bool rename(const std::string &from, const std::string &to);
	int64_t fileSize(const std::string &dosName);
	std::vector<std::string> list(const std::string &pattern);
	void truncate(const std::string &hostPath, long size);

	const std::string &dataDir() const { return data_; }
	const std::string &saveDir() const { return save_; }

	static std::string baseName(const std::string &dosName);
	// find a file in dir, case-insensitively; returns full path or ""
	static std::string findInDir(const std::string &dir, const std::string &name);

private:
	std::string data_, save_;
};
