#pragma once
/*
 * C++ 薄封装 — 底层协议与 tools/dbc_rw 一致（syscall 41 + magic）
 * 推荐新代码直接 #include "dbc_rw.h" 并链接 dbc_rw.c
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/uio.h>
#include <sys/syscall.h>
#include "syscall_arch.h"

#define DBC_GET_VERSION 0
#define DBC_READ_MEMORY 1
#define DBC_WRITE_MEMORY 2
#define DBC_MAGIC 0x1b1841fd2c1e0000ULL

#define DBC_CALL(cmd, ...) syscall(41, DBC_MAGIC | (cmd), ##__VA_ARGS__)

inline pid_t gpid = 0;

namespace Dbc
{
	inline bool init(pid_t pid)
	{
		long ver = DBC_CALL(DBC_GET_VERSION);
		if (ver <= 0) {
			std::cerr << "dbc-rw not loaded, ver=" << ver << std::endl;
			return false;
		}
		gpid = pid;
		return true;
	}

	inline bool read(uint64_t addr, void *buffer, size_t size)
	{
		return DBC_CALL(DBC_READ_MEMORY, gpid, addr, buffer, size) == (long)size;
	}

	template <typename T>
	inline T read(uint64_t addr)
	{
		T res{};
		if (Dbc::read(addr, &res, sizeof(T)))
			return res;
		return {};
	}

	inline bool write(uint64_t addr, void *buffer, size_t size)
	{
		return DBC_CALL(DBC_WRITE_MEMORY, gpid, addr, buffer, size) == (long)size;
	}

	template <typename T>
	inline bool write(uint64_t addr, T value)
	{
		return Dbc::write(addr, &value, sizeof(T));
	}

	/* 对照：正规 process_vm_*（非内核 dbc-rw） */
	inline bool uread(uint64_t addr, void *buffer, size_t size)
	{
		struct iovec l{buffer, size}, r{(void *)addr, size};
		return syscall(__NR_process_vm_readv, gpid, &l, 1, &r, 1, 0) ==
		       (long)size;
	}

	template <typename T>
	inline T uread(uint64_t addr)
	{
		T res{};
		if (Dbc::uread(addr, &res, sizeof(T)))
			return res;
		return {};
	}

	inline bool uwrite(uint64_t addr, void *buffer, size_t size)
	{
		struct iovec l{buffer, size}, r{(void *)addr, size};
		return syscall(__NR_process_vm_writev, gpid, &l, 1, &r, 1, 0) ==
		       (long)size;
	}

	template <typename T>
	inline bool uwrite(uint64_t addr, T value)
	{
		return Dbc::uwrite(addr, &value, sizeof(T));
	}

	inline int getPID(const char *PackageName)
	{
		char cmd[0x100] = "pidof ";
		strcat(cmd, PackageName);
		FILE *fp = popen(cmd, "r");
		pid_t pid = 0;
		if (fp) {
			fscanf(fp, "%d", &pid);
			pclose(fp);
		}
		if (pid > 0)
			gpid = pid;
		return pid;
	}

	inline uint64_t getModuleBase(const char *name)
	{
		char path[PATH_MAX];
		snprintf(path, PATH_MAX, "/proc/%d/maps", gpid);
		FILE *fp = fopen(path, "r");
		if (!fp)
			return 0;
		uint64_t s = 0;
		char line[1024];
		while (fgets(line, sizeof(line), fp)) {
			if (std::strstr(line, name)) {
				sscanf(line, "%lx-%*lx", &s);
				break;
			}
		}
		std::fclose(fp);
		return s;
	}
}

#define RB(addr) (Dbc::read<int8_t>(addr))
#define RW(addr) (Dbc::read<int16_t>(addr))
#define RD(addr) (Dbc::read<int32_t>(addr))
#define RQ(addr) (Dbc::read<uint64_t>(addr))
#define RF(addr) (Dbc::read<float>(addr))
#define RE(addr) (Dbc::read<double>(addr))
#define WB(addr, value) Dbc::write<int8_t>((addr), (value))
#define WW(addr, value) Dbc::write<int16_t>((addr), (value))
#define WD(addr, value) Dbc::write<int32_t>((addr), (value))
#define WQ(addr, value) Dbc::write<uint64_t>((addr), (value))
#define WF(addr, value) Dbc::write<float>((addr), (value))
#define WE(addr, value) Dbc::write<double>((addr), (value))
