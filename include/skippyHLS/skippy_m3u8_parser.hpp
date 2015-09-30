/*
 * SkippyM3UParser.hpp
 *
 *  Created on: May 25, 2011
 *      Author: Stephan Hesse
 *
 */

#pragma once

#include <string>
#include <vector>

// Child item info
struct SkippyM3UItem
 {
  std::string url, keyUri;
  //size_t rangeStart, rangeEnd; // not supported yet
  uint64_t index;
  uint64_t start, end, duration; // Nanoseconds
  uint8_t iv[16];
  bool encrypted;
};

typedef std::vector<SkippyM3UItem> SkippyM3UPlaylistItems;

struct SkippyM3UPlaylist
{
  SkippyM3UPlaylist(std::string uri)
  : version(0), programId(0), sequenceNo(0), bandwidthKbps(0), targetDuration(0), totalDuration(0), uri(uri), isComplete(false)
  {}

  uint64_t version;
  uint64_t programId;
  uint64_t sequenceNo;
  uint64_t bandwidthKbps; // kbps
  uint64_t targetDuration, totalDuration; // Nanoseconds

  std::string codec;
  std::string resolution;
  std::string uri;
  std::string type;
  bool isComplete;

  SkippyM3UPlaylistItems items;
};

typedef std::vector<SkippyM3UPlaylist> SkippyM3UMasterPlaylistItems;

struct SkippyM3UMasterPlaylist
{
  SkippyM3UMasterPlaylist(std::string uri)
  :uri(uri)
  {}

  std::string uri;
  SkippyM3UMasterPlaylistItems items;
};

class SkippyM3UParser
{
public:
  enum State { STATE_META_LINE, STATE_URL_LINE, STATE_RESET};

  enum SubState { SUBSTATE_STREAM,
						  SUBSTATE_INF,
              SUBSTATE_END,
						  SUBSTATE_RESET,
              };

	SkippyM3UParser();

	SkippyM3UPlaylist parse(std::string uri, const std::string& playlist);

protected:
  void readLine();
  void evalState();
  void evalSubstate();
  void update(SkippyM3UPlaylist& playlist);
  void metaTokenize();
  bool nextToken();
  uint64_t tokenToUnsignedInt();

private:
  // Parsing state
  State state;
  SubState subState;

  uint64_t version;
  uint64_t mediaSequenceNo;
  uint64_t targetDuration;
  std::string playlistType;

  // Line buffer
  std::string line;
  std::string token;
  std::vector<std::string> tokens;
  std::vector<std::string>::iterator tokenIt;

  // Stream sub-state vars
  uint64_t programId;
  uint64_t bandwidth;
  std::string res;
  std::string codec;

  // Xinf sub-state vars
  double length;
  uint64_t index;
  uint64_t position;
  
  // URI state vars
  std::string url;
};
