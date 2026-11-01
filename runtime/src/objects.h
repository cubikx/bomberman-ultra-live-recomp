// Kernel object table (semaphores, event queues, ...) keyed by guest-visible ids.
#pragma once

#include <map>
#include <memory>
#include <mutex>

#include "runtime/kernel.h"

struct KernelObject {
	virtual ~KernelObject() = default;
};

class ObjectTable {
public:
	template <typename T>
	u32 add(std::shared_ptr<T> obj) {
		std::lock_guard lock(m_lock);
		u32 id = m_next++;
		m_objs[id] = std::move(obj);
		return id;
	}
	template <typename T>
	std::shared_ptr<T> get(u32 id) {
		std::lock_guard lock(m_lock);
		auto it = m_objs.find(id);
		return it == m_objs.end() ? nullptr : std::dynamic_pointer_cast<T>(it->second);
	}
	bool remove(u32 id) {
		std::lock_guard lock(m_lock);
		return m_objs.erase(id) != 0;
	}

private:
	std::mutex m_lock;
	std::map<u32, std::shared_ptr<KernelObject>> m_objs;
	u32 m_next = 0x40000100;
};

ObjectTable& kobjects();
