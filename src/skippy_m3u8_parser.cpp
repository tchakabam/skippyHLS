/*
 *
 *  Created on: May 25, 2011
 *      Author: Stephan Hesse <disparat@gmail.com>, <stephan@soundcloud.com>
 */

#define UNIT_SECONDS 1000000000L // 10^9 (Nanoseconds)
#define ENABLE_DEBUG_LOG FALSE

#include <sstream>
#include <cmath>
#include <algorithm>

#include <glib-object.h>

#include "skippyHLS/skippy_m3u8_parser.hpp"

#if ENABLE_DEBUG_LOG
  #define LOG(...) g_message(__VA_ARGS__)
#else
  #define LOG(...)
#endif

using namespace std;

static const string default_delimiters ("# -.,:");
// words
static const string EXT("EXT");
static const string X("X");
static const string INF("INF");
static const string ID("ID");
static const string EXTM3U("EXTM3U");
static const string EXTINF("EXTINF");
static const string PLAYLIST("PLAYLIST");
static const string TYPE("TYPE");
static const string STREAM("STREAM");
static const string PROGRAM("PROGRAM");
static const string VERSION("VERSION");
static const string BANDWIDTH("BANDWIDTH");
static const string RES("READ");
static const string CODEC("CODEC");
static const string VOD("VOD");
static const string EVENT("EVENT");
static const string TARGETDURATION("TARGETDURATION");
static const string MEDIA("MEDIA");
static const string SEQUENCE("SEQUENCE");
static const string ENDLIST("ENDLIST");

typedef vector<string> Tokens;
Tokens custom_split(const string &s, const string &delim = default_delimiters) {
	vector<string> result;
	auto from = begin(s);
	auto endIt = end(s);
	auto is_delim = [&](const char &c) { return delim.find(c) != string::npos; };
	while( (from = find_if_not(from, endIt, is_delim)) != endIt ){
		auto tokEnd = find_if(from, endIt, is_delim);
		result.push_back(string(from, tokEnd));
		from = tokEnd;
	}
	return result;
}

SkippyM3UParser::SkippyM3UParser()
:state (STATE_RESET)
,subState (SUBSTATE_RESET)
// Put default values here
,mediaSequenceNo(0)
,targetDuration(0)
,programId(0)
,bandwidth(0)
,length(0)
,index(0)
,position(0)

{}

SkippyM3UPlaylist SkippyM3UParser::parse(string uri, const string& playlist)
{
  stringstream in(playlist);

  // Output playlist
  SkippyM3UPlaylist outputPlaylist(uri);

  LOG ("Dumping whole M3U8:\n\n\n%s\n\n\n", playlist.c_str());

  while ( getline(in, line) ){
    // evaluate main state of parser
    evalState();

    // Parses the current line into tokens
    // and updates the parser members
    readLine();

    // Updates the output playlist after every line
    update(outputPlaylist);
  }

  return outputPlaylist;
}

void SkippyM3UParser::metaTokenize() {
  tokens = custom_split (line);
  tokenIt = begin(tokens);
}

void SkippyM3UParser::evalState() {

  LOG ("Evaluating line: %s", line.c_str());

  if (line.find("#EXT") == 0) { //check if its a META line and that are not already in META line state

    LOG ("Found meta line");
    state = STATE_META_LINE;

  } else if(state == STATE_META_LINE && subState == SUBSTATE_INF) {

    LOG ("Assuming URL line");
    state = STATE_URL_LINE;

  } else if (state == STATE_URL_LINE) {

    LOG ("Reset");
    state = STATE_RESET;

  } else if ( state == STATE_META_LINE || state == STATE_RESET)  {

  }
}

bool SkippyM3UParser::nextToken() {
  if(tokenIt == tokens.end()) {
    return false;
  }
  token = *tokenIt++;
  return true;
}

