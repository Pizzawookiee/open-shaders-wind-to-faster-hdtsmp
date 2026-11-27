#pragma once
// Event-driven geometry refresh pacing. IDs only: no scene or Bullet pointers.
#include <cstdint>
#include <unordered_map>
namespace osfsmp {
class GeometryWake {
    struct Window { float remaining{2.0f}; float untilScan{}; };
    std::unordered_map<std::uint32_t, Window> windows_;
public:
    void note(std::uint32_t reference) { if(reference) windows_[reference]=Window{}; }
    void advance(float dt) {
        if(!(dt>0.f))return;
        for(auto it=windows_.begin();it!=windows_.end();){
            it->second.remaining-=dt;
            it->second.untilScan-=dt;
            // Expire bounded retry windows; topology changes and the periodic
            // fallback cover references that have not acquired FSMP systems yet.
            if(it->second.remaining<=0.f)it=windows_.erase(it);
            else ++it;
        }
    }
    bool due(std::uint32_t reference) const {
        const auto it=windows_.find(reference);
        return it!=windows_.end() && it->second.untilScan<=0.f;
    }
    void scanned(std::uint32_t reference) {
        if(auto it=windows_.find(reference);it!=windows_.end())it->second.untilScan=0.5f;
    }
    void clear() { windows_.clear(); }
    bool empty() const { return windows_.empty(); }
};
} // namespace osfsmp
