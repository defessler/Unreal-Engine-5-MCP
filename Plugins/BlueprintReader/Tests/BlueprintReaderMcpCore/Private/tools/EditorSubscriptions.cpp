#include "tools/EditorSubscriptions.h"

namespace bpr::tools {

std::string EditorSubscriptions::Subscribe(const std::vector<std::string>& eventTypes) {
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string id = "sub-" + std::to_string(nextId_++);
	std::unordered_set<std::string>& set = subs_[id];
	if (eventTypes.empty()) {
		set.insert(std::string{});    // "" = all events
	} else {
		for (const std::string& t : eventTypes) {
			set.insert(t);
		}
	}
	return id;
}

bool EditorSubscriptions::Unsubscribe(const std::string& id) {
	std::lock_guard<std::mutex> lock(mutex_);
	return subs_.erase(id) > 0;
}

bool EditorSubscriptions::IsSubscribed(const std::string& eventType) const {
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& [id, set] : subs_) {
		(void)id;
		if (set.count(std::string{}) != 0 || set.count(eventType) != 0) {
			return true;
		}
	}
	return false;
}

std::size_t EditorSubscriptions::Count() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return subs_.size();
}

}    // namespace bpr::tools