void SkippyM3UParser::evalSubstate() {

  // Get the very first token !
  nextToken();

  // Skip these tokens
  if (token == EXT) {
    nextToken();
  }
  if (token == X) {
    nextToken();
  }

  LOG ("Evaluating metaline substate from token: %s", token.c_str());

  if (token == EXTM3U) {

    LOG ("Start of M3U");

  } else if ( token == EXTINF ) {

    subState = SUBSTATE_INF;

    LOG ("Sub-State to: INF");

  } else if ( token == STREAM && nextToken()
      && token == INF) {

    subState = SUBSTATE_STREAM;

    LOG ("Sub-State to: STREAM");

  } else if ( token == MEDIA && nextToken()
      && token == SEQUENCE && nextToken() ) {

    mediaSequenceNo = tokenToUnsignedInt();

    LOG ("Media sequence no: %u", mediaSequenceNo);

  } else if (token == PLAYLIST && nextToken()
      && token == TYPE && nextToken() ) {

    playlistType = token;

    LOG ("Playlist type: %s", playlistType.c_str());

  } else if (token == VERSION && nextToken()) {

    version = tokenToUnsignedInt();

    LOG ("Version is: %u", version);
  } else if (token == TARGETDURATION && nextToken()) {

    targetDuration = tokenToUnsignedInt();

    LOG ("Target duration is: %u", targetDuration);
  } else if (token == ENDLIST) {

    LOG("Sub-State to: RESET (end of list)");

  subState = SUBSTATE_END;

  } else {

    LOG("Sub-State to: RESET (unknown token): %s", token.c_str());

    subState = SUBSTATE_RESET;
  }

}

uint64_t SkippyM3UParser::tokenToUnsignedInt()
{
  uint64_t i = 0;
  if (! (istringstream(token) >> i)) {
    LOG ("Failed to parse integer value!");
    state = STATE_RESET;
  }
  return i;
}

void SkippyM3UParser::readLine() {

  switch(state) {
  case STATE_RESET:
    break;
  case STATE_URL_LINE:
    if (line.find_first_of("\r", 0) != string::npos && line.length() >= 2) {
      line = line.substr(0, line.length()-1);
    }
    url = line;
    state = STATE_URL_LINE;
    subState = SUBSTATE_RESET;
    break;
  case STATE_META_LINE:

    // Reset the segment length (duration) field
    length = -1;

    // Tokenize the line
    metaTokenize();

    // Evaluate the substate
    evalSubstate();
// 9.12345
    //iterate over all tokens of this line
    while (nextToken()) {
      switch(subState) {
      case SUBSTATE_INF: {
        // Integer part already parsed?
        if (length == -1) {
          length = (float) tokenToUnsignedInt();
        } else { // Now parse fractional part
          double decimals = tokenToUnsignedInt();
          LOG ("Got decimals: %f (%s)", decimals, token.c_str());
          length += decimals / pow(10, token.length());
        }
        LOG ("Got INF duration: %f", length);
        break;
      }
      case SUBSTATE_STREAM:
        if ( token == PROGRAM && nextToken () && token == ID ) {
         nextToken();
         programId = tokenToUnsignedInt();
        }
        else if ( token == CODEC ) {
         nextToken();
         codec = token;
        }
        else if ( token == RES) {
         nextToken();
         res = token;
        }
        else if ( token == BANDWIDTH) {
         nextToken();
         bandwidth = tokenToUnsignedInt();
        }
        break;
      case SUBSTATE_RESET:
      default:
        break;
      }
    }
      break;
  }
}

void SkippyM3UParser::update(SkippyM3UPlaylist& playlist) {

  switch(state) {
  case STATE_RESET:
    break;
  case STATE_URL_LINE: {
    SkippyM3UItem item;
    item.start = position;
    item.duration = length * UNIT_SECONDS;
    item.end = item.start + item.duration;
    item.url = url;
    item.encrypted = false;
    item.index = index;

    playlist.items.push_back( item );

    position += item.duration;
    index++;

    LOG ("Added item: %s", url.c_str());
    break;
  }
  case STATE_META_LINE:
    switch (subState) {
    case SUBSTATE_END:
      playlist.bandwidthKbps = bandwidth; //kbps
      playlist.codec = codec;
      playlist.resolution = res;
      playlist.programId = programId;
      playlist.sequenceNo = mediaSequenceNo;
      playlist.targetDuration = targetDuration * UNIT_SECONDS;
      playlist.totalDuration = position;
      playlist.type = playlistType;
      break;
    default:
      break;
    }
    break;
  }
}