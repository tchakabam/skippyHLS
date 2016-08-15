//
// Created by Alexander Lenhardt on 5/25/16.
//

#pragma once

#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <vector>
#include <algorithm>
#include <iterator>
#include <ratio>
#include <iostream>

#include "string_utils.hpp"
#include "skippy_m3u8_playlist.hpp"

namespace ph = std::placeholders;
#define HANDLER(x) std::bind(&x, this, ph::_1, ph::_2, ph::_3)

namespace parser {
    
    using std::string;
    using std::istringstream;
    using StringUtils::split;
    
    using namespace std::placeholders;
    
    class M3U8 {
        using handler_func = std::function<void(const string &, istringstream &, M3UPlaylist &)>;
        std::unordered_map<string, handler_func> handlers;
    public:
        M3U8() {
            handlers["#EXTINF"] = HANDLER(parser::M3U8::onExtInf);
            handlers["#EXT-X-ENDLIST"] = HANDLER(parser::M3U8::onEnd);
            handlers["#EXT-X-VERSION"] = HANDLER(parser::M3U8::onVersion);
            handlers["#EXT-X-PLAYLIST-TYPE"] = HANDLER(parser::M3U8::onType);
            handlers["#EXT-X-TARGETDURATION"] = HANDLER(parser::M3U8::onTargetDur);
            handlers["#EXT-X-MEDIA-SEQUENCE"] = HANDLER(parser::M3U8::onMediaSeq);
            handlers["#EXT-X-KEY"] = HANDLER(parser::M3U8::onKey);
        }
        
        void parse(const string &uri, const string &playlist, M3UPlaylist& list) {
            list.uri = uri;
            istringstream iss(playlist);
            string line;
            while (getline(iss, line)) {
                for (auto &tag : handlers) {
                    if (StringUtils::startsWith(line, tag.first))
                        tag.second(line, iss, list);
                }
            }
            //return list;
        }
        
    private:
        using Tokens = std::vector<std::string>;
        void onExtInf(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            M3UItem item;
            Tokens tok = split(line, ":,");
            double duration = 0.0;
            istringstream(tok[1]) >> duration;
            item.duration = nanoseconds(static_cast<nanoseconds::rep>(duration * std::nano::den));
            item.start = playlist.totalDuration;
            item.end = item.start + item.duration;
            
            if (tok.size() > 2)
                item.comment = string(line.begin() + line.find(',') + 1, line.end());
            
            getline(stream, item.url);
            
            playlist.addItem(item);
        }
        
        void onEnd(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            playlist.isComplete = true;
        }
        
        void onVersion(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            Tokens tok = split(line, ":");
            istringstream(tok[1]) >> playlist.version;
        }
        
        void onType(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            Tokens tok = split(line, ":");
            istringstream(tok[1]) >> playlist.type;
        }
        
        void onTargetDur(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            Tokens tok = split(line, ":");
            double duration = 0.0;
            istringstream(tok[1]) >> duration;
            playlist.targetDuration = nanoseconds(static_cast<nanoseconds::rep>(duration * std::nano::den));
        }
        
        void onMediaSeq(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            Tokens tok = split(line, ":");
            istringstream(tok[1]) >> playlist.sequenceNo;
        }
        
        // #EXT-X-KEY:METHOD=AES-128,URI="https://priv.example.com/key.php?r=52", IV=0x9c7db8778570d05c3177c349fd9236aa
        //                                                                           \__________ 128bit IV ____________/
        void onKey(const string &line, istringstream &stream, M3UPlaylist &playlist) {
            Tokens tok = split(line, ':', 1);               // #EXT-X-KEY  :   METHOD=AES-128,URI="http://..."
            Tokens tagValue = split(tok[1], ',', 1);        // METHOD=AES-128   ,   URI="http://..."
            Tokens method = split(tagValue[0], '=');        // METHOD   =    AES-128
            istringstream(method[1]) >> playlist.encryptionMethod; //
            Tokens key = split(tagValue[1], '=');           // URI   =   "http://..."
            istringstream(key[1].substr(1, key[1].size()-2)) >> playlist.keyUri; // http://...
        }
    };
    
}