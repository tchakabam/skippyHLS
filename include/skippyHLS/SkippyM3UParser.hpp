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
  unsigned int index;
  unsigned long start, end, duration; // Nanoseconds
  unsigned char iv[16];
  bool encrypted;
};

typedef std::vector<SkippyM3UItem> SkippyM3UPlaylistItems;

struct SkippyM3UPlaylist
{
  SkippyM3UPlaylist(std::string uri)
  :uri(uri)
  {}

  unsigned int version;
  unsigned int programId;
  unsigned int sequenceNo;
  unsigned int bandwidthKbps; // kbps
  unsigned long targetDuration, totalDuration; // Nanoseconds

  std::string codec;
  std::string resolution;
  std::string uri;
  std::string type;

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
  unsigned int tokenToUnsignedInt();

private:
  // Parsing state
  State state;
  SubState subState;

  unsigned int version;
  unsigned int mediaSequenceNo;
  unsigned int targetDuration;
  std::string playlistType;

  // Line buffer
  std::string line;
  std::string token;
  std::vector<std::string> tokens;
  std::vector<std::string>::iterator tokenIt;

  // Stream sub-state vars
  unsigned int programId;
  unsigned int bandwidth;
  std::string res;
  std::string codec;

  // Xinf sub-state vars
  float length;
  unsigned int index;
  unsigned long position;

  // URI state vars
  std::string url;
};
