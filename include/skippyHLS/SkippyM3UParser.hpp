#pragma once

#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <vector>
#include <algorithm>
#include <iterator>
#include "skippy_m3uplaylist.h"
#include <ratio>
#include <iostream>
#include "skippy_string_utils.h"

using std::string;
using std::istringstream;
using StringUtils::Tokens;
using StringUtils::split;

using namespace std::placeholders;
#define HANDLER(x) std::bind(&x, this, _1, _2, _3)

class SkippyM3UParser {
    using handler_func = std::function<void(const string&, istringstream &, SkippyM3UPlaylist &)>;
    std::unordered_map<string, handler_func> handlers;
public:
    SkippyM3UParser() {
        handlers["#EXTINF"]               = HANDLER(SkippyM3UParser::onExtInf);
        handlers["#EXT-X-ENDLIST"]        = HANDLER(SkippyM3UParser::onEnd);
        handlers["#EXT-X-VERSION"]        = HANDLER(SkippyM3UParser::onVersion);
        handlers["#EXT-X-PLAYLIST-TYPE"]  = HANDLER(SkippyM3UParser::onType);
        handlers["#EXT-X-TARGETDURATION"] = HANDLER(SkippyM3UParser::onTargetDur);
        handlers["#EXT-X-MEDIA-SEQUENCE"] = HANDLER(SkippyM3UParser::onMediaSeq);
    }

    SkippyM3UPlaylist parse(const string& uri, const string &playlist) {
        SkippyM3UPlaylist list(uri);
        istringstream iss(playlist);
        string line;
        while(getline(iss, line)) {
            for(auto &tag : handlers) {
                if (StringUtils::startsWith(line, tag.first))
                    tag.second(line, iss, list);
            }
        }
        return list;
    }

private:
    void onExtInf(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        SkippyM3UItem item;
        Tokens tok = split(line, ":,");
        double duration = 0.0;
        istringstream(tok[1]) >> duration;

        item.duration   = nanoseconds(static_cast<nanoseconds::rep>(duration * std::nano::den));
        item.start      = playlist.totalDuration;
        item.end        = item.start + item.duration;

        if (tok.size() > 2) 
          item.comment = string(line.begin() + line.find(',') + 1, line.end());

        getline(stream, item.url);
        
        playlist.addItem(item);
    }
    void onEnd(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        playlist.isComplete = true;
    }
    void onVersion(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        Tokens tok = split(line, ":");
        istringstream(tok[1]) >> playlist.version;
    }
    void onType(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        Tokens tok = split(line, ":");
        istringstream(tok[1]) >> playlist.type;
    }
    void onTargetDur(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        Tokens tok = split(line, ":");
        double duration = 0.0;
        istringstream(tok[1]) >> duration;
        playlist.targetDuration = nanoseconds(static_cast<nanoseconds::rep>(duration * std::nano::den));
    }
    void onMediaSeq(const string &line, istringstream &stream, SkippyM3UPlaylist &playlist) {
        Tokens tok = split(line, ":");
        istringstream(tok[1]) >> playlist.sequenceNo;
    }
};
