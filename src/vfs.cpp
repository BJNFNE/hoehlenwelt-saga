#include "fileutil.h"
#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

static std::string upper(std::string s) {
	for (auto &c : s)
		c = (char)toupper((unsigned char)c);
	return s;
}

void Vfs::setDirs(const std::string &dataDir, const std::string &saveDir) {
	data_ = dataDir;
	save_ = saveDir;
	std::error_code ec;
	fs::create_directories(fsu::path(save_), ec);
}

std::string Vfs::baseName(const std::string &dosName) {
	size_t p = dosName.find_last_of("\\/:");
	std::string b = p == std::string::npos ? dosName : dosName.substr(p + 1);
	while (!b.empty() && (b.back() == ' ' || b.back() == '.'))
		b.pop_back();
	return b;
}

std::string Vfs::findInDir(const std::string &dir, const std::string &name) {
	if (dir.empty() || name.empty())
		return "";
	std::error_code ec;
	std::string want = upper(name);
	for (auto &e : fs::directory_iterator(fsu::path(dir), ec)) {
		if (!e.is_regular_file(ec))
			continue;
		if (upper(e.path().filename().u8string()) == want)
			return e.path().u8string();
	}
	return "";
}

FILE *Vfs::openHost(const std::string &dosName, bool write, bool create, std::string &hostPath) {
	std::string b = baseName(dosName);
	if (b.empty())
		return nullptr;
	std::string inSave = findInDir(save_, b);
	if (create) {
		hostPath = inSave.empty() ? (fsu::path(save_) / upper(b)).u8string() : inSave;
		return fsu::open(hostPath, "w+b");
	}
	if (!inSave.empty()) {
		hostPath = inSave;
		return fsu::open(hostPath, write ? "r+b" : "rb");
	}
	std::string inData = findInDir(data_, b);
	if (inData.empty())
		return nullptr;
	if (!write) {
		hostPath = inData;
		return fsu::open(hostPath, "rb");
	}
	// copy on write into the save directory
	hostPath = (fsu::path(save_) / upper(b)).u8string();
	std::error_code ec;
	fs::copy_file(fsu::path(inData), fsu::path(hostPath), fs::copy_options::overwrite_existing, ec);
	if (ec)
		return nullptr;
	return fsu::open(hostPath, "r+b");
}

bool Vfs::readWhole(const std::string &dosName, std::vector<uint8_t> &out) {
	std::string path;
	FILE *f = openHost(dosName, false, false, path);
	if (!f)
		return false;
	fsu::seek(f, 0, SEEK_END);
	long n = fsu::tell(f);
	fsu::seek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
	fclose(f);
	return got == out.size();
}

bool Vfs::exists(const std::string &dosName) {
	std::string b = baseName(dosName);
	return !findInDir(save_, b).empty() || !findInDir(data_, b).empty();
}

bool Vfs::remove(const std::string &dosName) {
	std::string p = findInDir(save_, baseName(dosName));
	if (p.empty())
		return false;
	std::error_code ec;
	return fs::remove(fsu::path(p), ec);
}

bool Vfs::rename(const std::string &from, const std::string &to) {
	std::string p = findInDir(save_, baseName(from));
	if (p.empty())
		return false;
	std::error_code ec;
	fs::rename(fsu::path(p), fsu::path(save_) / upper(baseName(to)), ec);
	return !ec;
}

int64_t Vfs::fileSize(const std::string &dosName) {
	std::string b = baseName(dosName);
	std::string p = findInDir(save_, b);
	if (p.empty())
		p = findInDir(data_, b);
	if (p.empty())
		return 0;
	std::error_code ec;
	auto s = fs::file_size(fsu::path(p), ec);
	return ec ? 0 : (int64_t)s;
}

static bool wildMatch(const std::string &pat, const std::string &s) {
	// DOS-style: '*' matches up to the dot, '?' one character; compare
	// name and extension separately
	auto split = [](const std::string &x, std::string &n, std::string &e) {
		size_t d = x.find('.');
		n = d == std::string::npos ? x : x.substr(0, d);
		e = d == std::string::npos ? "" : x.substr(d + 1);
	};
	auto part = [](const std::string &p, const std::string &v) {
		size_t i = 0;
		for (; i < p.size(); i++) {
			if (p[i] == '*')
				return true;
			if (i >= v.size())
				return false;
			if (p[i] != '?' && p[i] != v[i])
				return false;
		}
		return i == v.size();
	};
	std::string pn, pe, sn, se;
	split(pat, pn, pe);
	split(s, sn, se);
	if (pat.find('.') == std::string::npos)
		pe = "*";
	return part(pn, sn) && part(pe, se);
}

std::vector<std::string> Vfs::list(const std::string &pattern) {
	std::string pat = upper(baseName(pattern));
	std::vector<std::string> out;
	for (const std::string &dir : {save_, data_}) {
		std::error_code ec;
		if (dir.empty())
			continue;
		for (auto &e : fs::directory_iterator(fsu::path(dir), ec)) {
			if (!e.is_regular_file(ec))
				continue;
			std::string n = upper(e.path().filename().u8string());
			if (wildMatch(pat, n) && std::find(out.begin(), out.end(), n) == out.end())
				out.push_back(n);
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

void Vfs::truncate(const std::string &hostPath, long size) {
	std::error_code ec;
	fs::resize_file(fsu::path(hostPath), (uintmax_t)size, ec);
}
