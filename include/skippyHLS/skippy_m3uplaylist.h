#pragma once
#include <map>
#include <string>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <chrono>

using std::chrono::nanoseconds;

// Child item info
struct SkippyM3UItem
 {
  std::string url, keyUri, comment;
  //size_t rangeStart, rangeEnd; // not supported yet
  size_t index;
  nanoseconds start, end;
  nanoseconds duration;
  uint8_t iv[16];
  bool encrypted;
  friend std::ostream& operator<< (std::ostream& stream, const SkippyM3UItem& item) {
        return stream << "SkippyM3UItem -> Duration: " << item.duration.count()
                      << " / Start: " << item.start.count() << " / End: " << item.end.count()
                      << " / URL: " << item.url 
                      << " / Comment (optional): " << (item.comment.empty() ? "none" : item.comment);
  }
};

class SkippyM3UPlaylist
{
  using SkippyM3UPlaylistItems = std::map<nanoseconds, SkippyM3UItem>;

  public:
    SkippyM3UPlaylist(std::string uri)
    :uri(uri), isComplete(false)
    {}

    uint64_t version;
    uint64_t programId;
    uint64_t sequenceNo;
    uint64_t bandwidthKbps; // kbps
    nanoseconds targetDuration, totalDuration;

    std::string codec;
    std::string resolution;
    std::string uri;
    std::string type;

    bool isComplete;

    void addItem(SkippyM3UItem& item) {
      totalDuration += item.duration;
      item.index = items.size();
      items.insert(std::make_pair(totalDuration, item));
    }
    const SkippyM3UItem& itemAtTime(nanoseconds nanosecond) const {
      return std::prev(items.upper_bound(nanosecond))->second;
    }
    const SkippyM3UItem& itemAtIndex(size_t index) const {
      auto it = items.cbegin();
      std::advance(it, index);
      return it->second;
    }
    const SkippyM3UItem& operator[](size_t index) const {
      return itemAtIndex(index);
    }
    size_t size() const {
      return items.size();
    }
    SkippyM3UPlaylistItems::const_iterator begin() const {
      return items.cbegin();
    }
    SkippyM3UPlaylistItems::const_iterator end() const {
      return items.cend();
    }

  private:
    SkippyM3UPlaylistItems items;
};
