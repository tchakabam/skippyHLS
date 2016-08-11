#pragma once
#include <map>
#include <string>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <chrono>

using std::chrono::nanoseconds;

// Child item info
struct M3UItem
{
    std::string url, keyUri, comment;
    //size_t rangeStart, rangeEnd; // not supported yet
    size_t index{0};
    nanoseconds start, end;
    nanoseconds duration;
    uint8_t iv[16];
    bool encrypted{false};
    friend std::ostream& operator<< (std::ostream& stream, const M3UItem& item) {
        return stream << "M3UItem -> Duration: " << item.duration.count()
        << " / Start: " << item.start.count() << " / End: " << item.end.count()
        << " / URL: " << item.url
        << " / Comment (optional): " << (item.comment.empty() ? "none" : item.comment);
    }
};

class M3UPlaylist
{
    using M3UPlaylistItems = std::map<nanoseconds, M3UItem>;
    
public:
    
    uint64_t version{0};
    uint64_t programId{0};
    uint64_t sequenceNo{0};
    uint64_t bandwidthKbps{0}; // kbps
    nanoseconds targetDuration{0}, totalDuration{0};
    
    std::string codec;
    std::string resolution;
    std::string uri;
    std::string type;
    
    std::string encryptionMethod;
    std::string keyUri;
    
    bool isComplete{false};
    
    M3UPlaylist() : version(0), programId(0), sequenceNo(0),
    bandwidthKbps(0), targetDuration(nanoseconds(0)),
    totalDuration(nanoseconds(0)) {
        
    }
    
    
    void addItem(M3UItem& item) {
        item.index = items.size();
        items.insert(std::make_pair(totalDuration, item));
        totalDuration += item.duration;
    }
    M3UItem itemAtTime(nanoseconds nanosecond) const {
        return std::prev(items.upper_bound(nanosecond))->second;
    }
    M3UItem itemAtIndex(size_t index) const {
        auto it = items.cbegin();
        std::advance(it, index);
        return it->second;
    }
    M3UItem operator[](size_t index) const {
        return itemAtIndex(index);
    }
    size_t size() const {
        return items.size();
    }
    M3UPlaylistItems::const_iterator begin() const {
        return items.cbegin();
    }
    M3UPlaylistItems::const_iterator end() const {
        return items.cend();
    }
    
    void reset() {
        version = programId = sequenceNo = bandwidthKbps = 0;
        targetDuration = totalDuration = nanoseconds(0);
        codec = resolution = uri = type = "";
        items.clear();
    }
    
private:
    M3UPlaylistItems items;
};
